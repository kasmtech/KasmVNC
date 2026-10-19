#!/usr/bin/env bash
# Only for disposable package-validation containers. Runtime package dependencies
# are installed by the package manager; tar installations need them explicitly.
set -euo pipefail
format=${1:?package format required}
stage=${2:-all}
. /etc/os-release
export DEBIAN_FRONTEND=noninteractive
if command -v apt-get >/dev/null; then
  apt-get update
  if [[ "$stage" = bootstrap ]]; then
    apt-get install -y --no-install-recommends ca-certificates
    exit 0
  fi
  apt-get install -y --no-install-recommends ca-certificates curl openssl procps \
    python3 openbox xterm x11-utils dbus-x11 util-linux binutils
  if [[ "$format" = tar ]]; then
    apt-get install -y --no-install-recommends perl ssl-cert xauth x11-xkb-utils \
      xkb-data libswitch-perl libyaml-tiny-perl libhash-merge-simple-perl \
      libscalar-list-utils-perl liblist-moreutils-perl libtry-tiny-perl \
      libdatetime-perl libdatetime-timezone-perl libgl1 libgbm1
  fi
elif command -v zypper >/dev/null; then
  zypper --non-interactive refresh
  if [[ "$stage" = bootstrap ]]; then
    zypper --non-interactive install --no-recommends ca-certificates
    exit 0
  fi
  zypper --non-interactive install --no-recommends ca-certificates curl openssl \
    procps python3 openbox xterm xdpyinfo dbus-1-x11 util-linux shadow binutils tar gzip
  if [[ "$format" = tar ]]; then
    zypper --non-interactive install --no-recommends perl 'perl(lib)' xauth xkeyboard-config \
      xkbcomp perl-Switch perl-YAML-Tiny perl-Hash-Merge-Simple \
      perl-Scalar-List-Utils perl-List-MoreUtils perl-Try-Tiny perl-DateTime \
      perl-DateTime-TimeZone libglvnd libgbm1 hostname
  fi
else
  # EL9 needs EPEL for the same Perl/desktop prerequisites as existing packages.
  if [[ "$stage" != tools ]]; then
  case "$ID" in
    ol) dnf install -y oracle-epel-release-el9 dnf-plugins-core; dnf config-manager --set-enabled ol9_codeready_builder ;;
    rocky|almalinux) dnf install -y epel-release dnf-plugins-core; dnf config-manager --set-enabled crb ;;
    rhel) dnf install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm ;;
  esac
  fi
  if [[ "$stage" = bootstrap ]]; then
    dnf install -y ca-certificates
    exit 0
  fi
  dnf install -y ca-certificates curl openssl procps-ng python3 openbox xterm \
    '/usr/bin/xdpyinfo' dbus-x11 util-linux shadow-utils binutils tar gzip
  if [[ "$format" = tar ]]; then
    dnf install -y perl 'perl(lib)' xorg-x11-xauth xkeyboard-config xkbcomp perl-Switch \
      perl-YAML-Tiny perl-Hash-Merge-Simple perl-Scalar-List-Utils \
      perl-List-MoreUtils perl-Try-Tiny perl-DateTime perl-DateTime-TimeZone \
      libglvnd-glx mesa-libgbm hostname
  fi
fi
useradd -m -s /bin/bash kasm_test
