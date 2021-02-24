Name:           kasmvncserver
Version:        0.9.1~beta
Release:        1%{?dist}
Summary:        VNC server accessible from a web browser

License: GPLv2+
URL: https://github.com/kasmtech/KasmVNC

BuildRequires: rsync
Requires: xorg-x11-xauth, xorg-x11-xkb-utils, xkeyboard-config, xorg-x11-server-utils, openssl, perl
Conflicts: tigervnc-server, tigervnc-server-minimal

%description
VNC stands for Virtual Network Computing. It is, in essence, a remote
display system which allows you to view a computing `desktop' environment
not only on the machine where it is running, but from anywhere on the
Internet and from a wide variety of machine architectures.

KasmVNC has different goals than TigerVNC:

Web-based - KasmVNC is designed to provide a web accessible remote desktop.
It comes with a web server and web-socket server built in. There is no need to
install other components. Simply run and navigate to your desktop's URL on the
port you specify. While you can still tun on the legacy VNC port, it is
disabled by default.

Security - KasmVNC defaults to HTTPS and allows for HTTP Basic Auth. VNC
Password authentication is limited by specification to 8 characters and is not
sufficient for use on an internet accessible remote desktop. Our goal is to
create a by default secure, web based experience.

Simplicity - KasmVNC aims at being simple to deploy and configure.

%prep

%install
rm -rf $RPM_BUILD_ROOT
DESTDIR=$RPM_BUILD_ROOT make -f /src/debian/Makefile.to_fakebuild_tar_package install
cd $RPM_BUILD_ROOT/usr/bin && mv Xkasmvnc Xvnc && \
  mv kasmvncserver vncserver && mv kasmvncconfig vncconfig && \
  ln -s kasmvncpasswd vncpasswd
cd $RPM_BUILD_ROOT/usr/share/man/man1 && mv Xkasmvnc.1 Xvnc.1 && \
  mv kasmvncserver.1 vncserver.1 && mv kasmvncconfig.1 vncconfig.1 && \
  cp kasmvncpasswd.1 vncpasswd.1

%files
/usr/bin/*
/usr/share/man/man1/*
/usr/share/kasmvnc/www

%license /usr/share/doc/kasmvncserver/LICENCE.TXT
%doc /usr/share/doc/kasmvncserver/README.md

%changelog
* Fri Feb 12 2021 KasmTech <info@kasmweb.com> - 0.9.1~beta-1
- Initial release of the rpm package.

%post
  kasmvnc_group="kasmvnc-cert"

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
  rm -f /etc/pki/tls/private/kasmvnc.pem
