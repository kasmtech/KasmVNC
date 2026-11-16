# CLAUDE.md — KasmVNC

Guidance for AI agents working in this repository. Read this before making
changes. It describes how KasmVNC works **today**, not how the upstream project
it forked from once worked.

> `AGENTS.md` is a symlink to this file.

## What KasmVNC is (and isn't)

KasmVNC is a **web-native remote desktop server**. It was forked from TigerVNC
years ago and has diverged heavily since. Treat it as its own project, not a
TigerVNC variant.

- **Browser-only.** Users connect from any modern browser. There is **no
  supported native/thick VNC client**. Do not add or restore features whose only
  purpose is compatibility with standalone VNC viewers (RealVNC, TigerVNC viewer,
  TightVNC, etc.).
- **Intentionally diverges from the RFB spec.** "VNC" is in the name and the RFB
  wire protocol is the ancestor, but KasmVNC deliberately breaks RFB to support
  modern transport, encodings, and security. **Do not "fix" code toward RFB-spec
  compliance** — divergence is intended. (A detailed list of the deviations is
  not yet documented here; see `PLAN.md`. When in doubt, ask rather than
  reverting to standard VNC behavior.)
- **Modern config.** Server and per-user configuration is YAML-based (see
  Configuration below), not the classic VNC flag-only model.

## Repository layout

| Path | What it is |
|------|------------|
| `common/` | Core C++ libraries shared by the server. `rfb/` (protocol, encoding, security), `network/`, `rdr/` (readers/writers), `os/`, `Xregion/`. |
| `common/rfb/encoders/`, `common/rfb/ffmpeg.*` | **Modern** web encoding path: software + hardware video encoders (FFMPEG / VAAPI / H.264). This is the live encoding code. |
| `unix/xserver/hw/vnc/` | The Xvnc server itself — the Xorg DDX integrating KasmVNC into an X server. Produces the `Xvnc` / `Xkasmvnc` binary. |
| `unix/` | The `vncserver` Perl wrapper, `kasmvncpasswd`, `vncpasswd`, `vncconfig`, `kasmxproxy`, the `KasmVNC/*.pm` config/validation modules, the systemd unit, and default YAML. |
| `kasmweb/` | Web frontend — a fork of noVNC (git **submodule**). Vite build. This is the client. |
| `builder/` | Docker-based build & packaging for every supported distro. Canonical build path lives here. |
| `tests/`, `spec/`, `kasmvnc-functional-tests/` | Three distinct test layers (see Testing). |
| `debian/`, `fedora/`, `alpine/`, `opensuse/`, `oracle/` | Distro packaging metadata. |
| `win/` | **Unsupported Windows build. Do not invest effort here.** |

### Submodules

```bash
git submodule init
git submodule update --remote --merge
```

- `kasmweb` → the noVNC fork (public).
- `kasmvnc-functional-tests` → **private** GitLab repo; external agents may not
  have access.

## Building (Docker dev container — canonical)

The supported developer workflow is the Docker dev container. You run the
`docker build`/`docker run` from the **host** (the repo checkout, e.g.
`/home/ubuntu/KasmVNC`); the container bind-mounts it at `/src`, so every
`/src/...` path below refers to this same repo from inside the container. Full
detail is in `BUILDING.md`; the essentials:

```bash
# Build & enter the dev container (assumes host UID 1000).
# Prefix with sudo only if your user is NOT in the `docker` group.
docker build -t kasmvnc:dev -f builder/dockerfile.ubuntu_jammy.dev .
docker run -it --rm -v ./:/src -p 2222:22 -p 6901:6901 -p 8443:8443 \
  --group-add video --group-add render --name kasmvnc_dev kasmvnc:dev
```

> **GPU `--device` flags are optional and host-specific — detect, don't assume.**
> They give the container access to the host GPU for *runtime* hardware H.264
> (VAAPI); they are **not** needed to build. Before adding them, run `ls /dev/dri`
> on the host to see what exists. If there are render nodes, append the ones you
> actually find — `--device=/dev/dri/cardN --device=/dev/dri/renderDNNN` with the
> real numbers (they vary by machine; do not assume `card0`). **If `/dev/dri` is
> absent or empty, omit both `--device` flags entirely** — the build still works
> and the server falls back to software encoding at runtime.

Inside the container:

```bash
# 1. Frontend (only needed when web code changes)
cd kasmweb && npm install && npm run build && cd ..

# 2. Server. build.sh downloads/patches the target Xorg source, builds the
#    common libs + Xvnc, and links it into ./xorg.build/
#    --no-servertarball skips the final packaging step (see note below).
builder/build.sh --no-servertarball
```

**Automated / non-interactive build** (no TTY — for scripts or agents): drop
`-it`, don't `--rm` if you want to keep the container, and pass the steps to
`bash -lc` so the run returns when the build finishes:

```bash
docker run --rm -v ./:/src --entrypoint bash \
  --name kasmvnc_dev kasmvnc:dev \
  -lc 'cd /src/kasmweb && npm install && npm run build && cd /src && builder/build.sh --no-servertarball'
```

> **Keep build/test commands auto-approvable — don't make the user click through
> needless prompts.** The build/test entry points used in this doc are
> pre-approved in `.claude/settings.json` (`build.sh`, `build-package`,
> `build-tarball`/`-deb`/`-rpm`, `test-vncserver`, `docker build`/`run`/`exec`,
> `npm`, `cmake`, `make`) along with the read-only inspection tools (`grep`,
> `ls`, `find`, `cat`, `head`, `ldd`, `nm`, `dpkg -c`/`-l`, `dpkg-deb`). A Bash
> call is auto-approved only when **every** segment of a `;` / `&&` / `||` / `|`
> chain is covered, so one unlisted piece makes the whole line prompt — even the
> allowlisted parts next to it. Therefore:
>
> - **Issue each build/test/inspection command on its own.** Don't staple a
>   read-only `grep`/`ls`/`find` (or an allowlisted `dpkg-deb`) onto a command
>   that writes or substitutes — the whole line prompts.
> - **Never use `$(…)` command substitution or `VAR=…` shell assignments**
>   (`tmp=$(mktemp -d)`, `bin=$(find … )`). These *cannot* be allowlisted and
>   *always* prompt. Use a fixed scratch path instead — e.g. `mkdir -p
>   /tmp/kasmwork`, then reference `/tmp/kasmwork` directly across separate
>   commands — rather than capturing output into a variable.
> - **Let the harness capture build/test output** (and use a background run for
>   long builds/suites) instead of piping to `tee`/a logfile.
> - When inspecting a built package, run the steps as plain commands against a
>   fixed extraction dir (`dpkg-deb -x pkg.deb /tmp/kasmwork`, then `ldd
>   /tmp/kasmwork/usr/bin/Xkasmvnc`) — no `$(mktemp)`/`rm -rf "$tmp"` scaffolding.

> **If your `kasmvnc:dev` image predates the dev-Dockerfile fixes**, the command
> above fails in one of several ways (build exits 0 but produces nothing;
> `CC: sccache gcc` not found; `Permission denied` installing libjpeg to
> `/usr/local`; `Operation not permitted` on `/tmp/libwebp-*`). Rebuild the image
> (`docker build -t kasmvnc:dev -f builder/dockerfile.ubuntu_jammy.dev .`), or use
> this self-contained interim command:
>
> ```bash
> docker run --rm -v ./:/src -e SCRIPTS_DIR=/src/builder/scripts --entrypoint bash \
>   kasmvnc:dev -lc 'sudo find /tmp -mindepth 1 -maxdepth 1 ! -user kasm-user -exec rm -rf {} + ;
>     sudo /src/builder/scripts/install_sccache_from_github &&
>     cd /src/kasmweb && npm install && npm run build &&
>     cd /src && builder/build.sh --no-servertarball'
> ```

> **`builder/build.sh` ends with `make servertarball`**, which `cp`s `builder/www`
> (produced separately by `builder/build-www`). A dev build that only needs the
> `Xvnc` binary + `kasmweb/dist` should pass **`--no-servertarball`** to skip it
> (used above). Without the flag and without `builder/www`, the build fails *only
> at that final packaging step* — the binary and frontend are already complete.
> Packaging pipelines invoke `build.sh` with no args (tarball **on**, as intended,
> via `builder/build-www` first); never pass `--no-servertarball` there.

What `builder/build.sh` does, in brief: runs CMake with `BUILD_VIEWER=OFF` and
`ENABLE_GNUTLS=OFF`, builds the C++ side (C++20, OpenMP), then downloads an Xorg
server tarball (`XORG_VER`, default 1.19.6), applies the matching
`unix/xserver*.patch` + CVE patches, configures Xorg with most subsystems
disabled, and builds Xvnc. The runnable server ends up at
`/src/xorg.build/bin/Xvnc`.

> **`build.sh` builds in-source — clean before a dependency or flag change.** It
> runs `cmake .` at the repo root and leaves generated state in the tree
> (`config.h`, `CMakeCache.txt`, scattered `CMakeFiles/`, all git-ignored). CMake
> **caches** its library/feature detection, so after you change a build
> dependency (e.g. add/remove a `-dev` package) or a CMake flag, an incremental
> rebuild will **not** re-detect it — you'll silently keep building against the
> old config. Before such a build, check for leftover state and force a fresh
> configure (inside the container, from `/src`):
>
> ```bash
> ls config.h CMakeCache.txt 2>/dev/null   # any output ⇒ a prior in-source build exists
> rm -f config.h CMakeCache.txt            # force re-detection; keeps the costly xorg.build/
> ```
>
> Then confirm the change took in the regenerated `config.h` — a disabled feature
> shows `/* #undef HAVE_FOO */`, an enabled one `#define HAVE_FOO`. For a
> guaranteed-pristine tree use `git clean -xfd`, but note it also wipes
> `xorg.build/`, the downloaded Xorg tarball, and `kasmweb/node_modules` (full
> re-download + ~20-min Xorg rebuild), so prefer the targeted `rm` above unless
> you truly need a from-scratch build.
>
> This in-source habit also matters for **packaging**: `builder/dockerfile.*.build`
> does `COPY . /src`, so a stale `CMakeCache.txt` from a prior dev build would be
> copied into the packaging image and poison its fresh configure (e.g. a cache
> that recorded a now-absent dependency). `.dockerignore` excludes these generated
> CMake artifacts to prevent that — keep it that way, and clean (above) if a
> package build ever configures against the wrong settings.

**Verify the build** (quick smoke check). Inside an interactive container shell:

```bash
/src/xorg.build/bin/Xvnc -version   # prints "Xvnc KasmVNC <commit> - built ..."
```

> **Agents driving the build from the host: don't wrap the smoke check in
> `bash -lc '…'`.** The permission matcher *descends into the `bash -lc` string*
> and checks each `;`/`&&`/`|` segment, so `docker run … bash -lc
> '/src/xorg.build/bin/Xvnc -version'` is matched by its inner command — and
> things like a bare `Xvnc` path, `sleep`, `curl`, `kill`, or a `VAR=…`
> assignment inside that string each prompt even though `docker run*` is
> allowlisted. Run the binary **directly as the container entrypoint** instead,
> so the matcher only sees `docker run*` with no inner command to descend into:
>
> ```bash
> docker run --rm -v ./:/src --entrypoint /src/xorg.build/bin/Xvnc \
>   kasmvnc:dev -version
> ```

For an end-to-end smoke test, start a headless session without hardware codecs
(works even with no GPU) and confirm the web endpoint serves. From the host,
run the server as a **detached** container (entrypoint = the binary, no
`bash -lc`), poll the mapped port with a **URL-first** `curl` (the allowlist
scopes auto-approved `curl` to `http://localhost:…`/`http://127.0.0.1:…`, and
curl accepts the URL before its flags), then stop it with `docker kill` — every
command here is already auto-approved:

```bash
docker run -d --rm --name kasmvnc_smoke -v ./:/src -p 6901:6901 \
  --entrypoint /src/xorg.build/bin/Xvnc kasmvnc:dev \
  -interface 0.0.0.0 -disableBasicAuth -SecurityTypes None \
  -httpd /src/kasmweb/dist -websocketPort 6901 -sslOnly 0 -FreeKeyMappings :1
curl http://localhost:6901/ -fsS --retry 10 --retry-connrefused -o /dev/null && echo "web endpoint OK"
docker kill kasmvnc_smoke
```

Inside an interactive container shell you can instead use the simpler
backgrounded form (`Xvnc … & … curl … ; kill %1`); it's only the host-driven,
auto-approval path that needs the detached-container shape above.

> **Don't hand-roll `until …; sleep; done` wait loops to poll a background
> build/test.** They're not auto-approvable (`sleep`, `[`, `if`, `break` aren't
> allowlisted) *and* they're unnecessary: a `run_in_background` Bash command
> re-invokes the agent on completion. Start the long step in the background and
> wait for the completion notification; use `curl --retry` (above) rather than a
> sleep loop when you genuinely need to wait for a port to come up.

### Default hand-off: build, smoke-test, and leave a running server

**This is the default behavior when a developer asks you to build / rebuild /
"build and run" (anything that produces a new `Xvnc`).** You are working
*alongside* a developer who will do the real interactive testing in their
browser. So unless they tell you otherwise, after a successful build:

1. Run the **build smoke check** above (`Xvnc -version` + the `curl` endpoint
   check) to confirm the binary is healthy. This is the "quick test" — *not* the
   heavier `spec`/functional suites, which stay opt-in (run them only when asked
   or when the change clearly warrants them).
2. **Leave KasmVNC running** in a detached container so the developer can connect
   immediately, and
3. **End your message with the connection details** (URL + credentials) in the
   block shown below.

Bring it up with the committed entrypoint script
`builder/dev-run-inside-docker` — it generates a self-signed cert, sets the
static dev login, starts the **freshly-built** `/src/xorg.build/bin/Xvnc` over
HTTPS, and launches an xfce4 desktop. Running it as the container *entrypoint*
keeps the whole launch a single auto-approved `docker run` (no `bash -lc`). On a
rebuild, kill the previous session first (the `--rm` container is replaced):

```bash
docker kill kasmvnc_dev          # stop a previous session; harmless if none exists
docker run -d --rm --name kasmvnc_dev -v ./:/src -p 6901:6901 \
  --group-add video --group-add render \
  --entrypoint /src/builder/dev-run-inside-docker kasmvnc:dev
curl -k https://127.0.0.1:6901/ -sS -u kasm_user:kasmRulez --retry 15 \
  --retry-connrefused -o /dev/null -w "web endpoint: %{http_code}\n"   # expect 200
```

Then close your message with exactly this hand-off block (the login is a
**static dev-only** credential baked into the script; the browser will warn on
the self-signed cert — that's expected):

```
KasmVNC compiled and passed basic testing and is currently running for you:
URL: https://127.0.0.1:6901
Username: kasm_user
Password: kasmRulez
```

> The web login (`kasm_user` / `kasmRulez`, set via `kasmvncpasswd` into
> `~/.kasmpasswd`) is independent of the container's OS user (`kasm-user`) and of
> the RFB `SecurityTypes` — BasicAuth is the only gate, so an unauthenticated
> request returns `401`. To add GPU H.264 at runtime, append the host's real
> `--device=/dev/dri/...` nodes to the `docker run` (see the GPU note under
> Building); it still runs without them (software encoding).

### Packaging

`builder/build-package <os> <os_codename>` builds a distro package via Docker;
output lands in `builder/build/`. See `builder/README.md`.

> **The packaged server binary is `/usr/bin/Xkasmvnc`, not `Xvnc`.** The dev-tree
> build links it as `Xvnc` (`/src/xorg.build/bin/Xvnc`), but the installed package
> ships it as `Xkasmvnc`. When inspecting a built `.deb`/RPM (`dpkg-deb -c`,
> `ldd`, etc.), look for `Xkasmvnc`.

## Running locally (inside the dev container)

```bash
mkdir -p ~/.vnc
openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
  -keyout ~/.vnc/self.pem -out ~/.vnc/self.pem \
  -subj "/C=US/ST=VA/L=None/O=None/OU=DoFu/CN=kasm/emailAddress=none@none.none"

/src/xorg.build/bin/Xvnc -interface 0.0.0.0 -PublicIP 127.0.0.1 \
  -disableBasicAuth -RectThreads 0 -Log *:stdout:100 \
  -httpd /src/kasmweb/dist -sslOnly 1 -SecurityTypes None \
  -websocketPort 6901 -FreeKeyMappings -cert ~/.vnc/self.pem -key ~/.vnc/self.pem \
  -videoCodec h264 :1 &
/usr/bin/xfce4-session --display :1
```

Then browse to the dev host on port **6901**.

**Frontend inner loop** (live-rebuild UI without packaging): run Xvnc with
`-sslOnly 0`, start `sudo nginx`, then in `kasmweb/` run `npm run serve`
(foreground). nginx (8443, TLS) proxies the websocket to Xvnc and everything
else to the Vite dev server. Browse to port **8443**. See `BUILDING.md`.

In production the `vncserver` Perl wrapper starts/manages sessions
(`vncserver`, `vncserver -list`, `vncserver -kill :N`).

## Testing

### Required unit tests before merge requests

Run `builder/test-unit` and `builder/test-unit sanitizers` locally before creating
a merge request. Both use the dedicated unit container, with no package or running
Xvnc required. Record the commands/results; report blockers and failures honestly.
Add unit tests for new behavior, including failure and boundary cases. For each bug
fix, assess whether a unit test could have caught it; if so add a regression and
verify failure before and success after the fix. Explain integration/E2E-only cases
and cover them in the appropriate suite. Do not replace unit tests with a build
smoke check. The runtime spec and functional suites retain their separate roles.

Tests live in `kasmvnc-unit-tests`, a pinned Git submodule. Changes spanning source
and tests require validating the exact pair, committing/pushing the test revision
first, then updating the KasmVNC gitlink. Do not use `--remote` to initialize tests.
See its README for filters, standalone source selection, coverage and reports.
Native C++ launcher configuration is tested directly; legacy Perl is not part of
this unit suite. Frontend browser-component tests run against the pinned kasmweb
source without a KasmVNC server. Tests must exercise current Kasm interfaces.

> **Run every check inside a container — never on the host.** The build and its
> smoke check run inside the `kasmvnc:dev` container; each test suite runs inside
> its **own** dedicated container that provisions that suite's dependencies
> (`pipenv`, `mamba`, desktop environments, an installed `.deb`, browsers). Do
> **not** install test deps (`pipenv`/`mamba`/the `Pipfile` packages) on the host
> or in the `kasmvnc:dev` build container and run `./run-specs` there — that
> container has none of the runtime prerequisites (no installed `vncserver`, no
> desktop environments) and the run will hang or fail. Use the entry-point
> scripts below; they build and drive the correct container for you.

The fastest confirmation that a build is healthy is the **build smoke check**
above (`Xvnc -version` + the `curl` endpoint check), run **inside the dev
container**. The suites below are heavier and slower.

Three independent layers:

1. **`tests/`** — C++ micro/perf benchmarks built only with `-DTESTS=ON`.
   Largely **dead**: several (`decperf`, `encperf`, `fbperf`) reference the
   removed `vncviewer/` tree and the dead client/decoder code (see Legacy). Not
   part of the normal workflow.
2. **`spec/`** — Python behavioral specs (mamba) for the `vncserver` wrapper:
   YAML→CLI translation, env-var→CLI, validation, `-select-de`, etc. **Run via
   `builder/test-vncserver`** (the canonical entry point; CI's `spec_test` job
   uses it). It builds the dedicated
   `builder/dockerfile.<os>_<codename>.specs.test` image — which installs the
   built `.deb`, `pipenv`, the desktop environments the `-select-de` specs need,
   and fonts — then runs `builder/run-specs-inside-docker` (`pipenv install &&
   ./run-specs`) inside it. **Prerequisite: a built `.deb`** in
   `builder/build/<codename>/` (local) or `output/<codename>/` (CI); produce one
   first with `builder/build-package ubuntu jammy`. `./run-specs` and
   `pipenv run mamba` are the *inner* commands that script runs **inside that
   container** — do not invoke them on the host. `-v` (documentation format) /
   `-d` (debug) are `run-specs` flags.
3. **`kasmvnc-functional-tests/`** — end-to-end Playwright/Selenium tests that
   run a built `.deb` against a Jammy Workspaces container. The runner
   (`functional-test`) and harness live in the **private** submodule and build
   their own container; external agents may not have access. Run with
   `./functional-test` from the submodule (`--debug` for debug output); report in
   `report/`. Requires Docker socket access, the private submodule, and a built
   `.deb`.

CI is GitLab (`.gitlab-ci.yml`). It builds only portable glibc/musl packages on
AMD64/ARM64, using the same full pipeline with or without a merge request. Spec
and required minimal functional tests consume the portable DEB. Standard is
manual and optional; run it successfully on the current commit before requesting
review. It covers the glibc 2.28 floor and newer userspaces. See `BUILDING.md`.

## Security review and vulnerability triage (opt-in)

Start vulnerability investigation, remediation, or triage only when explicitly
requested, for example "address security findings" or "perform a security
review". Do not start a new review for each ordinary development commit or
pipeline. Existing build/CI scans and assessment validation continue normally;
this opt-in rule does not disable them. A review request authorizes investigation,
not automatically remediation, commits, pushes, or GitLab status changes: follow
the scope and authorization already given by the developer.

### Establish the review baseline

1. Inspect the current branch, HEAD, staged/unstaged/untracked changes, and
   submodule revisions. Preserve unfinished work; do not commit it just to obtain
   a scan.
2. Look for an existing GitLab pipeline for the exact HEAD commit. Reuse its
   security artifacts when the relevant scan jobs completed successfully and
   cover the requested architectures/libcs. A scan failure, missing artifact, or
   missing profile is not a clean result. Record the pipeline/job IDs, source
   revision, artifact hashes, scanner version, and database metadata.
3. If local changes affect dependencies, build configuration, shipped code, or
   assessment evidence, the pipeline describes only the committed baseline.
   Build/scan the relevant local changes before making claims about the working
   tree. Record that local evidence includes uncommitted changes.
4. If suitable pipeline results are unavailable, start locally using the canonical
   container workflow; do not create a commit/pipeline merely to begin a review.
   A clean checkout and matching pipeline do not establish database freshness.
   Distinguish exact reproduction with the same database from a fresh scan whose
   advisory data may differ, and explain differences instead of forcing counts
   to match.

### Investigate and record decisions

Read [the portable security workflow](builder/portable/README.md#persistent-security-assessments),
[the review registry](builder/portable/security-assessments.json), and
[the source-backed review](builder/portable/SECURITY-REVIEW.md) before assessing
portable findings. Use upstream advisories, source fixes, build flags, shipped
files, and actual call paths to establish applicability. A version match alone
does not establish exploitability; failure to reproduce alone does not establish
non-applicability. Disabled executables do not prove shared library code is
unreachable. Scope conclusions to the reviewed artifact, architecture, and libc;
embedded-dependency scans do not cover host OS packages or drivers.

| Evidence | Project assessment | GitLab triage, when authorized |
|---|---|---|
| Applicability uncertain or evidence changed | `in_triage` | Needs Triage |
| Vulnerability applies to the shipped build | `exploitable` | Confirmed |
| Vulnerable functionality absent or demonstrably unreachable | `not_affected` | Dismiss with an accurate reason, scope, and evidence |
| Incorrect scanner/advisory match | `false_positive` | Dismiss: False Positive |
| Fix/backport verified in the shipped build | `resolved` | Resolve with fix and validation evidence |
| Finding absent from the current successful scan | `not_detected` lifecycle | Investigate before declaring resolved |

For authorized changes, update the committed registry and review document with
the CVE, exact package/version/platform scope, rationale, reviewer/date, upstream
references, and source/recipe fingerprints. Never broaden scope or refresh hashes
just to keep an exception active. Keep real upstream versions and raw findings;
do not delete matches, lower severity, or invent versions to hide a backport.
Preserve review history: generated snapshots update `active`, `not_detected`,
`needs_review`, and `unreviewed` observations without a CI bot rewriting Git.
Accepting a valid risk is a separate decision requiring explicit developer
authorization, not a false-positive classification.

### Validate locally, then verify publication

Use `builder/build-portable` and `builder/build-portable --libc musl` for the
relevant profiles; these container builds invoke `builder/portable/scan` on the
generated SBOM. Follow their documented version/signing configuration. Run
targeted regression tests for fixes/backports (demonstrate failure before and
success after the patch where practical), appropriate package/runtime tests, and
`builder/portable/test-security-reports` against the current artifact directories.
Keep tests in containers. Do not mix stale builds into the validation inputs.

Verify schemas and agreement across the CycloneDX SBOM, assessment snapshot,
OpenVEX, annotated Grype output, and GitLab report. Exercise the actual Grype VEX
consumer: its remaining matches plus `ignoredMatches` must account for every raw
finding, and only applicable reviewed exceptions may move to `ignoredMatches`.
Test assessment invalidation/disappearance when changing reconciliation logic.
Check installed SBOM/VEX records across affected package formats, including
minimal images that omit documentation by default.

When commit/push/CI monitoring is authorized, verify the final revision's CI
artifacts, required platform coverage, and GitLab's ingested assessment details
and flags. Follow the developer's retry limits; distinguish infrastructure errors
from code failures. Local results can support research and remediation without
waiting for CI, but do not claim untested platforms or GitLab ingestion verified
until their evidence is available. Finish with a finding-by-finding status table,
evidence links, validation results, and remaining limitations. A green pipeline
or fewer raw findings does not mean every vulnerability is resolved.

### Keep GitLab triage consistent with evidence

Scanner false-positive flags and evidence are separate from GitLab's persistent
triage status. UI/API decisions do not update the registry or SBOM. When status
changes are authorized, apply evidence-backed decisions through the approved
platform tooling and record the rationale and reviewed commit. Recheck existing
dismissals when evidence/scope changes or an assessment becomes `needs_review`
or `in_triage`; an old dismissal must not silently outlive its justification.
Reopen stale decisions when authorized, otherwise report the discrepancy and
required action. Do not assume a regenerated report resets a GitLab dismissal.

## Configuration

YAML-based, layered:

- `/etc/kasmvnc/kasmvnc.yaml` — global, applies to all users.
- `~/.vnc/kasmvnc.yaml` — per-user override.

The Perl modules in `unix/KasmVNC/*.pm` parse, validate, and translate YAML (and
env vars) into Xvnc CLI flags. Defaults live in `unix/kasmvnc_defaults.yaml`.
Full setting reference: <https://www.kasmweb.com/kasmvnc/docs/latest/configuration.html>.

Many encoding settings are client-overridable unless
`runtime_configuration.allow_client_to_override_kasm_server_settings: false`.

## Legacy / do-not-touch

This tree carries inheritance from TigerVNC. Avoid spending effort on, and don't
expand, the following — they are unsupported or dead:

- **`win/` — Windows build.** Self-contained directory, built only under
  `if(WIN32)` so it never compiles on Linux. Unsupported. Don't extend it.
  (There are also scattered `WIN32`/`_WIN32` `#ifdef`s in ~22 cross-platform
  `common/` files — likewise dead on Linux.)
- **Native viewer + its client stack.** The TigerVNC viewer GUI has been removed
  (no `vncviewer/` directory), but a fair amount of client-side code is still
  compiled into `librfb` with no live consumer. Treat all of the following as
  dead:
  - The **C-prefix client classes** — `CConnection`, `CMsgReader`/`CMsgWriter`,
    `DecodeManager`, and all `*Decoder.cxx` (`RawDecoder`, `TightDecoder`,
    `HextileDecoder`, …). These *decode* an RFB stream, which only a client
    does; the KasmVNC server never decodes. (The browser does its own decoding
    in `kasmweb/core/decoders/`.)
  - The **`CSecurity*`** classes (client-side auth).
  - **`unix/tx/`** — the FLTK widget toolkit for the old viewer GUI.
  - The **`tests/`** perf binaries (`decperf`, `encperf`, `fbperf`), which
    reference the missing `vncviewer/` and exercise the dead client/decoder code.
  - **`BUILD_VIEWER`** in `builder/build.sh` — a no-op (not referenced by CMake).

  Do not reintroduce a native client. Their only "use" is that they still build.
- **Security types — server vs. client.** `SSecurity*` (server side, selected via
  `-SecurityTypes`) are **live**. `CSecurity*` (client side) are dead (see
  above). Which specific *server* types are actually reachable from the browser
  has not been fully audited (e.g. whether `SSecurityVeNCrypt` is ever
  negotiated by `kasmweb`) — verify before extending one.
- **VNC-client-only flags/options.** Many inherited CLI flags and config options
  assume a native viewer and no longer apply. Don't document or wire them up as
  if they're live without confirming.

> **Not legacy — the RFB encoders are live.** `common/rfb/EncodeManager` actively
> uses the `*Encoder.cxx` classes (Raw, RRE, Hextile, Tight, TightJPEG,
> TightWEBP, TightQOI, ZRLE) **plus** the modern KasmVideo path in
> `common/rfb/encoders/` (software + FFMPEG/VAAPI/H.264). The browser decodes
> these (`kasmweb/core/decoders/`: tight, tightpng, qoi, hextile, rre, raw,
> copyrect, kasmvideo). The server **encoders** are the live path that feeds the
> browser — only the C++ *decoders* above are dead. Target this path for
> encoding work.

When unsure whether code is live or legacy, check whether it's reachable from
the browser client (`kasmweb/`) and the Xvnc server path before changing it.

## Conventions

- **C++:** C++20, formatted per `.clang-format`. Asserts are kept even in
  release builds (`-UNDEBUG`).
- **JS (frontend):** eslint (`npm run lint` in `kasmweb/`).
- **Branches:** `(feature|bugfix)/VNC-123_short_description`, where `VNC-123` is
  the ticket number — e.g. `feature/VNC-123_add_claude_file`,
  `bugfix/VNC-325_hw_h264_nvenc`.
- **Frontend (submodule) changes:** if a task touches the UI, create a branch
  with the **same name** in the `kasmweb` (noVNC) submodule, and point the
  KasmVNC submodule reference at the tip of that branch. Server and frontend
  branches for one ticket share the same name.
- **Commits/PRs:** only commit or push when asked; branch off `master` first.

## Pointers

- `BUILDING.md` — full build, dev-container, CI detail.
- `DEBUGGING.md` — symbolizing crash backtraces with debug-symbol packages.
- `builder/README.md` — packaging.
