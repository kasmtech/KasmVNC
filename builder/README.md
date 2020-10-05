REQIUREMENTS
Docker CE

# build the docker image
    cd /src_code_root
    sudo docker build -t kasmvncbuilder:18.04 -f builder/dockerfile.build .

# run the builder
    sudo docker run -v /tmp:/build --rm  kasmvncbuilder:18.04

# tar will be on /tmp of host
    cp /tmp/kasmvnc*.tar.gz builder/

# build test desktop container with new binary installed
    cd builder
    sudo docker build -t kasmvnctester:18.04 -f dockerfile.test .
    
# run an instance of the new destkop
    sudo docker run -d -p 80:6901 -p 5901:5901 -e VNCOPTIONS="-detectScrolling -PreferBandwidth -DynamicQualityMin=0" kasmvnctester:latest

open browser and point to http://IPAddress/vnc_lite.html
default password is "vncpassword" or use a VNC client
