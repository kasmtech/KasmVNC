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
  --device=/dev/dri/card0 --device=/dev/dri/renderD128 \
  --group-add video --group-add render --name kasmvnc_dev kasmvnc:dev
```

> **Adjust the `--device` paths to your machine** — they're for runtime hardware
> H.264 (VAAPI) and the card number varies. Run `ls /dev/dri` and substitute the
> actual `cardN`/`renderDNNN` (e.g. this VM has `card1`, not `card0`). If the box
> has no `/dev/dri` GPU, drop both `--device` flags; the build still works and
> software encoding is used at runtime.

Inside the container:

```bash
# 1. Frontend (only needed when web code changes)
cd kasmweb && npm install && npm run build && cd ..

# 2. Server. build.sh downloads/patches the target Xorg source, builds the
#    common libs + Xvnc, and links it into ./xorg.build/
builder/build.sh
```

**Automated / non-interactive build** (no TTY — for scripts or agents): drop
`-it`, don't `--rm` if you want to keep the container, and pass the steps to
`bash -lc` so the run returns when the build finishes:

```bash
docker run --rm -v ./:/src \
  --name kasmvnc_dev kasmvnc:dev \
  bash -lc 'cd /src/kasmweb && npm install && npm run build && cd /src && builder/build.sh'
```

What `builder/build.sh` does, in brief: runs CMake with `BUILD_VIEWER=OFF` and
`ENABLE_GNUTLS=OFF`, builds the C++ side (C++20, OpenMP), then downloads an Xorg
server tarball (`XORG_VER`, default 1.19.6), applies the matching
`unix/xserver*.patch` + CVE patches, configures Xorg with most subsystems
disabled, and builds Xvnc. The runnable server ends up at
`/src/xorg.build/bin/Xvnc`.

**Verify the build** (quick smoke check, inside the container):

```bash
/src/xorg.build/bin/Xvnc -version   # prints "Xvnc KasmVNC <commit> - built ..."
```

For an end-to-end smoke test, start a headless session without hardware codecs
(works even with no GPU) and confirm the web endpoint serves:

```bash
/src/xorg.build/bin/Xvnc -interface 0.0.0.0 -disableBasicAuth -SecurityTypes None \
  -httpd /src/kasmweb/dist -websocketPort 6901 -sslOnly 0 -FreeKeyMappings :1 &
curl -fsS http://localhost:6901/ -o /dev/null && echo "web endpoint OK"
```

### Packaging

`builder/build-package <os> <os_codename>` builds a distro package via Docker;
output lands in `builder/build/`. See `builder/README.md`.

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

The fastest confirmation that a build is healthy is the **build smoke check**
above (`Xvnc -version` + the `curl` endpoint check). The suites below are
heavier, have their own dependencies, and are normally run via their dedicated
Docker images / CI rather than ad hoc in the dev container — check each one's
prerequisites before running.

Three independent layers:

1. **`tests/`** — C++ micro/perf benchmarks built only with `-DTESTS=ON`.
   Largely **dead**: several (`decperf`, `encperf`, `fbperf`) reference the
   removed `vncviewer/` tree and the dead client/decoder code (see Legacy). Not
   part of the normal workflow.
2. **`spec/`** — Python behavioral specs (mamba) for the `vncserver` wrapper:
   YAML→CLI translation, env-var→CLI, validation, `-select-de`, etc.
   Run with `./run-specs` (uses `pipenv run mamba`; deps in `Pipfile`). Add
   `-v` for documentation format, `-d` for debug output.
3. **`kasmvnc-functional-tests/`** — end-to-end Playwright/Selenium tests that
   run a built `.deb` against a Jammy Workspaces container. Run with
   `./functional-test` (`--debug` for debug output); report in `report/`.
   Requires Docker socket access and the private submodule.

CI is GitLab (`.gitlab-ci.yml`). `BUILD_DISTROS_REGEX` limits which distros
build (use `"jammy"` for a fast loop; Jammy is required for the spec and
functional test stages). See `BUILDING.md` for the CI fast-feedback knobs.

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
- `PLAN.md` — open work items for this documentation effort (incl. the deferred
  RFB-deviations writeup).
