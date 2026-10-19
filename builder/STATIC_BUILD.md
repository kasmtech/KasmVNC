# Experimental static application libraries

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
| OpenSSL | 4.0.3 | Major upgrade; HTTPS, authentication and UDP/WebRTC need regression coverage. |
| libpng | 1.6.59 | Static copy must not interpose on graphics-driver dependencies. |
| zlib | 1.3.2 | Same symbol-isolation requirement. |
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
That earlier artifact requires GLIBC_2.38; the EL9 profile below targets 2.34. Drivers pull in their
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

## EL9 / glibc 2.34 profile

The opt-in `builder/dockerfile.el9.static` and
`builder/build-static-glibc234-inside-docker` build the application on Oracle
Linux 9 with GCC Toolset 14, keeping Xorg 21.1.12 and the codec/TLS pins above.
Use a clean source tree when building this image:

```sh
docker build -f builder/dockerfile.el9.static -t kasmvnc-static-el9-builder .
docker run --name kasmvnc-static-el9-build kasmvnc-static-el9-builder --no-servertarball
docker cp kasmvnc-static-el9-build:/src/unix/xserver/hw/vnc/Xvnc ./Xvnc-static-glibc234
```

EL9 omits many static development archives. `build-static-desktop-el9` builds
those in the private prefix with verified source hashes, while retaining EL9's
patched GLib/PCRE1, crypt and SELinux archives. AT-SPI headers and pkg-config
metadata match the private 2.52 client. No replacement session or accessibility
daemons are installed. Newer compiler/header compatibility fixes are limited to
build integration, including libffi 3.5.2 and the systemd filesystem-name table.
These source copies still require security/backport review before release.

Current EL9 GCC archives use `_dl_find_object@GLIBC_2.35` through a distro
backport. Therefore merely selecting an EL9 builder is insufficient.
`build-static-gcc-runtime` builds hash-pinned upstream GCC 14.2.0's static
runtime, selecting its existing `dl_iterate_phdr` unwind implementation instead
of the optional newer libc API. The application still uses GCC Toolset 14 and
its static C++ library. The private GCC archive source, patch description,
licenses and hashes are recorded under `/opt/kasm-gcc-runtime/share/gcc-runtime`.
This is an upstream 14.2.0 runtime with a vendor 14.2.1 compiler, not the exact
vendor runtime patch set. Unwinding may be slower than the newer lookup path;
no encoding performance equivalence is claimed.

The entrypoint audits Xvnc and kasmvncpasswd with `check-glibc-floor`, failing
if either imports a symbol newer than GLIBC_2.34. This is an executable ABI
check, not validation of every driver, CPU, kernel or desktop combination.
EL9 static archives retain their CPU requirements; x86_64 targets x86-64-v2
and is not qualified on older CPUs. The pipeline also builds ARM64 natively. It still relies on host
GL/GBM and drivers, glibc, XKB/fonts and launcher/session helpers. EL8's glibc
2.28 and musl-only systems remain outside this target. The standalone development
profile defaults to EL9's `/usr/lib64/dri`; the portable package entrypoint adds
the native Debian multiarch directory and `/usr/lib/dri` to Mesa DRI and VAAPI
driver discovery. `LIBGL_DRIVERS_PATH` and `LIBVA_DRIVERS_PATH` remain available
for custom driver installations.

EL9 GIO carries a backport using `mnt_monitor_veil_kernel`; its private
util-linux/libmount is therefore 2.42.4 rather than Noble's 2.39.3. The password
utility also embeds libcrypt for this profile, avoiding EL9's `libcrypt.so.2`
versus Ubuntu's `libcrypt.so.1` dependency. Both executables are PIE; Xvnc keeps
its six shared dependencies listed above, and kasmvncpasswd needs only libc.

Local x86_64 validation of the final GLIBC_2.34 artifact passed software H.264
browser sessions on Oracle Linux 9 and Ubuntu 22.04; NVIDIA H.264 and AV1
browser sessions plus EGL rendering on Ubuntu 24.04 / RTX5090 / driver595.84;
GTK focus events delivered to Xvnc; HTTPS authentication; core font lookup;
and sd_notify READY. A separate static C++ runtime probe passed exceptions,
backtraces and pthread cancellation/destructors. The stripped Xvnc is
58,622,776 bytes. No text relocations or private library API exports were found.
These tests share the host kernel and do not qualify older kernels, ARM,
pre-v2 CPUs, VAAPI hardware, Vulkan or the complete core-image matrix. The
Dockerfile records the tested build-container setup; it has not received a
separate clean Docker image build and pipeline qualification.


## Portable package pipeline

The package pipeline applies the static-link policy to the shipped helper
executables as well as Xvnc, including PIC archives and static C++/GCC runtimes.
Its packaging audit checks every shipped ELF for PIE, architecture, imported
GLIBC versions and allowed shared dependencies. This avoids silently adding
an EL9-specific GLIBCXX or libcrypt dependency through a helper.

See [builder instructions](README.md#portable-application-packages) and the
[portable installation guide](portable/README.md). The local validation results
above describe the earlier development artifact, not certification of every
pipeline artifact or supported host. CI matrix results and separate manual
hardware/kernel testing determine the qualification of each release.

The portable distro tests require an actual software Mesa OpenGL renderer as
well as a browser session. This catches driver discovery failures even on CI
hosts without a GPU. It does not qualify hardware rendering or buffer capture
on every vendor driver.

The libunwind 1.8.3 pin incorporates the upstream [AArch64 context-type correction](https://github.com/libunwind/libunwind/pull/524) and [stable ARM unwind fixes](https://github.com/libunwind/libunwind/releases/tag/v1.8.3). Its archive hash and license are captured by the generated portable provenance inventory.
