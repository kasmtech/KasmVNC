ARG BUILDER_IMAGE
FROM ${BUILDER_IMAGE} AS ffmpeg-headers
ARG KASMVNC_FFMPEG_MODE=static
# Header preparation is independent of the expensive static dependency cache.
COPY builder/ffmpeg-adapters/install-headers builder/ffmpeg-adapters/sources.json /opt/kasm-ffmpeg-adapter-recipes/
RUN /opt/kasm-ffmpeg-adapter-recipes/install-headers "$KASMVNC_FFMPEG_MODE"
FROM ffmpeg-headers
COPY . /src/
ENV KASMVNC_PREBUILT_DEPENDENCIES=1
RUN python3 /src/builder/portable/test-security-assessments.py && /src/builder/portable/test-security-backports && /src/builder/portable/install-scanner && python3 /src/builder/portable/test-supply-chain.py && python3 /src/builder/portable/test-command-logging.py && python3 /src/builder/portable/test-progress.py && python3 /src/builder/portable/test-dependency-cache.py && python3 /src/builder/portable/test-dependency-phases.py
ENTRYPOINT ["/src/builder/portable/build-inside-docker"]
