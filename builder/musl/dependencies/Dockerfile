# Oldest supported musl ABI; keep both architectures on the same pinned index.
FROM alpine:3.21@sha256:ce64758a109eb420d874a118f87920e625e12d3634e03b4a5573fd9f6e5d3507
ARG DEPENDENCY_CONTEXT_SHA
LABEL org.kasmweb.portable-dependency-key=$DEPENDENCY_CONTEXT_SHA
RUN test -n "$DEPENDENCY_CONTEXT_SHA" && apk add --no-cache \
    alpine-sdk abuild bash coreutils findutils grep sed patch file binutils gdb \
    gcc g++ make cmake meson ninja git curl wget tar xz bzip2 gzip \
    autoconf automake libtool pkgconf python3 py3-pip python3-dev py3-jinja2 \
    bison flex gperf gettext-dev gettext-static perl nasm linux-headers \
    util-macros xorgproto xtrans font-util-dev libx11-dev libxau-dev \
    libxdmcp-dev libxfont2-dev libxkbfile-dev libxrandr-dev libxcvt-dev \
    libxtst-dev libxcursor-dev libsm-dev at-spi2-core-dev dbus-dev \
    libva-dev libdrm-dev libxshmfence-dev mesa-dev mesa-dri-gallium \
    pixman-dev freetype-dev zlib-dev libpng-dev libjpeg-turbo-dev \
    tiff-dev libxml2-dev util-linux-dev libcap-dev lz4-dev zstd-dev \
    libunwind-dev openssl-dev bzip2-dev brotli-dev libffi-dev pcre2-dev \
    glib-dev glib-static libucontext-dev libucontext
COPY recipes/ /opt/kasm-dependency-recipes/
COPY musl/ /opt/kasm-musl-profile/
ENV BUILD_JOBS=8 KASMVNC_LIBC=musl
RUN /opt/kasm-dependency-recipes/install_sccache_from_github && /opt/kasm-musl-profile/build
