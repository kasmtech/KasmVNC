Name:           kasmvncserver
Version:        0.9.1~beta
Release:        1%{?dist}
Summary:        Lorem ipsum

License: GPLv2+
URL: https://github.com/kasmtech/KasmVNC

BuildRequires: rsync
Requires: xorg-x11-xauth, xorg-x11-xkb-utils, xkeyboard-config, xorg-x11-server-utils, openssl

%description

Lorem ipsum

%prep

%install
rm -rf $RPM_BUILD_ROOT
DESTDIR=$RPM_BUILD_ROOT make -f /src/debian/Makefile.to_fakebuild_tar_package install

%files
/usr/bin/*
/usr/share/man/man1
/usr/share/kasmvnc/www

%doc /usr/share/doc/kasmvncserver

%changelog

%post
  bindir=/usr/bin
  mandir=/usr/share/man
  commands="kasmvncserver kasmvncpasswd kasmvncconfig Xkasmvnc"

  for kasm_command in $commands; do
    generic_command=`echo "$kasm_command" | sed -e 's/kasm//'`;
    update-alternatives --install "$bindir/$generic_command" \
      "$generic_command" "$bindir/$kasm_command" 90 \
      --slave "$mandir/man1/$generic_command.1.gz" "$generic_command.1.gz" \
        "$mandir/man1/$kasm_command.1.gz"
  done

  kasmvnc_group="kasmvnc"

  create_kasmvnc_group() {
    if ! getent group "$kasmvnc_group" >/dev/null; then
	    groupadd --system "$kasmvnc_group"
    fi
  }

  make_self_signed_certificate() {
    local cert_file=/etc/pki/tls/private/kasmvnc.pem
    [ -f "$cert_file" ] && return 0

    openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
      -keyout "$cert_file" \
      -out "$cert_file" -subj \
      "/C=US/ST=VA/L=None/O=None/OU=DoFu/CN=kasm/emailAddress=none@none.none"
    chgrp "$kasmvnc_group" "$cert_file"
    chmod 640 "$cert_file"
  }

  create_kasmvnc_group
  make_self_signed_certificate

%postun
  bindir=/usr/bin
  mandir=/usr/share/man
  commands="kasmvncserver kasmvncpasswd kasmvncconfig Xkasmvnc"

  for kasm_command in $commands; do
    generic_command=`echo "$kasm_command" | sed -e 's/kasm//'`;
    update-alternatives --remove "$generic_command" "$bindir/$kasm_command"
  done

  rm -f /etc/pki/tls/private/kasmvnc.pem
