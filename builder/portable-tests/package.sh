#!/usr/bin/env bash
# Package lifecycle operations inside disposable test containers only.
set -euo pipefail
# Each Docker RUN has a fresh environment; prerequisites.sh's setting does not
# reach package installation (notably tzdata on the minimal CUDA base).
export DEBIAN_FRONTEND=noninteractive
action=${1:?}; format=${2:?}; package=${3:-}
config=/etc/kasmvnc/kasmvnc.yaml
marker='# portable-ci-config-preservation'
apk_installed_version() {
  apk info --installed --verbose kasmvncserver | sed -n 's/^kasmvncserver-//p'
}
apk_candidate_version() {
  # Published APKs deliberately use format 2 for Alpine 3.21 compatibility.
  tar -xOf "$package" .PKGINFO | sed -n 's/^pkgver = //p'
}
select_apk_key() {
  local signing_key=/artifacts/kasmvnc_signing_key.rsa.pub
  if [[ "$package" = /artifacts/previous.apk && -f /artifacts/previous_signing_key.rsa.pub ]]; then
    signing_key=/artifacts/previous_signing_key.rsa.pub
  fi
  install -m 644 "$signing_key" /etc/apk/keys/kasmvnc_signing_key.rsa.pub
}
install_package() {
  case "$format" in
    apk)
      select_apk_key
      apk verify "$package"
      # Cache the signed local APK so apk fix exercises a real reinstall even
      # though this development artifact is not in an Alpine repository.
      mkdir -p /var/cache/apk
      [[ -e /etc/apk/cache ]] || ln -s /var/cache/apk /etc/apk/cache
      if [[ "$action" = reinstall ]]; then
        [[ "$(apk_installed_version)" = "$(apk_candidate_version)" ]]
        apk fix --no-network --reinstall kasmvncserver
      else
        apk add "$package"
      fi ;;
    deb) apt-get install -y --reinstall -o Dpkg::Options::=--force-confold "$package" ;;
    rpm)
      if command -v zypper >/dev/null; then
        ZYPP_CONF=/portable/zypp-with-docs.conf zypper --non-interactive --no-gpg-checks install --force "$package"
      else
        if rpm -q kasmvncserver >/dev/null 2>&1 && [[ "$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$package")" = "$(rpm -q --qf '%{VERSION}-%{RELEASE}' kasmvncserver)" ]]; then
          dnf --setopt=tsflags= reinstall -y "$package"
        else
          dnf --setopt=tsflags= install -y "$package"
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
verify_candidate() {
  command -v kasmvncserver
  command -v Xkasmvnc
  test "$(head -c4 /usr/bin/kasmvncserver)" = "$(printf '\177ELF')"
  test ! -d /usr/share/perl5/KasmVNC
  test ! -d /usr/share/perl5/vendor_perl/KasmVNC
  for tool in xdpyinfo xauth xkbcomp openssl; do command -v "$tool"; done
  test -s /usr/share/X11/xkb/rules/evdev
  Xkasmvnc -version
  test -s /usr/share/kasmvnc/www/index.html
  test -f "$config"
  if [[ "$format" = deb ]]; then
    test -s /etc/ssl/certs/ssl-cert-snakeoil.pem
    test -s /etc/ssl/private/ssl-cert-snakeoil.key
  else
    tls=/etc/pki/tls/private/kasmvnc.pem
    [[ "$format" != apk ]] || tls=/etc/ssl/private/kasmvnc.pem
    test -s "$tls"
    [[ $(stat -c '%a' "$tls") = 640 ]]
    [[ $(stat -c '%G' "$tls") = kasmvnc-cert ]]
  fi
  # Linker dependencies must resolve in the target, without builder libraries.
  ldd /usr/bin/Xkasmvnc /usr/bin/kasmvncserver /usr/bin/kasmvncconfig \
    /usr/bin/kasmvncpasswd /usr/bin/kasmxproxy > /tmp/portable-ldd.txt
  ! grep -q 'not found' /tmp/portable-ldd.txt
}
# A legacy native package can still have the Perl launcher and old TLS setup.
# Validate its actual package identity/config before applying candidate audits.
verify_previous() {
  test -s "$config"
  case "$format" in
    rpm)
      [[ "$(rpm -qp --qf '%{NAME}' "$package")" = kasmvncserver ]]
      [[ "$(rpm -qp --qf '%{ARCH}' "$package")" = "$(rpm --eval '%{_arch}')" ]]
      [[ "$(rpm -q --qf '%{VERSION}-%{RELEASE}.%{ARCH}' kasmvncserver)" = "$(rpm -qp --qf '%{VERSION}-%{RELEASE}.%{ARCH}' "$package")" ]] ;;
    deb)
      [[ "$(dpkg-deb -f "$package" Package)" = kasmvncserver ]]
      [[ "$(dpkg-deb -f "$package" Architecture)" = "$(dpkg --print-architecture)" ]]
      [[ "$(dpkg-query -W -f='${Version}' kasmvncserver)" = "$(dpkg-deb -f "$package" Version)" ]] ;;
    apk)
      [[ "$(apk_installed_version)" = "$(apk_candidate_version)" ]]
      apk info --exists kasmvncserver ;;
    *) echo "Previous package requires a native package format" >&2; exit 2 ;;
  esac
}
case "$action" in
  install-previous)
    install_package
    verify_previous
    ;;
  install)
    install_package
    verify_candidate
    ;;
  mark)
    printf '\n%s\n' "$marker" >> "$config" ;;
  verify-security)
    # Verify the installed assessment follows the package across every distro
    # and format, not just the sidecar artifacts uploaded by the build job.
    python3 - <<'PY'
import json
from pathlib import Path
p = Path('/usr/share/doc/kasmvncserver')
snapshot = json.loads((p / 'security-assessments.json').read_text())
bom = json.loads((p / 'sbom.cdx.json').read_text())
embedded = json.loads(next(v['value'] for v in bom['metadata']['properties']
                          if v['name'] == 'kasmvnc:security-assessments'))
assert embedded['assessments'] == snapshot['assessments']
vex = json.loads((p / 'security.openvex.json').read_text())
assert vex['statements']
assert (p / 'SECURITY-REVIEW.md').is_file()
print('Installed SBOM/VEX/assessment consistency PASS')
PY
    ;;
  upgrade)
    if [[ "$format" = deb ]]; then
      before=$(dpkg-query -W -f='${Version}' kasmvncserver)
      candidate=$(dpkg-deb -f "$package" Version)
      dpkg --compare-versions "$before" lt "$candidate"
    elif [[ "$format" = apk ]]; then
      select_apk_key
      apk verify "$package"
      before=$(apk_installed_version)
      candidate=$(apk_candidate_version)
      [[ "$(apk version -t "$before" "$candidate")" = '<' ]]
    else
      before=$(rpm -q --qf '%{VERSION}-%{RELEASE}' kasmvncserver)
      candidate=$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$package")
      [[ "$before" != "$candidate" ]]
    fi
    install_package
    if [[ "$format" = deb ]]; then
      after=$(dpkg-query -W -f='${Version}' kasmvncserver)
    elif [[ "$format" = apk ]]; then
      after=$(apk_installed_version)
    else
      after=$(rpm -q --qf '%{VERSION}-%{RELEASE}' kasmvncserver)
    fi
    [[ "$after" = "$candidate" && "$after" != "$before" ]]
    verify_candidate
    grep -Fq "$marker" "$config"
    printf 'Version transition: %s -> %s\n' "$before" "$after"
    ;;
  reinstall)
    install_package
    grep -Fq "$marker" "$config"
    ;;
  remove)
    case "$format" in
      apk) apk del kasmvncserver ;;
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
      if [[ "$format" = apk ]]; then
        grep -Fq "$marker" "$config.apk-save"
      else
        grep -Fq "$marker" "$config.rpmsave"
      fi
    fi
    ;;
  *) exit 2 ;;
esac
