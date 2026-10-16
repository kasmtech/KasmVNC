{ pkgs, src, webAssets, revision ? "local", version ? "1.5.1" }:
let
  inherit (pkgs) lib;
  ffmpeg = pkgs.ffmpeg_6-headless;
  perl = pkgs.perl.withPackages (p: with p; [ Switch YAMLTiny HashMergeSimple ListMoreUtils TryTiny DateTime DateTimeTimeZone ]);
  helpers = with pkgs; [ bash coreutils findutils gnugrep gnused gawk procps hostname openssl xorg.xauth xorg.xkbcomp xorg.setxkbmap ];
  xserver = pkgs.fetchurl {
    url = "https://www.x.org/archive/individual/xserver/xorg-server-21.1.12.tar.xz";
    sha256 = "03x954bygi6sdynk5yy3yvsfhg6i9gjhisn3x9jxvk5mw4mnw08y";
  };
in pkgs.stdenv.mkDerivation {
  pname = "kasmvnc";
  inherit version src;
  nativeBuildInputs = with pkgs; [ cmake pkg-config autoconf automake libtool makeWrapper bison flex xorg.fontutil python3 xorg.utilmacros xorg.xorgproto ];
  buildInputs = with pkgs; [
    libjpeg_turbo libwebp libpng libtiff giflib zlib openssl libxcrypt-legacy
    mesa-gl-headers libxcvt dri-pkgconfig-stub libtirpc libepoxy freetype fontconfig pixman libunwind libGL libgbm libdrm libva
    ffmpeg libcpuid fmt tbb libyuv libstatgrab at-spi2-core dbus glib
    xorg.libXi xorg.libX11 xorg.libXext xorg.libXtst xorg.libXrandr xorg.libXcursor
    xorg.libXfont2 xorg.libxshmfence xorg.libpciaccess xorg.libxkbfile
    xorg.libSM xorg.libICE xorg.libxcb xorg.libXdamage xorg.libXfixes
    xorg.libXau xorg.libXdmcp xorg.xtrans
  ];
  dontUseCmakeConfigure = true;
  enableParallelBuilding = true;
  KASMVNC_COMMIT_ID = revision;
  # dlopen dependencies must survive --as-needed and closure collection.
  NIX_LDFLAGS = "-rpath ${lib.getLib ffmpeg}/lib";
  postPatch = ''
    # Nix supplies these libraries; do not force the distro build's static JPEG.
    substituteInPlace CMakeLists.txt \
      --replace-fail 'cmake_policy(SET CMP0022 OLD)' 'cmake_policy(SET CMP0022 NEW)' \
      --replace-fail 'set(JPEG_LIBRARIES "-Wl,-Bstatic -lturbojpeg -Wl,-Bdynamic")' 'set(JPEG_LIBRARIES "${lib.getLib pkgs.libjpeg_turbo}/lib/libturbojpeg.so;${lib.getLib pkgs.libjpeg_turbo}/lib/libjpeg.so")'
    substituteInPlace unix/vncserver \
      --replace-fail '$0 =~ m!^/usr!;' '1;' \
      --replace-fail '/usr/share/kasmvnc' "$out/share/kasmvnc" \
      --replace-fail '/usr/lib/kasmvncserver/select-de.sh' "$out/lib/kasmvncserver/select-de.sh"
    tar -xf ${xserver} -C unix/xserver --strip-components=1
    (cd unix/xserver; patch -p1 < ../xserver21.patch)
    patchShebangs unix/xserver
  '';
  configurePhase = ''
    runHook preConfigure
    cmake . -DCMAKE_INSTALL_PREFIX="$out" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_GNUTLS=OFF -DENABLE_PAM=OFF -DATSPI_DEBUG=OFF
    runHook postConfigure
  '';
  buildPhase = ''
    runHook preBuild
    make -j"$NIX_BUILD_CORES"
    cd unix/xserver
    autoreconf -fi
    ./configure --prefix="$out" \
      --disable-config-hal --disable-config-udev --disable-dmx --disable-dri --disable-dri2 \
      --disable-kdrive --disable-static --disable-xephyr --disable-xinerama --disable-xnest \
      --disable-xorg --disable-xvfb --disable-xwayland --disable-xwin --enable-glx \
      --without-dtrace --with-sha1=libcrypto \
      --with-default-font-path=built-ins \
      --with-xkb-bin-directory=${pkgs.xorg.xkbcomp}/bin \
      --with-xkb-output=/tmp --with-xkb-path=${pkgs.xkeyboard_config}/share/X11/xkb
    make -j"$NIX_BUILD_CORES"
    cd ../..
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p "$out/bin" "$out/share/kasmvnc" "$out/share/perl5" "$out/lib/kasmvncserver" "$out/etc/kasmvnc"
    install -m755 unix/xserver/hw/vnc/Xvnc "$out/bin/Xkasmvnc"
    install -m755 unix/vncserver "$out/bin/kasmvncserver"
    install -m755 unix/kasmvncpasswd/kasmvncpasswd "$out/bin/"
    install -m755 unix/vncconfig/vncconfig "$out/bin/kasmvncconfig"
    install -m755 unix/kasmxproxy/kasmxproxy "$out/bin/"
    cp -r unix/KasmVNC "$out/share/perl5/"
    cp unix/kasmvnc_defaults.yaml "$out/share/kasmvnc/"
    cp unix/kasmvnc.yaml "$out/etc/kasmvnc/"
    cp -r ${webAssets}/. "$out/share/kasmvnc/www"
    install -m755 builder/startup/deb/select-de.sh "$out/lib/kasmvncserver/"
    substituteInPlace "$out/bin/kasmvncserver" --replace-fail '#!/usr/bin/perl' '#!${perl}/bin/perl'
    wrapProgram "$out/bin/kasmvncserver" \
      --prefix PERL5LIB : "$out/share/perl5" \
      --prefix PATH : "$out/bin:${lib.makeBinPath helpers}" \
      --set-default XKB_CONFIG_ROOT ${pkgs.xkeyboard_config}/share/X11/xkb
    for pair in 'Xkasmvnc Xvnc' 'kasmvncserver vncserver' 'kasmvncpasswd vncpasswd' 'kasmvncconfig vncconfig'; do
      set -- $pair
      ln -s "$1" "$out/bin/$2"
    done
    runHook postInstall
  '';
  postFixup = ''
    # Keep dlopen libraries after the standard shrink-rpath fixup.
    patchelf --add-rpath ${lib.getLib ffmpeg}/lib "$out/bin/Xkasmvnc"
    wrapProgram "$out/bin/Xkasmvnc" \
      --unset LD_PRELOAD \
      --set LD_LIBRARY_PATH /run/opengl-driver/lib \
      --set-default LIBGL_DRIVERS_PATH ${pkgs.mesa}/lib/dri
  '';
  doInstallCheck = true;
  installCheckPhase = ''
    "$out/bin/Xvnc" -version
    ${perl}/bin/perl -I"$out/share/perl5" -c "$out/bin/.kasmvncserver-wrapped"
    test -s "$out/share/kasmvnc/www/index.html"
    # Default libxcrypt disables SHA-256 crypt; KasmVNC requires this format.
    export HOME="$TMPDIR/auth-test"
    mkdir -p "$HOME"
    printf 'nix-test-password\nnix-test-password\n' | "$out/bin/kasmvncpasswd" -u nix-test -wo
    grep -q '^nix-test:\$5\$' "$HOME/.kasmpasswd"
  '';
  meta = {
    description = "KasmVNC server built from source with a Nix runtime closure";
    homepage = "https://github.com/kasmtech/KasmVNC";
    platforms = [ "x86_64-linux" "aarch64-linux" ];
    mainProgram = "kasmvncserver";
  };
}
