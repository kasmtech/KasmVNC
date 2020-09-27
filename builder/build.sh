#!/bin/sh -e

# For build-dep to work, the apt sources need to have the source server
#sudo apt-get build-dep xorg-server

#sudo apt-get install cmake git libjpeg-dev libgnutls-dev

# Ubuntu applies a million patches, but here we use upstream to simplify matters
cd /tmp
wget https://www.x.org/archive/individual/xserver/xorg-server-1.19.6.tar.bz2

#git clone https://kasmweb@bitbucket.org/kasmtech/kasmvnc.git
#cd kasmvnc
#git checkout dynjpeg
cd /src

# We only want the server, so FLTK and manual tests aren't useful.
# Alternatively, install fltk 1.3 and its dev packages.
sed -i -e '/find_package(FLTK/s@^@#@' \
	-e '/add_subdirectory(tests/s@^@#@' \
	CMakeLists.txt

cmake .
make -j5

tar -C unix/xserver -xvf /tmp/xorg-server-1.19.6.tar.bz2 --strip-components=1

cd unix/xserver
patch -Np1 -i ../xserver119.patch
autoreconf -i
# Configuring Xorg is long and has many distro-specific paths.
# The distro paths start after prefix and end with the font path,
# everything after that is based on BUILDING.txt to remove unneeded
# components.
./configure --prefix=/opt/kasmweb \
	--with-xkb-path=/usr/share/X11/xkb \
	--with-xkb-output=/var/lib/xkb \
	--with-xkb-bin-directory=/usr/bin \
	--with-default-font-path="/usr/share/fonts/X11/misc,/usr/share/fonts/X11/cyrillic,/usr/share/fonts/X11/100dpi/:unscaled,/usr/share/fonts/X11/75dpi/:unscaled,/usr/share/fonts/X11/Type1,/usr/share/fonts/X11/100dpi,/usr/share/fonts/X11/75dpi,built-ins" \
	--with-pic --without-dtrace --disable-dri \
        --disable-static \
	--disable-xinerama --disable-xvfb --disable-xnest --disable-xorg \
	--disable-dmx --disable-xwin --disable-xephyr --disable-kdrive \
	--disable-config-hal --disable-config-udev \
	--disable-dri2 --enable-glx --disable-xwayland --disable-dri3
make -j5

# modifications for the servertarball
cd /src
mkdir -p xorg.build/bin
cd xorg.build/bin/
ln -s /src/unix/xserver/hw/vnc/Xvnc Xvnc
cd ..
mkdir -p man/man1
touch man/man1/Xserver.1
touch man/man1/Xvnc.1
mkdir lib
cd lib
ln -s /usr/lib/x86_64-linux-gnu/dri dri
cd /src
sed  $'s#pushd $TMPDIR/inst#CWD=$(pwd)\\\ncd $TMPDIR/inst#' release/maketarball > release/maketarball2
sed  $'s#popd#cd $CWD#' release/maketarball2 > release/maketarball3
mv release/maketarball3 release/maketarball

make servertarball

cp kasmvnc*.tar.gz /build/
