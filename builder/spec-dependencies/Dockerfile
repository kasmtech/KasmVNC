FROM ubuntu:jammy
ARG DEPENDENCY_CONTEXT_SHA
LABEL org.kasmweb.spec-dependency-key=$DEPENDENCY_CONTEXT_SHA
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y vim less python3-pip strace silversearcher-ag xfonts-base cinnamon mate wget && rm -rf /var/lib/apt/lists/*
RUN useradd -m docker && mkdir /opt/spec-env && chown docker:docker /opt/spec-env
ENV USER=docker HOME=/home/docker
ENV PIPENV_PIPFILE=/opt/spec-env/Pipfile PIPENV_VENV_IN_PROJECT=1
ENV PATH=/home/docker/.local/bin:$PATH
COPY --chown=docker:docker Pipfile Pipfile.lock /opt/spec-env/
USER docker
RUN pip3 install --user pipenv==2026.10.1 && pipenv sync
ENV LC_ALL=C.UTF-8 LANG=C.UTF-8
