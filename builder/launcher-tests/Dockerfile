FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-yaml python3-pip python3-pexpect clang-format g++ libyaml-dev perl libswitch-perl \
    liblist-moreutils-perl libtry-tiny-perl libyaml-tiny-perl \
    libhash-merge-simple-perl libdatetime-perl libdatetime-timezone-perl \
    xauth x11-utils xfonts-base hostname tzdata \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
ENTRYPOINT ["python3", "/src/builder/launcher-tests/parity.py"]
