#!/usr/bin/env bash
# Only for disposable package-validation containers. Runtime package dependencies
# are installed by the package manager; tar installations need them explicitly.
set -euo pipefail
format=${1:?package format required}
stage=${2:-all}
. /etc/os-release
export DEBIAN_FRONTEND=noninteractive
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
  apt-get update
  if [[ "$stage" = bootstrap ]]; then
    apt-get install -y --no-install-recommends ca-certificates
    exit 0
  fi
  apt-get install -y --no-install-recommends ca-certificates curl openssl procps \
    python3 openbox xterm x11-utils dbus-x11 util-linux binutils mesa-utils libgl1-mesa-dri
  if [[ "$format" = tar ]]; then
    apt-get install -y --no-install-recommends bash tzdata ssl-cert xauth x11-xkb-utils \
      xkb-data libgl1 libgbm1
  fi
elif command -v zypper >/dev/null; then
  zypper --non-interactive refresh
  if [[ "$stage" = bootstrap ]]; then
    zypper --non-interactive install --no-recommends ca-certificates
    exit 0
  fi
  zypper --non-interactive install --no-recommends ca-certificates curl openssl \
    procps python3 openbox xterm xdpyinfo dbus-1-x11 util-linux shadow binutils tar gzip Mesa-dri Mesa-demo-x
  if [[ "$format" = tar ]]; then
    zypper --non-interactive install --no-recommends bash timezone xauth xkeyboard-config \
      xkbcomp libglvnd libgbm1 hostname
  fi
else
  # EL9 needs EPEL for the same desktop prerequisites as existing packages.
  if [[ "$stage" != tools ]]; then
  case "$ID" in
    ol) dnf install -y oracle-epel-release-el9 dnf-plugins-core; dnf config-manager --set-enabled ol9_codeready_builder ;;
    rocky|almalinux) dnf install -y epel-release dnf-plugins-core; dnf config-manager --set-enabled crb ;;
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
    dnf install -y bash tzdata xorg-x11-xauth xkeyboard-config xkbcomp \
      libglvnd-glx mesa-libgbm hostname
  fi
fi
useradd -m -s /bin/bash kasm_test
