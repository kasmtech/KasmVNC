## REQIUREMENTS
Docker CE

# Build the www webpack
```
docker build -t kasmweb/www -f builder/dockerfile.www.build .
docker run -it --rm -v $PWD/builder/www:/build kasmweb/www:latest
```

# Build a deb/rpm package
```
# builder/build-package <os> <os_codename>
# os_codename is what "lsb_release -c" outputs, e.g. buster, focal.
# Packages will be placed under builder/build/

builder/build-package ubuntu bionic
builder/build-package ubuntu focal
builder/build-package debian buster
builder/build-package debian bullseye
builder/build-package kali kali-rolling
builder/build-package centos core # CentOS 7
builder/build-package fedora thirtythree
```

# build the docker image
```
    cd /src_code_root
    docker build -t kasmvncbuilder:18.04 -f builder/dockerfile.ubuntu1804.build .
```

### Run the builder
```sh
    mkdir -p builder/build
    docker run -v /tmp:/build --rm  kasmvncbuilder:18.04
    cp /tmp/build/kasmvnc.ubuntu_18.04.tar.gz builder/build/
```

### Build test desktop container
```sh
    cd builder
    docker build -t kasmvnctester:18.04 -f dockerfile.ubuntu1804.test .
```

### run an instance of the new destkop
```sh
docker run -it -p 443:8443 --rm -e "VNC_USER=username" -e "VNC_PW=password123"  kasmvnctester:18.04
```

open browser and point to https://<ip-address>/vnc.html
The username and password were set in the docker run command
