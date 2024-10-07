Name:           kasmvncserver
Version:        1.3.2
Release:        1%{?dist}
Summary:        VNC server accessible from a web browser

License: GPLv2+
URL: https://github.com/kasmtech/KasmVNC

BuildRequires: rsync
Requires: xorg-x11-xauth, xorg-x11-xkb-utils, xkeyboard-config, xorg-x11-server-utils, openssl, perl, perl-Switch, perl-YAML-Tiny, perl-Hash-Merge-Simple, perl-Scalar-List-Utils, perl-List-MoreUtils, perl-Try-Tiny, perl-DateTime-TimeZone, hostname, mesa-libgbm, libxshmfence
Conflicts: tigervnc-server, tigervnc-server-minimal

%description
KasmVNC provides remote web-based access to a Desktop or application.
While VNC is in the name, KasmVNC differs from other VNC variants such
as TigerVNC, RealVNC, and TurboVNC. KasmVNC has broken from the RFB
specification which defines VNC, in order to support modern technologies
and increase security. KasmVNC is accessed by users from any modern
browser and does not support legacy VNC viewer applications. KasmVNC
uses a modern YAML based configuration at the server and user level,
allowing for ease of management. KasmVNC is maintained by Kasm
Technologies Corp, www.kasmweb.com.

WARNING: this package requires EPEL and CodeReady builder.

%prep

%install
rm -rf $RPM_BUILD_ROOT

TARGET_OS=$KASMVNC_BUILD_OS
TARGET_OS_CODENAME=$KASMVNC_BUILD_OS_CODENAME
TARBALL=$RPM_SOURCE_DIR/kasmvnc.${TARGET_OS}_${TARGET_OS_CODENAME}.tar.gz
TAR_DATA=$(mktemp -d)
tar -xzf "$TARBALL" -C "$TAR_DATA"

SRC=$TAR_DATA/usr/local
SRC_BIN=$SRC/bin
DESTDIR=$RPM_BUILD_ROOT
DST_MAN=$DESTDIR/usr/share/man/man1

mkdir -p $DESTDIR/usr/bin $DESTDIR/usr/share/man/man1 \
  $DESTDIR/usr/share/doc/kasmvncserver $DESTDIR/usr/lib \
  $DESTDIR/usr/share/perl5 $DESTDIR/etc/kasmvnc
cp $SRC_BIN/Xvnc $DESTDIR/usr/bin;
cp $SRC_BIN/vncserver $DESTDIR/usr/bin;
cp -a $SRC_BIN/KasmVNC $DESTDIR/usr/share/perl5
cp $SRC_BIN/vncconfig $DESTDIR/usr/bin;
cp $SRC_BIN/kasmvncpasswd $DESTDIR/usr/bin;
cp $SRC_BIN/kasmxproxy $DESTDIR/usr/bin;
cp -r $SRC/lib/kasmvnc/ $DESTDIR/usr/lib/kasmvncserver
cd $DESTDIR/usr/bin && ln -s kasmvncpasswd vncpasswd;
cp -r $SRC/share/doc/kasmvnc*/* $DESTDIR/usr/share/doc/kasmvncserver/
rsync -r --links --safe-links --exclude '.git*' --exclude po2js --exclude xgettext-html \
  --exclude www/utils/ --exclude .eslintrc --exclude configure \
  $SRC/share/kasmvnc $DESTDIR/usr/share

sed -i -e 's!pem_certificate: .\+$!pem_certificate: /etc/pki/tls/private/kasmvnc.pem!' \
    $DESTDIR/usr/share/kasmvnc/kasmvnc_defaults.yaml
sed -i -e 's!pem_key: .\+$!pem_key: /etc/pki/tls/private/kasmvnc.pem!' \
    $DESTDIR/usr/share/kasmvnc/kasmvnc_defaults.yaml
sed -e 's/^\([^#]\)/# \1/' $DESTDIR/usr/share/kasmvnc/kasmvnc_defaults.yaml > \
  $DESTDIR/etc/kasmvnc/kasmvnc.yaml
cp $SRC/man/man1/Xvnc.1 $DESTDIR/usr/share/man/man1/;
cp $SRC/share/man/man1/vncserver.1 $DST_MAN;
cp $SRC/share/man/man1/vncconfig.1 $DST_MAN;
cp $SRC/share/man/man1/vncpasswd.1 $DST_MAN;
cp $SRC/share/man/man1/kasmxproxy.1 $DST_MAN;
cd $DST_MAN && ln -s vncpasswd.1 kasmvncpasswd.1;


%files
%config(noreplace) /etc/kasmvnc

/usr/bin/*
/usr/lib/kasmvncserver
/usr/share/man/man1/*
/usr/share/perl5/KasmVNC
/usr/share/kasmvnc

%license /usr/share/doc/kasmvncserver/LICENSE.TXT
%doc /usr/share/doc/kasmvncserver/README.md

%changelog
* Tue Sep 24 2024 KasmTech <info@kasmweb.com> - 1.3.2-1
- Disable seamless clipboard on Firefox by default, due to the Firefox overlaying a Paste menu over the canvas.
- Fixed CVE-2024-38449, directory traversal bug in built-in web server.
- Allow for larger header sizes, up to 16k. Provide better logging and handling for requests that contain HTTP headers that are larger than the 16k limit.
- Fixed memory leak in kasmproxy.
- Fixed mime types of downloads to ensure the browser interprets them as downloads.
* Tue Mar 12 2024 KasmTech <info@kasmweb.com> - 1.3.1-1
- Fix exception thrown on Firefox 124 and higher
- Fix artifacts on high resolution secondary screens
- Fixes for touch support on primary and secondary screens
- Fix for Oculus keyboard input
* Mon Feb 05 2024 KasmTech <info@kasmweb.com> - 1.3.0-1
- Multi-monitor support.
- Increased performance with watermark enabled.
- Added support for Fedora 39 and Alpine 319.
- Allow special characters in usernames.
- Better logging of client settings when client connects or changes settings.
- Add support for rotation of text-based watermark.
* Fri Aug 25 2023 KasmTech <info@kasmweb.com> - 1.2.0-1
- Add support for Unix relays for bidirectional communication between noVNC
  and containerized applications.
- Text based watermark overlays with date and time support.
- New builds for Bookworm, Alpine 3.18, and Fedora 38.
- Multi-language support.
- Add support for rendering pixmaps via DRI3 GPU acceleration allowing
  compositing and other 3d accelerated workloads in a KasmVNC session.
- Fix crash that can occur.
- Fixed tearing when compositing is enabled with DRI3 hardware acceleration.
- Fix stuck command key on MacOS clients.
* Wed Apr 05 2023 KasmTech <info@kasmweb.com> - 1.1.0-1
- Upstream release
* Tue Nov 29 2022 KasmTech <info@kasmweb.com> - 1.0.0-1
- WebRTC UDP transit support with support of STUN servers
- Lossless compression using multi-threaded WASM QOI decoder client side
- New yaml based configuration
- Significantly improved FPS through both client-side and server-side improvements.
- Support for the admin to define arbitrary http response headers for the built in web server
- Support for additional mouse buttons
- Refinement of vncserver checks and user prompts
- Added send_full_frame to developer API, forces full frame to be sent to all connected users that have at least read permission.
* Tue Mar 22 2022 KasmTech <info@kasmweb.com> - 0.9.3~beta-1
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
