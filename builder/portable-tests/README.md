# Portable runtime validation scope

[matrix.json](matrix.json) is the machine-readable list used by
[`builder/test-portable`](../test-portable) and the CI package tests. A successful
pipeline qualifies its exact package payloads against these container userspaces.
It does not establish universal Linux or kernel compatibility. Consult the
pipeline for the revision being installed; a matrix entry alone is not a passing
result.

## Container matrix

Every row below runs on native AMD64 and ARM64 runners, except CUDA Jammy, which
is AMD64-only in the source Workspace image matrix. DEB and RPM cases also test
the same binaries installed from the tar archive. Alpine tests its separate
musl APK. This gives 47 distro/architecture cases and 86 format runs.

| Userspace | Container image(s) | Formats |
| --- | --- | --- |
| Ubuntu 22.04, 24.04, 26.04 | `ubuntu:22.04`, `ubuntu:24.04`, `ubuntu:26.04` | DEB, tar |
| Debian 12, 13 | `debian:bookworm-slim`, `debian:trixie-slim` | DEB, tar |
| Kali rolling | `kalilinux/kali-rolling:latest` | DEB, tar |
| Parrot OS 7.1 | `parrotsec/core:7.1` | DEB, tar |
| KasmOS userspace fixture | `debian:bookworm-slim`; not a booted KasmOS system | DEB, tar |
| CUDA on Ubuntu 22.04 (AMD64 only) | `nvidia/cuda:12.2.2-cudnn8-devel-ubuntu22.04`; software runtime test, no GPU claim | DEB, tar |
| Oracle Linux 8 | `oraclelinux:8` plus Oracle EPEL/CodeReady and RPM Fusion | RPM, tar |
| Rocky Linux 8 | `rockylinux:8` plus EPEL/PowerTools and RPM Fusion | RPM, tar |
| AlmaLinux 8 | `almalinux:8` plus EPEL/PowerTools and RPM Fusion | RPM, tar |
| Oracle Linux 9 | `oraclelinux:9` | RPM, tar |
| RHEL 9 userspace fixture | `registry.access.redhat.com/ubi9/ubi` plus signed Oracle EL9 repositories; not pristine or entitled RHEL | RPM, tar |
| Rocky Linux 9 | `rockylinux:9` | RPM, tar |
| AlmaLinux 9 | `almalinux:9` | RPM, tar |
| openSUSE Leap 15.6, 16.0 | `opensuse/leap:15.6`, `opensuse/leap:16.0` | RPM, tar |
| Fedora 42, 43 | `fedora:42`, `fedora:43` | RPM, tar |
| Alpine 3.21, 3.22, 3.23, 3.24 | `alpine:3.21`, `alpine:3.22`, `alpine:3.23`, `alpine:3.24` | APK |

The glibc build's ABI floor is glibc 2.28. The musl build targets musl 1.2.5.
CPU build baselines are x86-64 for glibc and musl AMD64, and ARMv8-A for ARM64. Native runner execution proves
operation on those runner CPUs; it is not a test on every minimum-generation
CPU. Host DSOs and drivers have their own requirements. The
[host-library boundary](../portable/README.md#system-desktop-and-compiler-libraries)
and [FFmpeg families](../portable/README.md#ffmpeg-runtime-adapters) must also be
satisfied.

Fedora 42 uses RPM Fusion's archived repositories through an archive mirror
listed by its metalink service; package GPG checks remain enabled. RPM fixtures
explicitly install `ffmpeg-libs` to require the software codecs rather than
accepting Fedora's restricted `ffmpeg-free` provider. Tar fixtures retain
already-installed SONAME providers, avoiding incompatible cross-vendor D-Bus
upgrades in the UBI9/Oracle environment.

## What the evidence establishes

The runtime tests use Docker with `runc` and reject cross-architecture emulation.
They check package installation, live host-library ownership, authenticated TLS,
software H.264, keyboard input, software GLX and package lifecycle behavior.
Reports identify the package and executable hashes; these must match the
published build being qualified. Container image tags and package repositories
can change, so historical success does not qualify a later payload or userspace.

All distro containers on one runner share that runner's host kernel. This matrix
does not test separate oldest/current kernel boots, establish a minimum kernel,
or qualify host GPU drivers. Record distinct VM/kernel and actual GPU evidence
separately before claiming those capabilities or a public-release support range.

## Exclusions and remaining qualification

| Target | Reason it is outside this portable qualification |
| --- | --- |
| Ubuntu 20.04 | Outside the current Workspace image CI matrix; satisfying glibc alone does not provide the required OpenSSL 3 and desktop runtime. |
| Earlier openSUSE Leap 15 releases | The Leap 15 target is specifically 15.6 (glibc 2.38); its results do not qualify earlier minor releases. |
| NixOS | No Nix closure or NixOS integration is shipped by this portable package profile. Its runtime loader, host libraries and data paths assume a conventional filesystem layout. Installation and runtime on NixOS have not been qualified. |
| Untested distro releases and kernels | The listed userspace runs do not qualify future releases or an arbitrary old host kernel. |
| GPU rendering/encoding | Software GLX and H.264 tests do not establish VA-API, NVENC or proprietary-driver compatibility. Separate hardware tests are required. |

Alpine 3.21 remains an ABI regression target; inclusion in this matrix does not
extend its upstream security-maintenance lifetime. See the
[musl baseline limits](../musl/README.md#baseline-maintenance-and-limits).
