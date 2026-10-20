ARG BUILDER_IMAGE
FROM ${BUILDER_IMAGE}
RUN dnf install -y dpkg rpm-build gdb && dnf clean all
COPY builder/portable /src/builder/portable
ENTRYPOINT ["/src/builder/portable/build-inside-docker"]
