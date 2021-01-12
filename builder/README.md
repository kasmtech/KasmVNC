## REQIUREMENTS
Docker CE

# Build the www webpack
```
sudo docker build -t kasmweb/www -f builder/dockerfile.www.build .
sudo docker run -it --rm -v $PWD/builder/www:/build kasmweb/www:latest
```

# build the docker image
```
    cd /src_code_root
    sudo docker build -t kasmvncbuilder:18.04 -f builder/dockerfile.ubuntu1804.build .
```

### Run the builder
```sh
    mkdir -p builder/build
    sudo docker run -v /tmp:/build --rm  kasmvncbuilder:18.04
    cp /tmp/build/kasmvnc.ubuntu_18.04.tar.gz builder/build/
```

### Build test desktop container
```sh
    cd builder
    sudo docker build -t kasmvnctester:18.04 -f dockerfile.ubuntu1804.test .
```

### run an instance of the new destkop
```sh
sudo docker run -it -p 443:8443 --rm -e "VNC_USER=username" -e "VNC_PW=password123"  kasmvnctester:18.04
```

open browser and point to https://<ip-address>/vnc.html
The username and password were set in the docker run command
