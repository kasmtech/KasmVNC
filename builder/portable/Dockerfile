ARG BUILDER_IMAGE
FROM ${BUILDER_IMAGE}
COPY . /src/
ENV KASMVNC_PREBUILT_DEPENDENCIES=1
RUN /src/builder/portable/test-security-backports && /src/builder/portable/install-scanner && python3 /src/builder/portable/test-supply-chain.py && python3 /src/builder/portable/test-command-logging.py && python3 /src/builder/portable/test-progress.py && python3 /src/builder/portable/test-dependency-cache.py && python3 /src/builder/portable/test-dependency-phases.py
ENTRYPOINT ["/src/builder/portable/build-inside-docker"]
