#!/bin/sh -e

detect_quilt() {
  if which quilt 1>/dev/null; then
    QUILT_PRESENT=1
    export QUILT_PATCHES=debian/patches
  fi
}

# For build-dep to work, the apt sources need to have the source server
#sudo apt-get build-dep xorg-server

#sudo apt-get install cmake git libjpeg-dev libgnutls-dev

# Ubuntu applies a million patches, but here we use upstream to simplify matters
cd /tmp
# default to the version of x in Ubuntu 18.04, otherwise caller will need to specify
XORG_VER=${XORG_VER:-"1.19.6"}
XORG_PATCH=$(echo "$XORG_VER" | grep -Po '^\d.\d+' | sed 's#\.##')
wget https://www.x.org/archive/individual/xserver/xorg-server-${XORG_VER}.tar.bz2

#git clone https://kasmweb@bitbucket.org/kasmtech/kasmvnc.git
#cd kasmvnc
#git checkout dynjpeg
cd /src

# We only want the server, so FLTK and manual tests aren't useful.
# Alternatively, install fltk 1.3 and its dev packages.
sed -i -e '/find_package(FLTK/s@^@#@' \
	-e '/add_subdirectory(tests/s@^@#@' \
	CMakeLists.txt

cmake -D CMAKE_BUILD_TYPE=RelWithDebInfo .
make -j5

tar -C unix/xserver -xvf /tmp/xorg-server-${XORG_VER}.tar.bz2 --strip-components=1

cd unix/xserver
patch -Np1 -i ../xserver${XORG_PATCH}.patch
if [[ $XORG_VER =~ ^1\.20\..*$ ]]; then
  patch -Np1 -i ../xserver120.7.patch
fi

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
cp /src/unix/xserver/hw/vnc/Xvnc.man man/man1/Xvnc.1
mkdir lib
cd lib
if [ -d /usr/lib/x86_64-linux-gnu/dri ]; then
  ln -s /usr/lib/x86_64-linux-gnu/dri dri
else
  ln -s /usr/lib64/dri dri
fi
cd /src

detect_quilt
if [ -n "$QUILT_PRESENT" ]; then
  quilt push -a
fi
make servertarball

cp kasmvnc*.tar.gz /build/kasmvnc.${KASMVNC_BUILD_OS}_${KASMVNC_BUILD_OS_CODENAME}.tar.gz
