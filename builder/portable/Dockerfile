ARG BUILDER_IMAGE
FROM ${BUILDER_IMAGE} AS openssl-sdk
ARG BUILD_JOBS=8
ENV KASMVNC_OPENSSL_PREFIX=/opt/kasm-openssl3
COPY builder/openssl3/ /opt/kasm-openssl-recipes/
RUN BUILD_JOBS=${BUILD_JOBS} python3 /opt/kasm-openssl-recipes/prepare-sdk
FROM openssl-sdk AS ffmpeg-headers
ARG KASMVNC_FFMPEG_MODE=static
# Header preparation is independent of the expensive static dependency cache.
COPY builder/ffmpeg-adapters/install-headers builder/ffmpeg-adapters/sources.json /opt/kasm-ffmpeg-adapter-recipes/
RUN /opt/kasm-ffmpeg-adapter-recipes/install-headers "$KASMVNC_FFMPEG_MODE"
FROM ffmpeg-headers
# EL9's development package provides libcrypt.so.2. Portable applications use
# the libcrypt.so.1 ABI available across the target distributions. Prepare this
# runtime even when an older dependency cache did not install the compat RPM.
RUN dnf install -y libxcrypt-compat && dnf clean all
# Use exactly the archived revision; local output, Git metadata and private test
# submodules must not leak into the package image or consume the ARM disk.
ADD builder/portable/kasmvnc-source.tar /src/
COPY builder/www/ /src/builder/www/
COPY builder/www-supply-chain/ /src/builder/www-supply-chain/
COPY builder/portable/kasmvnc-source.tar /src/builder/portable/kasmvnc-source.tar
ENV KASMVNC_PREBUILT_DEPENDENCIES=1
RUN python3 /src/builder/portable/test-security-assessments.py && /src/builder/portable/test-security-backports && /src/builder/portable/install-scanner && python3 /src/builder/portable/test-supply-chain.py && python3 /src/builder/portable/test-cmake-profiles.py && python3 /src/builder/portable/test-openssl-link.py && python3 /src/builder/portable/test-openssl-boundary.py && python3 /src/builder/portable/test-host-libraries.py && python3 /src/builder/portable/test-command-logging.py && python3 /src/builder/portable/test-progress.py && python3 /src/builder/portable/test-dependency-cache.py && python3 /src/builder/portable/test-dependency-phases.py
RUN CPPFLAGS="-I/opt/kasm-openssl3/include" LDFLAGS="-L/opt/kasm-openssl3/lib" bash /src/builder/openssl-tests/build /tmp/kasm-openssl-tests && /tmp/kasm-openssl-tests/crypto-regression
ENTRYPOINT ["/src/builder/portable/build-inside-docker"]
