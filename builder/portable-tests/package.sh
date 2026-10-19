#!/usr/bin/env bash
# Package lifecycle operations inside disposable test containers only.
set -euo pipefail
action=${1:?}; format=${2:?}; package=${3:-}
config=/etc/kasmvnc/kasmvnc.yaml
marker='# portable-ci-config-preservation'
install_package() {
  case "$format" in
    deb) apt-get install -y --reinstall -o Dpkg::Options::=--force-confold "$package" ;;
    rpm)
      if command -v zypper >/dev/null; then
        zypper --non-interactive --no-gpg-checks install --force "$package"
      else
        if rpm -q kasmvncserver >/dev/null 2>&1 && [[ "$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$package")" = "$(rpm -q --qf '%{VERSION}-%{RELEASE}' kasmvncserver)" ]]; then
          dnf reinstall -y "$package"
        else
          dnf install -y "$package"
        fi
      fi ;;
    tar)
      options=()
      if [[ -f "$config" ]]; then
        options+=(--exclude=etc/kasmvnc/kasmvnc.yaml --exclude=./etc/kasmvnc/kasmvnc.yaml)
      fi
      tar -xzf "$package" -C / "${options[@]}"
      /usr/lib/kasmvncserver/initialize-tls
      printf '%s\n' "$package" > /tmp/portable-tar-package ;;
  esac
}
case "$action" in
  install)
    install_package
    command -v kasmvncserver
    command -v Xkasmvnc
    perl -c /usr/bin/kasmvncserver
    for tool in xdpyinfo xauth xkbcomp openssl hostname; do command -v "$tool"; done
    Xkasmvnc -version
    test -s /usr/share/kasmvnc/www/index.html
    test -f "$config"
    if [[ "$format" = deb ]]; then
      test -s /etc/ssl/certs/ssl-cert-snakeoil.pem
      test -s /etc/ssl/private/ssl-cert-snakeoil.key
    else
      test -s /etc/pki/tls/private/kasmvnc.pem
      [[ $(stat -c '%a' /etc/pki/tls/private/kasmvnc.pem) = 640 ]]
      [[ $(stat -c '%G' /etc/pki/tls/private/kasmvnc.pem) = kasmvnc-cert ]]
    fi
    # Linker dependencies must resolve in the target, without builder libraries.
    ldd /usr/bin/Xkasmvnc > /tmp/portable-ldd.txt
    ! grep -q 'not found' /tmp/portable-ldd.txt
    ;;
  mark) printf '\n%s\n' "$marker" >> "$config" ;;
  upgrade)
    if [[ "$format" = deb ]]; then
      before=$(dpkg-query -W -f='${Version}' kasmvncserver)
      candidate=$(dpkg-deb -f "$package" Version)
      dpkg --compare-versions "$before" lt "$candidate"
    else
      before=$(rpm -q --qf '%{VERSION}-%{RELEASE}' kasmvncserver)
      candidate=$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$package")
      [[ "$before" != "$candidate" ]]
    fi
    install_package
    if [[ "$format" = deb ]]; then
      after=$(dpkg-query -W -f='${Version}' kasmvncserver)
    else
      after=$(rpm -q --qf '%{VERSION}-%{RELEASE}' kasmvncserver)
    fi
    [[ "$after" = "$candidate" && "$after" != "$before" ]]
    grep -Fq "$marker" "$config"
    printf 'Version transition: %s -> %s\n' "$before" "$after"
    ;;
  reinstall)
    install_package
    grep -Fq "$marker" "$config"
    ;;
  remove)
    case "$format" in
      deb) apt-get remove -y kasmvncserver ;;
      rpm) rpm -e kasmvncserver ;;
      tar)
        # Tar is a payload, not a package manager. Remove its listed files in
        # this disposable container, retaining administrator configuration.
        python3 - "$(cat /tmp/portable-tar-package)" <<'PYREMOVE'
import pathlib,sys,tarfile
with tarfile.open(sys.argv[1]) as archive:
    for member in archive:
        path=pathlib.PurePosixPath(member.name)
        if path.is_absolute() or '..' in path.parts:
            raise RuntimeError('Unsafe tar member')
        if path.parts and path.parts[0] == 'usr' and (member.isfile() or member.issym()):
            target=pathlib.Path('/')/path
            if target.is_symlink() or target.is_file():
                target.unlink()
PYREMOVE
        ;;
    esac
    test ! -e /usr/bin/Xkasmvnc
    # RPM may save a modified config as .rpmsave when removing the package.
    if ! grep -Fq "$marker" "$config" 2>/dev/null; then
      grep -Fq "$marker" "$config.rpmsave"
    fi
    ;;
  *) exit 2 ;;
esac
