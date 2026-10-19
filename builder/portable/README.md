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
DateTime::TimeZone. Group creation uses groupadd/getent. For VAAPI set
`LIBVA_DRIVERS_PATH` to the host driver directory when it differs from
`/usr/lib64/dri` (for example `/usr/lib/x86_64-linux-gnu/dri` on Ubuntu).

All shipped ELF executables are checked for architecture, PIE, unexpected
dynamic dependencies, search paths, and a maximum glibc ABI requirement of 2.34.
The adjacent checksum and provenance files identify the build inputs and ELF
boundary. Installed documentation includes dependency license notices and the
builder RPM inventory. This provenance is not a complete SBOM or vulnerability
assessment. Embedded dependencies require rebuilding KasmVNC for security fixes;
host package updates do not patch them. A public release still requires review
of corresponding-source distribution and all embedded dependency licenses.
