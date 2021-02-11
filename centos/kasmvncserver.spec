Name:           kasmvncserver
Version:        0.9.1~beta
Release:        1%{?dist}
Summary:        Lorem ipsum

License: GPLv2+
URL: https://github.com/kasmtech/KasmVNC
Source0: kasmvnc.centos_core.tar.gz

Requires: xorg-x11-xauth, xorg-x11-xkb-utils, xkeyboard-config, xorg-x11-server-utils, openssl

%description

Lorem ipsum

%prep

%install
rm -rf $RPM_BUILD_ROOT
DESTDIR=$RPM_BUILD_ROOT make -f /src/KasmVNC/debian/Makefile.to_fakebuild_tar_package install

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
;;

%postun
  bindir=/usr/bin
  mandir=/usr/share/man
  commands="kasmvncserver kasmvncpasswd kasmvncconfig Xkasmvnc"

  for kasm_command in $commands; do
    generic_command=`echo "$kasm_command" | sed -e 's/kasm//'`;
    update-alternatives --remove "$generic_command" "$bindir/$kasm_command"
  done
