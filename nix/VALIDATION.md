# VNC-680 Nix packaging validation

Validated locally on 2026-10-16. All 44 authenticated distro cases passed,
plus two full Ubuntu Noble Workspaces desktop cases (software and NVIDIA).
This report records the pre-CI local validation of
`feature/VNC-680_nix_packaging`. No remote pipeline or S3 write had been
performed at the time of these local tests.

## Provenance

- KasmVNC base: `b78c7491f29d35a4d125eb85a119c3a63143d069`, plus this Nix packaging change.
- Recorded kasmweb submodule: `3ecabb3d2c10a85d6098f527c661ab595bb9da88`.
- workspaces-core-images: `11a313aaf3ebf1d35851d707875c2c7627e8fdd0`.
  Matrix source: `ci-scripts/template-vars.yaml`; EGL integration follows `vnc_startup.sh`.
- nixpkgs: `714a5f8c4ead6b31148d829288440ed033ccc041` (content hash in `nixpkgs.json`).
- Architecture: x86_64-linux; host kernel: `7.0.0-30-generic`.
- GPU: NVIDIA RTX 5090; NVIDIA driver `595.84`; EGL via VirtualGL.
- Browser: Google Chrome `155.0.8059.84`, Playwright `1.56.1`.
- Package: `/nix/store/d6nl8a8l0mn06i2jz523sncsvisbxbdm-kasmvnc-1.5.1`.
- Full core fixture: `kasmweb/core-ubuntu-noble@sha256:a54e0a391ae750950848983bb47dced43652e4e7908b20ede09df0c7cc08e6d6`.

## Matrix results

Each row uses the identical server closure. Software cases use Docker's runc
runtime without GPU devices; GPU cases use NVIDIA Container Toolkit. A separate
Nix test-tools closure supplies Openbox, xterm, glxgears, VirtualGL and test
commands. Tests verify X11, authenticated HTTP, Mesa llvmpipe or NVIDIA renderer,
a two-second FFmpeg encode, browser connection and explicit H.264 codec
selection, sustained framebuffer traffic, and server-side encoder output.
Screenshots and logs are retained. The two exported-package smoke tests do not
mount the test-tools closure, checking that it is not required to start KasmVNC.

There are 22 core pipeline entries; KasmOS and Debian Bookworm share a base.
These are base-container tests, not 22 rebuilt full Workspaces desktop images.
The additional Noble core fixture retains the actual XFCE/services/startup path.

| Core matrix entry | Base image | No GPU / software H.264 | NVIDIA EGL / NVENC H.264 |
|---|---|---|---|
| kasmos-kasmos | `debian:bookworm-slim` | PASS | PASS |
| ubuntu-jammy | `ubuntu:22.04` | PASS | PASS |
| ubuntu-noble | `ubuntu:24.04` | PASS | PASS |
| ubuntu-resolute | `ubuntu:26.04` | PASS | PASS |
| debian-bookworm | `debian:bookworm-slim` | PASS | PASS |
| debian-trixie | `debian:trixie-slim` | PASS | PASS |
| kali-rolling | `kalilinux/kali-rolling:latest` | PASS | PASS |
| oracle-8 | `oraclelinux:8` | PASS | PASS |
| oracle-9 | `oraclelinux:9` | PASS | PASS |
| rhel-9 | `registry.access.redhat.com/ubi9/ubi` | PASS | PASS |
| rockylinux-8 | `rockylinux:8` | PASS | PASS |
| rockylinux-9 | `rockylinux:9` | PASS | PASS |
| almalinux-8 | `almalinux:8` | PASS | PASS |
| almalinux-9 | `almalinux:9` | PASS | PASS |
| opensuse-16 | `opensuse/leap:16.0` | PASS | PASS |
| fedora-42 | `fedora:42` | PASS | PASS |
| fedora-43 | `fedora:43` | PASS | PASS |
| parrotos-7 | `parrotsec/core:7.1` | PASS | PASS |
| alpine-321 | `alpine:3.21` | PASS | PASS |
| alpine-322 | `alpine:3.22` | PASS | PASS |
| alpine-323 | `alpine:3.23` | PASS | PASS |
| cuda-jammy | `nvidia/cuda:12.2.2-cudnn8-devel-ubuntu22.04` | PASS | PASS |

## Full Workspaces image and package checks

- Replaced the packaged server executables and web assets in the Noble core
  image with the exported Nix closure. Original core desktop/services remain.
- Software: XFCE and animated GL spheres with llvmpipe; browser selected
  `SW H.264/AVC` and received frames.
- NVIDIA: `KASM_EGL_CARD=/dev/dri/card2`, `KASM_RENDERD=/dev/dri/renderD129`,
  actual device groups 44 and 992. VirtualGL reported RTX 5090. Browser selected
  `HW H.264/AVC (NVENC)`; server logs show successful hardware-encoded frames.
- Both core cases: unauthenticated HTTP 401, incorrect password 401, valid
  credentials 200. Browser screenshot confirms a rendered desktop/application.
- Source build and install checks passed, including SHA-256 password creation.
- Exported tar closure: checksums, Xvnc version and HTTP startup passed on
  Ubuntu 24.04 and Alpine 3.23, without test-tools.
- Export stream imported into a fresh Nix container; Xvnc version ran and the
  runtime dependency closure contained 224 store paths. The test initially
  lacked an xz command in the import container; streaming decompressed input
  into `nix-store --import` resolved that harness prerequisite.
- Shell syntax, Python syntax and CI YAML checks passed inside containers.
- S3 upload-helper mock verified the existing `kasmvnc/$CI_COMMIT_SHA/` path
  and preview-index registration. Actual S3 delivery remains unverified until CI runs.
- ARM64 derivation evaluated; no ARM64 compilation or runtime testing on this host.

## Packaging issues found and corrected

1. The launcher treated `/nix/store` as a development-tree installation and
   selected the wrong desktop helper. The derivation patches its system-install
   detection and data/helper paths, and supplies Perl modules and helper commands.
2. Default nixpkgs libxcrypt disables SHA-256 crypt, producing empty KasmVNC
   password hashes. Use `libxcrypt-legacy` and fail the build if a password does
   not contain the expected `$5$` hash. Authenticated matrix/core tests pass.
3. FFmpeg is dynamically loaded; explicit retained RPATH keeps it in the
   exported closure. GPU libraries remain a separately provided runtime input.
4. Host `LD_LIBRARY_PATH`/`LD_PRELOAD` can break a Nix interpreter before its
   wrapper executes. Core fixture entry points clear these variables before
   entering Nix; only NVIDIA vendor libraries enter `/run/opengl-driver/lib`.
5. Current nixpkgs CMake/JPEG linking needs derivation-local compatibility
   substitutions. Existing distro build source behavior is unchanged.

## Limits and follow-up

- Containers share one kernel. This does not establish a minimum kernel or
  compatibility with older enterprise kernels; validate those in VMs/native hosts.
- Alpine uses Nix glibc alongside the host's musl. This is not a musl-linked build.
- GPU support requires suitable device permissions and host-matched vendor
  libraries. Only this NVIDIA GPU/driver was exercised; Intel/AMD/VAAPI,
  alternative NVIDIA versions and Vulkan/Zink remain untested.
- EGL desktop acceleration and NVENC encoding were tested separately and together.
  Passing EGL does not establish Vulkan/Zink support.
- Native ARM64 CI build and smoke jobs are implemented but not yet executed.
- Nix pins user-space dependencies, including Mesa. It does not package a kernel,
  kernel GPU module, firmware, or the surrounding desktop/session integration.
- No remote CI run, release publication, signed binary cache, or independent
  reproducibility comparison has been performed. Web assets retain the existing
  frontend build procedure. Follow the existing dependency/security update process.
- Artifacts embed the base source revision; this local test includes uncommitted
  packaging changes. Rebuild in CI after committing for final release provenance.

## Reproduction and evidence

See `README.md` for build/import/GPU prerequisites. Local build command:
`KASMVNC_NIX_VERSION=1.5.1 NIX_BUILD_CORES=10 builder/build-nix`.
Run `builder/test-nix-smoke builder/build/nix_x86_64-linux` for package smoke checks.
The matrix was run with `builder/nix-tests/matrix.py --jobs 3` using the package
above and test-tools `/nix/store/d0pxh6m62ilixxw4ycj8canvza47maqz-kasmvnc-test-tools`.

The task's `validation/` directory (outside this repository) contains
`matrix-results.json` with actual image IDs/package/kernel, per-case browser
JSON/logs/screenshots, renderer output, encoder/server logs, build/import/smoke
logs and the core desktop screenshots. `builder/nix-tests/` contains reusable
session/browser/matrix scripts and the core-image replacement fixture.

Artifact checksums from the successful build:

```text
a5a52f0eeae5ebe5e52eddf1d80ccbef41c93dcb60fd1e48f4054d4e506db578  kasmvncserver_nix_1.5.1_feature_VNC-680_nix_packaging_b78c74_x86_64-linux.tar.xz
4e5dbd769e2ad59a62d5cbadbe99005f8978e7c199a63d8a1051426d4761a79f  kasmvncserver_nix_1.5.1_feature_VNC-680_nix_packaging_b78c74_x86_64-linux.nar.xz
c5a00dcd107169cd6df2adb1d739d10198e814ce76651dac6bbde4a1662f7bd8  kasmvncserver_nix_1.5.1_feature_VNC-680_nix_packaging_b78c74_x86_64-linux.store-path
070eafee107371ad004e47f3f9ca08203b5345178df0a6ad0cb0c4b9ccdc534d  kasmvncserver_nix_1.5.1_feature_VNC-680_nix_packaging_b78c74_x86_64-linux.closure
18aed9f81d49dfb042d7e702d4b0d489f00de4f59492bb2dfad417afd7bf5ccd  kasmvncserver_nix_1.5.1_feature_VNC-680_nix_packaging_b78c74_x86_64-linux.nixpkgs.json
```
