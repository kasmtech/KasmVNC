# First-party KasmVNC Nix build

This builds the checked-out server source, patched Xorg 21.1.12, and the same
web assets used by the DEB/RPM/APK pipeline. It does not repackage a DEB.
`nixpkgs.json` pins both the nixpkgs revision and its content hash; the Docker
bootstrap image is also pinned. Native `x86_64-linux` and `aarch64-linux` CI
jobs produce separate closures. The server's Xorg version follows the current
Noble builder; updating it remains a coordinated upstream security task.

## Build

Initialize the **recorded** kasmweb submodule revision, then run:

```sh
git submodule update --init kasmweb
builder/build-www
builder/build-nix
```

The default version uses `.ci/next_release_version`, as other package jobs do.
For a shallow checkout with no release tags, supply `KASMVNC_NIX_VERSION`.
`NIX_BUILD_CORES` defaults to 4. All building runs in Docker; no host Nix
installation is required. With Nix already installed, `nix-build nix` works
after `builder/build-www`; `revision` and `version` are overridable arguments.
The www stage retains its existing npm build process; it is an explicit input
to the sandboxed server derivation, not a second npm download during that build.

Outputs are in `builder/build/nix_<system>/`:

- `.nar.xz`: native Nix export of the complete runtime closure, for import into
  a Nix installation. This is an export stream, not a single bare NAR file.
- `.tar.xz`: the same closure under `nix/store/`, for container images without
  the Nix package manager. Absolute `/nix/store` paths must be preserved.
- `.store-path`: the package's entry point in the store.
- `.closure`: all referenced store paths.
- `.nixpkgs.json`: dependency pin provenance.
- `.sha256`: checksums for the five files above.

The pipeline's `build_nix_amd64` and `build_nix_arm64` jobs reuse `build_www`,
then upload these files using the existing S3 helpers to
`kasmvnc/$CI_COMMIT_SHA/`, alongside distro packages. They also register the
existing download links and preview index entries. `BUILD_NIX=false` disables
these jobs and their smoke tests; `BUILD_DISTROS_REGEX=none` disables them too.
Native `test_nix_*` jobs validate the exported closure on Ubuntu and Alpine.
No GPU is required by the build or CI smoke jobs. Uploading follows the
existing package pipeline's ordering (build jobs upload before runtime tests).
The checksum file is integrity metadata, not a signed binary-cache trust key.

## Install and run

Verify the downloaded checksum file first, in the directory with its artifacts:

```sh
sha256sum -c kasmvncserver_nix_*.sha256
xz -dc kasmvncserver_nix_*.nar.xz | nix-store --import
nix-env -i "$(cat kasmvncserver_nix_*.store-path)"
```

For a fresh container image without Nix, use Docker `ADD <artifact>.tar.xz /`
and run the package path from `.store-path`. Do not extract over an unrelated
existing Nix installation; use the native import there so its database and GC
roots remain consistent. Copying only the KasmVNC output directory loses its
runtime dependencies. Nix profile installation creates a GC root; a container
closure has no Nix daemon/GC and needs no profile.

The package supplies the interpreter, Perl modules, aliases, XKB data/helpers,
web assets, software Mesa, and dynamically loaded FFmpeg libraries. It reads
optional `/etc/kasmvnc/kasmvnc.yaml` and per-user `~/.vnc/kasmvnc.yaml` as usual.
Provide TLS certificates and writable HOME/configuration yourself; certificates,
passwords and session state must not be written to `/nix/store`. It does not
install or enable a host service, desktop session, or create host users/groups.
Use `-select-de manual` with an explicitly configured desktop startup when the
host's normal desktop-discovery integration is not available.

Video streaming is opt-in. `-videoCodec h264` enables software H.264;
`-videoCodec h264,h264_nvenc` permits software and NVIDIA H.264, which the
browser can select separately. An empty/default setting does not enable video
streaming even when FFmpeg can probe available encoders.

## GPU boundary

The host supplies its kernel driver, firmware, device access, and (for NVIDIA)
a matching user-space driver release. In a container, use NVIDIA Container
Toolkit with `--gpus all` and `NVIDIA_DRIVER_CAPABILITIES=all`. Make only the
injected vendor libraries visible at `/run/opengl-driver/lib`; do not put host
libc or a whole host library directory there. `builder/nix-tests/session.sh`
and `core-entrypoint.sh` demonstrate the container adapter. On NixOS use its
normal graphics driver directory instead of replacing it.

The Xvnc wrapper clears inherited preloads and uses this dedicated driver
path plus the pinned Nix dependency set. Launch Nix programs from a clean
environment: a foreign `LD_LIBRARY_PATH` can break a Nix interpreter before a
shell wrapper runs. The core-image fixture uses the host `/usr/bin/env` to
clear `LD_LIBRARY_PATH` and `LD_PRELOAD` before entering the Nix launcher.
This preserves the original core desktop's native library environment.

Desktop rendering and video encoding are independent. The matrix uses
VirtualGL's EGL backend for a real OpenGL application and then selects NVENC
in the browser. The core-image fixture exercises the existing `KASM_EGL_CARD`
and `KASM_RENDERD` XFCE startup path. `KASM_ENABLE_ZINK` is the core project's
alternative Vulkan/Zink path; it is not implied by passing an EGL test.

No claim is made that this replaces the host kernel or runs on every kernel,
GPU, CPU architecture, or driver version. Alpine runs the Nix-provided glibc
closure alongside musl; it is not a musl-linked KasmVNC binary. A container
distro matrix on one machine tests one kernel, not multiple kernel versions.

## Validation tools

`builder/test-nix-smoke ARTIFACT_DIRECTORY` tests the actual exported closure,
without the test-tools closure. It works with local Docker and DinD; CI sets
`NIX_TEST_DOCKER_HOST=docker` and `NIX_TEST_BIND_IP=0.0.0.0` to reach the DinD
service. Local listening ports default to loopback.

`nix/test-tools.nix` builds separate rendering/encoding/browser-session helpers;
they are not included in the shipped package. `nix/test-matrix.json` records
the 22 bases from workspaces-core-images `ci-scripts/template-vars.yaml` at the
revision recorded in the validation report. `builder/nix-tests/matrix.py` runs
software and NVIDIA/EGL cases with one unchanged server closure across them.
It retains the actual image IDs, renderer output, FFmpeg encode logs, server
logs, browser-selected codec, WebSocket traffic and screenshots. Refresh the
matrix deliberately when core-images changes.

The browser harness uses Playwright with Google Chrome (`channel: chrome`)
because its bundled Chromium may not decode H.264. Prepare a disposable
browser container with Playwright and Chrome, copy `browser.cjs` to
`/tmp/browser/browser.cjs`, and mount the result directory at `/results`.
Export the test-tools closure alongside the server under the test rootfs,
then invoke the matrix script with `--rootfs`, `--package`, `--tools`, and
`--results`. Optional `--only` and `--modes` select a smaller set of cases; `--jobs` controls
concurrency (default 1). Tests use a disposable `kasm_user` / `kasmRulez` login.
All test endpoints are loopback-bound and use disposable test sessions.

`core.Dockerfile` is an integration fixture: it swaps the KasmVNC executables
and web assets in an existing Noble core image, keeps its desktop/services,
and stages NVIDIA libraries at runtime. It does not publish or modify the
workspaces-core-images repository.
