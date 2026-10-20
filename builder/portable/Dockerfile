ARG BUILDER_IMAGE
FROM ${BUILDER_IMAGE}
RUN dnf install -y dpkg rpm-build gdb && dnf clean all
COPY builder/portable /src/builder/portable
RUN /src/builder/portable/install-scanner && python3 /src/builder/portable/test-supply-chain.py
ENTRYPOINT ["/src/builder/portable/build-inside-docker"]
