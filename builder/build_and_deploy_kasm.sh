#!/bin/bash

# this script will build kasmvnc and build a new kasm desktop image
# this script assumes you have an instance of kasm already deployed
# it will replace the existing kasm desktop image so the next kasm you launch will use the updated image

set -e

if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root"
   exit 1
fi

docker build -t kasmvncbuilder:latest -f builder/dockerfile.build .
docker run -v /tmp:/build  kasmvncbuilder:latest
cp /tmp/kasmvnc*.tar.gz builder/
cd builder
docker build -t kasmweb/desktop-deluxe:develop -f dockerfile.test .
docker ps -aq --no-trunc -f status=exited | xargs docker rm
docker rmi $(docker images | grep "<none>" | awk "{print $3}")
