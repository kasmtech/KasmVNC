# KasmVNC - Linux Web Remote Desktop

[![Kasm Technologies](https://kasm-static-content.s3.amazonaws.com/368_kasm_logo_.jpg "Kasm Logo")](https://kasmweb.com)

[Kasm Technologies LLC](https://www.kasmweb.com) developed Kasm Server, a Containerized Desktop Infrastructure (CDI) solution. Kasm started with TigerVNC and eventually forked it to create KasmVNC. KasmVNC is used within the overal Kasm CDI infrastructure, however, you can use KasmVNC for individual servers. KasmVNC has different goals than TigerVNC:

  - Web-based - KasmVNC is designed to provide a web accessible remote desktop. It comes with a web server and websocket server built in. There is no need to install other components. Simply run and navigate to your desktop's URL on the port you specify. While you can still tun on the legacy VNC port, it is disabled by default.
  - Security - KasmVNC defaults to HTTPS and allows for HTTP Basic Auth. VNC Password authentication is limited by specification to 8 characters and is not suffecient for use on an internet accessible remote desktop. Our goal is to create a by default secure, web based experience.
  - Simplicity - KasmVNC aims at being simple to deploy and configure.

# New Features!

  - Webp image compression for better bandwidth usage
  - Automatic mixing of webp and jpeg based on CPU availability on server
  - Multi-threaded image encoding for smoother frame rate for servers with more cores
  - [Full screen video detection](https://github.com/kasmtech/KasmVNC/wiki/Video-Rendering-Options#video-mode), goes into configurable video mode for better full screen videoo playback performance.
  - [Dynamic jpeg/webp image coompression](https://github.com/kasmtech/KasmVNC/wiki/Video-Rendering-Options#dynamic-image-quality) quality settings based on screen change rates
  - Seemless clipboard support (on Chromium based browsers)
  - Allow client to set/change most configuration settings
  - [Data Loss Prevention features](https://github.com/kasmtech/KasmVNC/wiki/Data-Loss-Prevention)
    - Key stroke logging
    - Clipboard logging
    - Max clipboard transfer size up and down
    - Min time between clipboard operations required
    - Keyboard input rate limit
    - Screen region selection
  - Deb packages for Debian, Ubuntu, and Kali Linux included in release.
  - RPM packages for CentOS, Fedora. RPM packages are currently not updatable and not released, though you can build and install them. See build documentation.
  - Web [API](https://github.com/kasmtech/KasmVNC/wiki/API) added for remotely controlling and getting information from KasmVNC
  - Multi-User with ability to pass control to other users.
  - Web UI uses a webpack for faster load times.
  - Network and CPU bottleneck statistics


Future Goals:

  - Support uploads and downloads
  - Pre-build Packages for all major Linux distributions

### Installation

#### Debian-based

```sh
wget -qO- https://github.com/kasmtech/KasmVNC/releases/download/v0.9.1-beta/kasmvncserver_0.9.1~beta-1_amd64.deb

sudo dpkg -i kasmvncserver_0.9.1~beta-1_amd64.deb
sudo apt-get -f install

# We provide an example script to run KasmVNC at #
# /usr/share/doc/kasmvncserver/examples/kasmvncserver-easy-start. It runs a VNC
# server on display :10 and on interface 0.0.0.0. If you're happy with those
# defaults you can just use it as is:
sudo ln -s /usr/share/doc/kasmvncserver/examples/kasmvncserver-easy-start /usr/bin/

# Add your user to the ssl-cert group
sudo addgroup $USER ssl-cert
# You will need to re-connect in order to pick up the group change

# Create ~/.vnc directory and corresponding files.
kasmvncserver-easy-start -d && kasmvncserver-easy-start -kill

# Modify vncstartup to launch your environment of choice, in this example LXDE
# This may be optional depending on your system configuration
echo '/usr/bin/lxsession -s LXDE &' >> ~/.vnc/xstartup

# Start KasmVNC with debug logging:
kasmvncserver-easy-start -d

# Tail the logs
tail -f ~/.vnc/`hostname`:10.log
```

Now navigate to your system at https://[ip-address]:8443/

To stop a running KasmVNC:

```sh
kasmvncserver-easy-start -kill
```

The options for vncserver:

| Argument | Description |
| -------- | ----------- |
| depth | Color depth, for jpeg/webp should be 24bit |
| geometry | Screensize, this will automatically be adjusted when the client connects. |
| websocketPort | The port to use for the web socket. Use a high port to avoid having to run as root. |
| cert | SSL cert to use for HTTPS |
| sslOnly | Disable HTTP |
| interface | Which interface to bind the web server to. |

### Development
Would you like to contribute to KasmVNC? Please reachout to us at info@kasmweb.com

We need help, especially in packaging KasmVNC for various operating systems. We would love to have standard debian or RMP packages and host our own repo, however, that all requires a lot of experience, proper testing, and pipeline development for automated builds.

We also need help with Windows, which is not currently supported. While KasmVNC can technically be built for Windows 10, it is unusably slow, due to all the changes that occured in Windows since the original Windows support was added in the chain of VNC forked projects.

### Compiling From Source
See the [builder/README.md](https://github.com/kasmtech/KasmVNC/blob/master/builder/README.md). We containerize our build systems to ensure highly repeatable builds.

### License and Acknowledgements
See the [LICENSE.TXT](https://github.com/kasmtech/KasmVNC/blob/master/LICENSE.TXT) and [ACKNOWLEDGEMENTS.MD](https://github.com/kasmtech/KasmVNC/blob/master/LICENSE.TXT)
