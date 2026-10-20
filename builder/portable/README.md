# Portable KasmVNC packages

These packages contain the same native application build in DEB, RPM and tar
formats. They require glibc 2.34 or newer; Alpine/musl and EL8 are not supported.
The x86_64 build uses EL9 distro archives, so its supported CPU baseline is
x86-64-v2. ARM64 is built natively. GPU loaders and drivers, Perl and its modules,
XKB tools/data, and the desktop remain host dependencies.

Install the DEB/RPM with the distro package manager, allowing it to resolve
dependencies. The package name remains `kasmvncserver`, replacing an existing
distro-specific KasmVNC package. RPM systems may need EPEL/CRB for Perl modules.
DEB uses the existing command alternatives and ssl-cert certificate; RPM uses
the existing `kasmvnc-cert` group and `/etc/pki/tls/private/kasmvnc.pem` certificate.
The package does not add desktop users to privileged certificate groups.

The tar archive has filesystem-root-relative paths. Install its runtime
dependencies first, stop existing sessions, preserve `/etc/kasmvnc/kasmvnc.yaml`,
then extract as root at `/`, excluding that configuration file when upgrading.
Run `/usr/lib/kasmvncserver/initialize-tls` as root to generate the RPM-style
certificate if needed. Configure a certificate readable by your desktop user
or add that user to `kasmvnc-cert`. Do not overlay a package-managed installation
with the tar archive; use its DEB/RPM instead. Tar installation has no package
manager tracking or automatic uninstaller; retain its file listing.

Tar dependencies include glibc, libGL, libgbm, Perl, xauth, xdpyinfo, xkbcomp, XKB data,
procps, hostname, openssl, and Perl modules Switch, YAML::Tiny,
Hash::Merge::Simple, List::Util, List::MoreUtils, Try::Tiny, DateTime and
DateTime::TimeZone. Group creation uses groupadd/getent. Mesa DRI and VAAPI
drivers are supplied by the host. Their default search checks the native Debian
multiarch directory (`/usr/lib/x86_64-linux-gnu/dri` or
`/usr/lib/aarch64-linux-gnu/dri`), then `/usr/lib64/dri` and `/usr/lib/dri`.
Explicit `LIBGL_DRIVERS_PATH` and `LIBVA_DRIVERS_PATH` retain their upstream
override behavior for custom driver locations. These paths do not guarantee
compatibility with every host driver; validate the intended GPU and distro.

All shipped ELF executables are checked for architecture, PIE, unexpected
dynamic dependencies, search paths, and a maximum glibc ABI requirement of 2.34.
The adjacent checksum and provenance files identify the build inputs and ELF
boundary. Installed documentation includes dependency license notices and the
builder RPM inventory. This provenance is not a complete SBOM or vulnerability
assessment. Embedded dependencies require rebuilding KasmVNC for security fixes;
host package updates do not patch them. A public release still requires review
of corresponding-source distribution and all embedded dependency licenses.

## Portable CI and legacy fallback

The default pipeline builds portable DEB/RPM/tar packages for amd64 and arm64.
It retains native distro builds for Ubuntu Focal, Oracle 8, openSUSE 15 and
Alpine 3.21–3.23. The 31 portable distro/architecture test jobs remain mandatory.
Functional and vncserver specification tests consume the portable amd64 DEB.
Set `BUILD_FULL_LEGACY=true` to additionally build/test every traditional distro
for comparison. `BUILD_PORTABLE=false` restores the traditional-only matrix and
Jammy test artifacts. Existing distro recipes remain available locally.
`BUILD_DISTROS_REGEX` still filters legacy builds; it does not narrow the portable
test matrix. Its special `none` value disables portable builds as before.

This reduces feature-branch jobs from 101 to 61 and package builds from 34 to 14
compared with the combined portable/legacy pipeline after Nix was removed.
Compare total job duration (runner time), pipeline wall time, and artifact bytes
against the prior successful pipeline; counts alone do not predict time savings.
Public rollout remains subject to manual host/kernel/GPU validation.

Separate build-ID debug symbols are published in DEB, RPM and tar formats, with
checksums and a `.debug.json` mapping. See `DEBUGGING.md` for exact-build matching
and the existing crashpad lookup. SBOM work is deferred; provenance is not an SBOM.
