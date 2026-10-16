# Pin the bootstrap image as well as nixpkgs; supports native amd64/arm64 runners.
FROM nixos/nix@sha256:7a007c766426c1877758ddc5cb87a965ac131fc78c582ce0083d922d51ae945c
COPY . /src
WORKDIR /src
ARG KASMVNC_NIX_VERSION
ARG KASMVNC_COMMIT_ID
ARG KASMVNC_NIX_BASENAME
ARG NIX_BUILD_CORES=4
RUN builder/build-nix-inside-docker
