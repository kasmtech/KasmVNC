# Portable glibc 2.28 static-application builder (native x86_64/aarch64).
FROM oraclelinux:8@sha256:9225108d00fb6611ebc4a5eced166f27d9dcfeaa539fb127be79425f0da5e745
ARG DEPENDENCY_CONTEXT_SHA
LABEL org.kasmweb.portable-dependency-key=$DEPENDENCY_CONTEXT_SHA
RUN test -n "$DEPENDENCY_CONTEXT_SHA" && dnf install -y dnf-plugins-core && \
    dnf config-manager --enable ol8_codeready_builder && \
    dnf install -y oracle-epel-release-el8 && \
    dnf install -y dpkg rpm-build gdb gcc-toolset-14 gcc gcc-c++ make cmake meson ninja-build git \
      curl wget tar xz bzip2 patch autoconf automake libtool pkgconf-pkg-config \
      python3.11 python3.11-pip python3.11-devel bison flex gperf \
      gettext-devel gmp-devel mpfr-devel libmpc-devel perl-core xorg-x11-util-macros xorg-x11-proto-devel \
      xorg-x11-xtrans-devel xorg-x11-font-utils libX11-devel libXau-devel \
      libXdmcp-devel libXfont2-devel libxkbfile-devel libXrandr-devel \
      libXtst-devel libXcursor-devel libSM-devel libuuid-devel at-spi2-core-devel dbus-devel \
      libva-devel libdrm-devel libxshmfence-devel mesa-libGL-devel mesa-libgbm-devel \
      pixman-devel freetype-devel zlib-devel libpng-devel libjpeg-turbo-devel \
      libtiff-devel libxml2-devel libmount-devel libcap-devel lz4-devel \
      libzstd-devel libgcrypt-devel libselinux-devel libunwind-devel openssl-devel \
      systemd-devel bzip2-devel brotli-devel libffi-devel pcre2-devel \
      libtool-ltdl-devel libxcrypt-devel glib2-static pcre-static \
      libselinux-static libsepol-static && \
    if [ "$(uname -m)" = x86_64 ]; then dnf install -y nasm; fi && \
    ln -s /usr/bin/python3.11 /usr/local/bin/python3 && \
    dnf clean all
# Bump repository-refresh.txt here to intentionally refresh the toolchain RPMs.
COPY . /opt/kasm-gcc-recipes/
ENV BUILD_JOBS=8
RUN /opt/kasm-gcc-recipes/build
