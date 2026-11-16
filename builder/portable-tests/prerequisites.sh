#!/usr/bin/env bash
# Only for disposable package-validation containers. Runtime package dependencies
# are installed by the package manager; tar installations need them explicitly.
set -euo pipefail
format=${1:?package format required}
stage=${2:-all}
ffmpeg_mode=${3:-static}
case "$ffmpeg_mode" in static|adapters) ;; *) echo 'Unknown FFmpeg mode' >&2; exit 2 ;; esac
. /etc/os-release
export DEBIAN_FRONTEND=noninteractive
# Tar archives have no package transaction to resolve their host library ABI.
# RPM capability names avoid EL/Fedora/SUSE package-name differences, including
# the libxcrypt-compat provider of libcrypt.so.1. All supported targets are 64-bit.
tar_host_sonames=(libpng16.so.16 libfreetype.so.6 libpixman-1.so.0 libatspi.so.0
  libglib-2.0.so.0 libgobject-2.0.so.0 libdbus-1.so.3 libX11.so.6 libXau.so.6
  libXcursor.so.1 libXdmcp.so.6 libXext.so.6 libXfixes.so.3 libXfont2.so.2
  libXrandr.so.2 libXtst.so.6 libxshmfence.so.1 libunwind.so.8 libstdc++.so.6
  libgcc_s.so.1 libcrypt.so.1 libsystemd.so.0 libssl.so.3 libcrypto.so.3)
tar_rpm_capabilities=()
if [[ "$ffmpeg_mode" = static ]]; then
  tar_host_sonames+=(libdrm.so.2 libva.so.2 libva-drm.so.2)
fi
for soname in "${tar_host_sonames[@]}"; do
  tar_rpm_capabilities+=("$soname()(64bit)")
done
if command -v apk >/dev/null; then
  if [[ "$stage" = bootstrap ]]; then
    apk add --no-cache bash ca-certificates
    exit 0
  fi
  # Install test tools only after the native APK has proved its own runtime
  # dependencies resolve. The browser runs in a separate Ubuntu container.
  apk add --no-cache curl openssl procps python3 openbox xterm xdpyinfo \
    dbus-x11 runuser shadow binutils mesa-utils mesa-dri-gallium
elif command -v apt-get >/dev/null; then
  # Minimal Debian/Ubuntu images discard documentation during dpkg install.
  # Retain this package's SBOM/VEX so the installed-artifact check can run.
  printf 'path-include=/usr/share/doc/kasmvncserver/\npath-include=/usr/share/doc/kasmvncserver/*\n' > /etc/dpkg/dpkg.cfg.d/zz-portable-test-docs
  if [[ "$ID" = kali ]]; then
    # The geographic redirector repeatedly selected an unreachable CI mirror.
    # Use Kali's official CDN directly; keep the distribution's signed metadata.
    # HTTP bootstraps ca-certificates; apt still verifies the Kali signing key.
    for source in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
      [[ -f "$source" ]] || continue
      sed -i -E 's!https?://http.kali.org/kali!http://kali.download/kali!g' "$source"
    done
  fi
  printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\n' > /etc/apt/apt.conf.d/80portable-test-network
  apt-get update
  if [[ "$stage" = bootstrap ]]; then
    apt-get install -y --no-install-recommends ca-certificates
    exit 0
  fi
  apt-get install -y --no-install-recommends ca-certificates curl openssl procps \
    python3 openbox xterm x11-utils dbus-x11 util-linux binutils mesa-utils libgl1-mesa-dri
  if [[ "$format" = tar ]]; then
    tar_deb_libraries=()
    if [[ "$ffmpeg_mode" = static ]]; then
      tar_deb_libraries+=(libdrm2 libva2 libva-drm2)
    fi
    for library in libpng16-16 libatspi2.0-0 libglib2.0-0; do
      if apt-cache show "${library}t64" >/dev/null 2>&1; then
        tar_deb_libraries+=("${library}t64")
      else
        tar_deb_libraries+=("$library")
      fi
    done
    apt-get install -y --no-install-recommends bash tzdata ssl-cert xauth x11-xkb-utils \
      xkb-data libgl1 libgbm1 "${tar_deb_libraries[@]}" libfreetype6 libpixman-1-0 \
      libdbus-1-3 libx11-6 libxau6 libxcursor1 libxdmcp6 libxext6 libxfixes3 \
      libxfont2 libxrandr2 libxtst6 libxshmfence1 libunwind8 libstdc++6 libgcc-s1 \
      libcrypt1 libsystemd0
  fi
elif command -v zypper >/dev/null; then
  # Leap container defaults omit RPM documentation. Override only the test
  # package transaction, leaving dependency installation policy unchanged.
  sed 's/^[[:space:]]*rpm.install.excludedocs[[:space:]]*=.*/rpm.install.excludedocs = no/' /etc/zypp/zypp.conf > /portable/zypp-with-docs.conf
  zypper --non-interactive refresh
  if [[ "$stage" = bootstrap ]]; then
    zypper --non-interactive install --no-recommends ca-certificates
    exit 0
  fi
  zypper --non-interactive install --no-recommends ca-certificates curl openssl \
    procps python3 openbox xterm xdpyinfo dbus-1-x11 util-linux shadow binutils tar gzip Mesa-dri Mesa-demo-x
  if [[ "$format" = tar ]]; then
    zypper --non-interactive install --no-recommends bash timezone xauth xkeyboard-config \
      xkbcomp libglvnd libgbm1 hostname "${tar_rpm_capabilities[@]}"
  fi
else
  # Enterprise Linux needs EPEL for the desktop prerequisites.
  if [[ "$stage" != tools ]]; then
  case "$ID" in
    ol)
      el_major=${VERSION_ID%%.*}
      dnf install -y "oracle-epel-release-el${el_major}" dnf-plugins-core
      dnf config-manager --set-enabled "ol${el_major}_codeready_builder" ;;
    rocky|almalinux)
      dnf install -y epel-release dnf-plugins-core
      if [[ ${VERSION_ID%%.*} = 8 ]]; then
        dnf config-manager --set-enabled powertools
      else
        dnf config-manager --set-enabled crb
      fi ;;
    rhel)
      # Match workspaces-core-images install_rhel.sh (11a313aa): its RHEL
      # desktop is UBI9 with Oracle repositories, not an entitled RHEL base.
      # UBI alone omits required X11 packages even with EPEL.
      for repo in appstream codeready/builder baseos/latest distro/builder; do
        id=${repo//\//_}
        cat > "/etc/yum.repos.d/portable-core-el9-${id}.repo" <<EOF
[portable_core_el9_${id}]
name=Core images EL9 ${repo}
baseurl=https://yum.oracle.com/repo/OracleLinux/OL9/${repo}/\$basearch/
gpgkey=https://yum.oracle.com/RPM-GPG-KEY-oracle-ol9
gpgcheck=1
enabled=1
EOF
      done
      dnf install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm ;;
  esac
  fi
  if [[ "$stage" = bootstrap ]]; then
    dnf install -y ca-certificates
    exit 0
  fi
  dnf install -y ca-certificates '/usr/bin/curl' openssl procps-ng python3 openbox xterm \
    '/usr/bin/xdpyinfo' dbus-x11 util-linux shadow-utils binutils tar gzip \
    '/usr/bin/glxinfo' mesa-dri-drivers
  if [[ "$format" = tar ]]; then
    # Do not upgrade an already satisfied SONAME capability. On the UBI/Oracle
    # fixture, requesting libdbus explicitly can select a newer UBI dbus-libs
    # that conflicts with the exact version required by Oracle's dbus-x11.
    missing_capabilities=()
    for capability in "${tar_rpm_capabilities[@]}"; do
      if ! rpm -q --whatprovides "$capability" >/dev/null 2>&1; then
        missing_capabilities+=("$capability")
      fi
    done
    dnf install -y bash tzdata xorg-x11-xauth xkeyboard-config xkbcomp \
      libglvnd-glx mesa-libgbm hostname "${missing_capabilities[@]}"
  fi
fi
if [[ "$ffmpeg_mode" = adapters ]]; then
  # Match the codec repositories used by Workspaces core images. Installing the
  # runtime in this disposable fixture does not bundle it into the application.
  if command -v apk >/dev/null; then
    apk add --no-cache ffmpeg
  elif command -v apt-get >/dev/null; then
    apt-get install -y --no-install-recommends ffmpeg
  elif command -v zypper >/dev/null; then
    zypper addrepo -cfp 90 'https://ftp.gwdg.de/pub/linux/misc/packman/suse/openSUSE_Leap_$releasever/' packman
    zypper --non-interactive --gpg-auto-import-keys refresh packman
    zypper --non-interactive install --no-recommends --allow-vendor-change --from packman ffmpeg-4
  else
    if [[ "$ID" = fedora ]]; then
      release="https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-${VERSION_ID}.noarch.rpm"
    else
      release="https://mirrors.rpmfusion.org/free/el/rpmfusion-free-release-${VERSION_ID%%.*}.noarch.rpm"
    fi
    # Bootstrap the repository package over HTTPS; its configuration enables
    # signature verification for the FFmpeg packages installed below.
    dnf install -y --nogpgcheck "$release"
    if [[ "$ID" = fedora && "$VERSION_ID" = 42 ]]; then
      # Fedora 42 is archived. Its live RPM Fusion metalink currently advertises
      # stale repomd checksums; use a mirror listed by RPM Fusion's archive
      # metalink directly. Retain repository keys and package GPG verification.
      for repo in /etc/yum.repos.d/rpmfusion-free.repo /etc/yum.repos.d/rpmfusion-free-updates.repo; do
        sed -i \
          -e 's|^metalink=|#metalink=|' \
          -e 's|^#baseurl=http://download1.rpmfusion.org/free/fedora/|baseurl=https://mirror1.hs-esslingen.de/pub/Mirrors/archive.rpmfusion.org/free-archive/fedora/|' \
          "$repo"
      done
      dnf clean metadata
    fi
    # Fedora's ffmpeg-free also provides "ffmpeg", but omits libx264/libx265.
    # Require RPM Fusion's library package as well so a newer restricted
    # provider cannot silently satisfy the fixture's software-codec dependency.
    dnf install -y --allowerasing ffmpeg ffmpeg-libs
  fi
fi
useradd -m -s /bin/bash kasm_test
