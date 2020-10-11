# Containerized Examples

These example containers show how to do UI app streaming within Docker containers using KasmVNC.

## Doom

What better way to demonstrait containerized app streaming then to stream Doom! The included Dockerfile.ubuntu18.doom file and the source code have everything you need to hit the ground running.

### Building
```sh
sudo docker build -t kasm/doom -f Dockerfile.ubuntu18.doom .
```

### Running
```sh
sudo docker run -it -p 8443:8443 --rm -e "VNC_USER=matt" -e "VNC_PW=password123"  kasm/doom:latest
```

The environmental variables VNC_USER and VNC_PW set the username and password respectively. The VNC_PW is unset during container startup. 

Now navigate to https://<ip-address>:8443/vnc.html

![Kasm Technologies](https://kasm-static-content.s3.amazonaws.com/doom-screenshot.jpg "Doom rendered in browser")

### License

See the Chocolate Doom project for details on license specifics of Doom. (https://github.com/chocolate-doom/chocolate-doom)
