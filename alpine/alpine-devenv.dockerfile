FROM alpine:3.21

RUN apk add abuild sudo less
ENV HOME /src
WORKDIR /src/kasmvncserver

RUN adduser --disabled-password docker
RUN adduser docker abuild
RUN echo "docker ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

USER docker
