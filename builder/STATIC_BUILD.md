# Experimental static application libraries

The portable packages now dynamically link system OpenSSL 3 using a pinned
OpenSSL 3.0 build-only SDK. They do not embed the OpenSSL 4 runtime described in
the older standalone static experiments below. See the
[portable OpenSSL boundary](portable/README.md#system-openssl-3) for packaging,
ABI auditing, provenance and security-update behavior. This change applies to
both glibc and musl portable profiles, independently of FFmpeg linking mode.

`KASMVNC_STATIC_FFMPEG` is a CMake option, OFF by default. OFF preserves the
soname-based `dlopen` path and first-use loading of libavformat. ON resolves
FFmpeg functions directly and links the archives, including pkg-config's private
codec dependencies. Missing archives fail configuration. The wrapper API and
thread-safe first-use initialization are shared by both modes. Static mode does
not postpone mapping FFmpeg code; the executable contains that code from startup.

`KASMVNC_STATIC_RUNTIME_PREFIX` optionally embeds OpenSSL, PNG, zlib and WebP
from a separate static prefix, and links the C++ runtime statically for Xvnc.
The normal build already uses static JPEG, libyuv and several utility libraries.
Private codec/TLS/image symbols are hidden from dynamically loaded drivers.

This is **not a fully static executable or a distro-independent package**.
Without the desktop option below, glibc, graphics/driver interfaces and
X11/desktop integration dependencies remain shared. The Perl launcher and external helpers are unchanged. Building on Noble
produced a binary requiring GLIBC_2.38 in local testing. Shared font/graphics
libraries can still map their own distro PNG, zlib and C++ runtime copies;
embedding private copies does not remove those transitive dependencies. Do not
advertise this artifact as working on every distribution, including musl systems.

## Container build

Build the existing Noble builder from the source tree, then opt into the new
entrypoint. Use a fresh builder/source tree when changing dependency prefixes;
CMake, pkg-config and autotools cache discovery results.

```sh
docker build -f builder/dockerfile.ubuntu_noble.build \
  --build-arg OUTPUT_OWNER_UID="$(id -u)" -t kasmvnc-static-builder .
docker run --name kasmvnc-static-build \
  --entrypoint /src/builder/build-static-inside-docker \
  kasmvnc-static-builder --no-servertarball
docker cp kasmvnc-static-build:/src/unix/xserver/hw/vnc/Xvnc ./Xvnc-static
```

Omit `--no-servertarball` and mount `/build` to use the existing server tarball
packaging step (requires `builder/www`). This does not change CI release jobs or
turn the tarball into an installer. For desktop testing, replace
`/usr/bin/Xkasmvnc` in the matching Workspaces core image, retaining its existing
launcher/configuration, and replace the web assets with those from this revision.

## Dependency policy and compatibility

The opt-in scripts pin source hashes or commits; they do not follow moving heads:

| Component | Selected version | Compatibility consideration |
| --- | --- | --- |
| FFmpeg | 9.0.2 | New major ABI; build all consumers against matching headers. |
| x264 | stable commit b35605ace3dd | Rolling stable branch pinned to a commit. |
| x265 | 4.3 | Server supports HEVC; browser decoding support is separate. |
| SVT-AV1 | 4.2.0 | Included, but KasmVNC currently disables software AV1 probing. |
| NVIDIA codec headers | 13.0.19.1 | Requires driver >=570. Latest 13.1 requires >=610, so is deliberately not selected. |
| OpenSSL | 4.0.3 (standalone static experiment) | Portable packages instead use a build-only 3.0 SDK and dynamically link the host's OpenSSL 3.x runtime. |
| libpng | 1.6.59 | Static copy must not interpose on graphics-driver dependencies. |
| zlib | 1.4 | Same symbol-isolation requirement; includes the upstream fix for CVE-2026-85091. |
| WebP | 1.6.0 | Same symbol-isolation requirement. |

Existing builder pins for JPEG, fmt, TBB, libcpuid, libstatgrab and libyuv remain
in `build-deps.sh`. This experiment is not a complete lockfile or SBOM for the
whole server. Distro-owned desktop libraries remain on their distro versions.
FFmpeg disables network protocols and automatic external-library detection;
file demuxing/decoding remains available for benchmarks. CUDA/NVENC and VAAPI
still discover host drivers at runtime. No proprietary driver is embedded.

Updating an embedded dependency requires rebuilding and redistributing KasmVNC.
Before public release: produce a complete SBOM and vulnerability results, retain
corresponding sources/license notices, select/test an oldest glibc baseline,
validate each architecture and GPU-driver baseline, cover UDP/WebRTC and all
supported codecs, and finish the remaining shared-library/packaging policy.

## Extended static desktop profile (experimental, Noble)

Set `KASMVNC_STATIC_DESKTOP_PREFIX=/opt/kasm-desktop` when invoking
`build-static-inside-docker` to embed the remaining supported desktop libraries.
This is opt-in; an empty prefix retains the existing link command. The prototype
uses Noble development-package archives and builds missing AT-SPI 2.52.0,
libva 2.20.0, util-linux/libmount 2.39.3, and PIC libunwind 1.8.3 archives with
source hashes. These versions preserve the current experiment's interfaces;
they are not a claim to be latest upstream or security-approved. Before release,
reconcile source-built copies with distro security patches and the SBOM policy.

In the existing Noble builder, install the extra build prerequisites first:

```sh
apt-get update
apt-get install -y --no-install-recommends meson ninja-build libxml2-dev \
  libmount-dev libcap-dev liblz4-dev libzstd-dev libgcrypt20-dev libselinux1-dev
```

Run inside that container with the current source at `/src`:

```sh
KASMVNC_STATIC_DESKTOP_PREFIX=/opt/kasm-desktop \
  /src/builder/build-static-inside-docker --no-servertarball
```

The new linker wrapper runs after libtool expands `.la` dependencies. It selects
archives explicitly, adds pkg-config's private dependencies and libsystemd's
missing libcap dependency, and fails on missing archives instead of silently
falling back to shared libraries. A rescan group handles circular archive
references. Private third-party symbols are hidden from host driver modules.
`Xvnc.static-link.json` records the archives and final command; it is build
provenance, not a complete SBOM. The default dynamic build is unaffected.

Xorg and libX11 both define `XkbFreeGeomOverlayKeys`. The wrapper renames only
libX11's copy and its internal references in a private archive, preserving
Xorg's implementation and retaining duplicate-symbol error checking. The distro
libunwind archive uses non-PIE LTO; rebuilding it as PIC preserves executable
PIE rather than disabling ASLR compatibility. No features were deliberately
removed merely to eliminate a link dependency.

| Previously shared dependency | Extended profile |
| --- | --- |
| libva, libva-drm | Static frontends; vendor VA drivers remain dynamic. |
| libdrm, libxshmfence | Static; kernel/device requirements remain. |
| libX11, libXau, libXdmcp | Static; X11 private symbol adjustment noted above. |
| libXfont2, FreeType, pixman | Static, including fontenc and compression dependencies. |
| libcrypt | Static, retaining the builder's password-hash support. |
| libsystemd | Static, selective archive extraction; notification tested. |
| libdbus, GLib, GObject, AT-SPI | Static with GIO/GModule, PCRE2, ffi, mount/blkid, SELinux/sepol and X extension closure. |
| libunwind | Static PIC 1.8.3 with LZMA support; upstream AArch64 context-type fixes for GCC 14. |
| GCC runtime | Static application runtime; host drivers/glibc may still load libgcc_s. |
| libGL | Shared host GLVND dispatch; a second private dispatcher risks split GLX/EGL state. |
| libgbm | Shared host Mesa/backend interface; upstream target is shared and backend modules remain dynamic. |
| libc, libm, libmvec, ELF loader | Shared deliberately. |

GLVND and Mesa GBM do not expose ordinary supported static equivalents in the
reviewed builds. Keeping them shared preserves host GPU integration; it is not
proof that custom static ports are impossible. See [GLVND architecture](https://github.com/NVIDIA/libglvnd/blob/master/README.md)
and [Mesa GBM build](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gbm/meson.build).

The earlier Noble-built x86_64 Xvnc directly needs only `libGL.so.1`, `libgbm.so.1`,
`libm.so.6`, `libmvec.so.1`, `libc.so.6`, and `ld-linux-x86-64.so.2`.
That earlier artifact requires GLIBC_2.38; the current portable profile below targets 2.28. Drivers pull in their
own X11/DRM/C++ and other dependencies: a process map is broader than this list.
Perl, external helpers, font/XKB data, session/accessibility bus daemons, and
host GPU drivers remain deployment requirements. This profile targets Xvnc;
it does not make every auxiliary executable or the entire desktop static.

Local validation: Noble core image with software H.264; RTX5090 NVENC H.264
and AV1; EGL rendering; GTK focus gain/loss delivered to Xvnc's AT-SPI callbacks;
HTTPS authentication; sd_notify READY; separate C++ exception/backtrace/thread
cancellation probe. PIE and private-symbol isolation checked. Accessibility
logging requires `ATSPI_DEBUG=1` for the Xorg make step. No ARM, older-distro,
Vulkan, VA-API hardware, socket activation, or full release qualification is
claimed. GIO modules must not introduce conflicting private GObject state;
process maps during the accessibility tests showed no shared GLib/GObject/GIO
copies in Xvnc.

## Production portable profile

Use `builder/build-portable` from a clean committed checkout. The production
profile uses pinned Oracle Linux 8, GCC Toolset 14, C++20 and Xorg 21.1.25,
with a glibc 2.28 ABI ceiling. Local and CI defaults use host FFmpeg adapters,
OpenSSL 3, desktop libraries and compiler runtimes. Private application zlib
and selected performance libraries remain embedded. This supersedes the
removed standalone EL9 Dockerfile and its glibc 2.34 entrypoint.

The application supplies small first-party executable startup and stat wrappers;
it does not embed glibc CRT or libc_nonshared objects. GCC CRT and selected
compatibility objects retain their source and SBOM evidence. The pinned
xorgproto 2024.1 SDK supplies build-only headers needed by current Xorg.

Every packaged executable is audited for PIE, hardening, architecture, allowed
shared dependencies, glibc versions and GCC 8-compatible C++ symbol imports.
See [builder instructions](README.md#portable-application-packages) and the
[portable installation guide](portable/README.md) for exact runtime requirements.
The earlier static experiments above are historical evidence, not qualification
of the current portable artifacts. Native runtime and functional CI qualify the
exact packages, including the EL8 floor and newer userspaces.

The portable distro tests require an actual software Mesa OpenGL renderer as
well as a browser session. This catches driver discovery failures even on CI
hosts without a GPU. It does not qualify hardware rendering or buffer capture
on every vendor driver.

The libunwind 1.8.3 pin incorporates the upstream [AArch64 context-type correction](https://github.com/libunwind/libunwind/pull/524) and [stable ARM unwind fixes](https://github.com/libunwind/libunwind/releases/tag/v1.8.3). Its archive hash and license are captured by the generated portable provenance inventory.
