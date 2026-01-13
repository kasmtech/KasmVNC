#!/bin/bash

set -e

debian_patches_dir="debian/patches"

is_debian_patches_present() {
  [[ -d "$debian_patches_dir" ]]
}

is_debian() {
  [[ -f /usr/bin/dpkg ]]
}

apply_debian_patches() {
  if is_debian_patches_present; then
    export QUILT_PATCHES="$debian_patches_dir"
    quilt push -a
    echo 'Patches applied!'
  fi
}

fail_on_gcc_12() {
  if [[ -n "$CC" && -n "$CXX" ]]; then
    return;
  fi

  if gcc --version | head -1 | grep -q 12; then
    cat >&2 <<EOF

Error: gcc 12 detected. It has a bug causing the build to fall because of a
-Warray-bounds bug. Please use gcc 11 in the build Dockerfile:
ENV CC=gcc-11
ENV CXX=g++-11
RUN <install gcc 11>
EOF
  exit 1
  fi
}

# For build-dep to work, the apt sources need to have the source server
#sudo apt-get build-dep xorg-server

#sudo apt-get install cmake git libjpeg-dev libgnutls-dev

# Gcc12 builds fail due to bug
#fail_on_gcc_12

# Ubuntu applies a million patches, but here we use upstream to simplify matters
cd /tmp
# default to the version of x in Ubuntu 18.04, otherwise caller will need to specify
XORG_VER=${XORG_VER:-"1.19.6"}
if [[ "${XORG_VER}" == 21* ]]; then
  XORG_PATCH=21
else
  XORG_PATCH=$(echo "$XORG_VER" | grep -Po '^\d.\d+' | sed 's#\.##')
fi

TARBALL="xorg-server-${XORG_VER}.tar.gz"

if [ ! -f "$TARBALL" ]; then
  wget --no-check-certificate https://www.x.org/archive/individual/xserver/"$TARBALL"
fi

#git clone https://kasmweb@bitbucket.org/kasmtech/kasmvnc.git
#cd kasmvnc
#git checkout dynjpeg
cd /src

cmake -D CMAKE_BUILD_TYPE=RelWithDebInfo . -DBUILD_VIEWER:BOOL=OFF \
  -DENABLE_GNUTLS:BOOL=OFF
make -j"$(nproc)"

if [ ! -d unix/xserver/include ]; then
  tar -C unix/xserver -xf /tmp/"$TARBALL" --strip-components=1

  cd unix/xserver
  # Apply patches
  patch -Np1 -i ../xserver"${XORG_PATCH}".patch
  case "$XORG_VER" in
    1.20.*)
        patch -s -p0 < ../CVE-2022-2320-v1.20.patch
        if [ -f ../xserver120.7.patch ]; then
          patch -Np1 -i ../xserver120.7.patch
        fi ;;
    1.19.*)
        patch -s -p0 < ../CVE-2022-2320-v1.19.patch
        ;;
  esac
else
  cd unix/xserver
fi

autoreconf -i
# Configuring Xorg is long and has many distro-specific paths.
# The distro paths start after prefix and end with the font path,
# everything after that is based on BUILDING.txt to remove unneeded
# components.
# remove gl check for opensuse
if [ "${KASMVNC_BUILD_OS}" == "opensuse" ] || ([ "${KASMVNC_BUILD_OS}" == "oracle" ] && [ "${KASMVNC_BUILD_OS_CODENAME}" == 9 ]); then
  sed -i 's/LIBGL="gl >= 7.1.0"/LIBGL="gl >= 1.1"/g' configure
fi

# build X11
./configure \
    --disable-config-hal \
    --disable-config-udev \
    --disable-dmx \
    --disable-dri \
    --disable-dri2 \
    --disable-kdrive \
    --disable-static \
    --disable-xephyr \
    --disable-xinerama \
    --disable-xnest \
    --disable-xorg \
    --disable-xvfb \
    --disable-xwayland \
    --disable-xwin \
    --enable-glx \
    --prefix=/opt/kasmweb \
    --with-default-font-path="/usr/share/fonts/X11/misc,/usr/share/fonts/X11/cyrillic,/usr/share/fonts/X11/100dpi/:unscaled,/usr/share/fonts/X11/75dpi/:unscaled,/usr/share/fonts/X11/Type1,/usr/share/fonts/X11/100dpi,/usr/share/fonts/X11/75dpi,built-ins" \
    --without-dtrace \
    --with-sha1=libcrypto \
    --with-xkb-bin-directory=/usr/bin \
    --with-xkb-output=/var/lib/xkb \
    --with-xkb-path=/usr/share/X11/xkb "${CONFIG_OPTIONS}"

# remove array bounds errors for new versions of GCC
find . -name "Makefile" -exec sed -i 's/-Werror=array-bounds//g' {} \;


make -j"$(nproc)"

# modifications for the servertarball
cd /src
mkdir -p xorg.build/bin
mkdir -p xorg.build/lib
cd xorg.build/bin/
ln -sfn /src/unix/xserver/hw/vnc/Xvnc Xvnc
cd ..
mkdir -p man/man1
touch man/man1/Xserver.1
cp /src/unix/xserver/hw/vnc/Xvnc.man man/man1/Xvnc.1

mkdir -p lib

cd lib
if [ -d /usr/lib/x86_64-linux-gnu/dri ]; then
  ln -sfn /usr/lib/x86_64-linux-gnu/dri dri
elif [ -d /usr/lib/aarch64-linux-gnu/dri ]; then
  ln -sfn /usr/lib/aarch64-linux-gnu/dri dri
elif [ -d /usr/lib/arm-linux-gnueabihf/dri ]; then
  ln -sfn /usr/lib/arm-linux-gnueabihf/dri dri
elif [ -d /usr/lib/xorg/modules/dri ]; then
  ln -sfn /usr/lib/xorg/modules/dri dri
else
  ln -sfn /usr/lib64/dri dri
fi
cd /src

if is_debian; then
  apply_debian_patches
fi

make servertarball

cp kasmvnc*.tar.gz /build/kasmvnc.${KASMVNC_BUILD_OS}_${KASMVNC_BUILD_OS_CODENAME}.tar.gz
if [ "$BUILD_TAG" = "+libjpeg-turbo_latest" ]; then
	mkdir -p /build/${KASMVNC_BUILD_OS_CODENAME}/
	cp /libjpeg-turbo/libjpeg*.deb /build/${KASMVNC_BUILD_OS_CODENAME}/
fi
