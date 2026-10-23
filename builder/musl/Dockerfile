ARG BUILDER_IMAGE
FROM ${BUILDER_IMAGE}
COPY . /src/
ENV KASMVNC_PREBUILT_DEPENDENCIES=1 KASMVNC_LIBC=musl
RUN python3 /src/builder/portable/test-security-assessments.py && /src/builder/portable/test-security-backports && /src/builder/portable/install-scanner && python3 /src/builder/portable/test-command-logging.py && python3 /src/builder/portable/test-progress.py && python3 /src/builder/portable/test-dependency-cache.py && python3 /src/builder/portable/test-dependency-phases.py && python3 /src/builder/musl/test-packaging.py
ENTRYPOINT ["/src/builder/musl/build-inside-docker"]
