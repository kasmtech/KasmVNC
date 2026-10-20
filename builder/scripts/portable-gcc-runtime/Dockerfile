# Experimental glibc 2.34 static-application builder (native x86_64/aarch64).
FROM oraclelinux:9@sha256:09159fa3ed53f6950034daa5c48bc13cdf3d1323f23e17c324a5c3854f14aae1
ARG DEPENDENCY_CONTEXT_SHA
LABEL org.kasmweb.portable-dependency-key=$DEPENDENCY_CONTEXT_SHA
RUN test -n "$DEPENDENCY_CONTEXT_SHA" && dnf install -y dnf-plugins-core && \
    dnf config-manager --enable ol9_codeready_builder ol9_distro_builder && \
    dnf install -y oracle-epel-release-el9 && \
    dnf install -y dpkg rpm-build gdb gcc-toolset-14 gcc gcc-c++ make cmake meson ninja-build git \
      curl wget tar xz bzip2 patch autoconf automake libtool pkgconf-pkg-config \
      python3 python3-pip python3-devel python3-jinja2 bison flex gperf \
      gettext-devel gmp-devel mpfr-devel libmpc-devel perl-core xorg-x11-util-macros xorg-x11-proto-devel \
      xorg-x11-xtrans-devel xorg-x11-font-utils libX11-devel libXau-devel \
      libXdmcp-devel libXfont2-devel libxkbfile-devel libXrandr-devel \
      libxcvt-devel libXtst-devel libXcursor-devel libSM-devel libuuid-devel at-spi2-core-devel dbus-devel \
      libva-devel libdrm-devel libxshmfence-devel mesa-libGL-devel mesa-libgbm-devel \
      pixman-devel freetype-devel zlib-devel libpng-devel libjpeg-turbo-devel \
      libtiff-devel libxml2-devel libmount-devel libcap-devel lz4-devel \
      libzstd-devel libgcrypt-devel libselinux-devel libunwind-devel openssl-devel \
      systemd-devel bzip2-devel brotli-devel libffi-devel pcre2-devel \
      libtool-ltdl-devel libxcrypt-devel glib2-static libxcrypt-static pcre-static \
      libselinux-static libsepol-static && \
    if [ "$(uname -m)" = x86_64 ]; then dnf install -y nasm; fi && \
    dnf clean all
# Bump repository-refresh.txt here to intentionally refresh the toolchain RPMs.
COPY . /opt/kasm-gcc-recipes/
ENV BUILD_JOBS=8
RUN /opt/kasm-gcc-recipes/build
