# KasmVNC - Linux Web Remote Desktop

<a href="https://kasmweb.com"><img src="https://kasm-static-content.s3.amazonaws.com/logo_kasm.png" width="300"><a/>

KasmVNC provides remote web-based access to a Desktop or application. While VNC is in the name, KasmVNC differs from other VNC variants such as TigerVNC, RealVNC, and TurboVNC. KasmVNC has broken from the RFB specification which defines VNC, in order to support modern technologies and increase security. KasmVNC is accessed by users from any modern browser and does not support legacy VNC viewer applications. KasmVNC uses a modern YAML based configuration at the server and user level, allowing for ease of management.

[Kasm Technologies](https://www.kasmweb.com) developed Kasm Workspaces, the Containerized Streaming Platform. Kasm has open-sourced the Workspace docker images, which include containerized [full desktops and apps](https://github.com/kasmtech/workspaces-images) and [base images](https://github.com/kasmtech/workspaces-core-images) intended for developers to create customized streaming containers. These containers can be used standalone or within the [Kasm Workspaces Platform](https://www.kasmweb.com) which provides a full Enterprise feature set.

## Documentation

**Do not use the README from the master branch**, unless you are compiling KasmVNC yourself from the tip of master. Use the documentation for your specific release.

  - [KasmVNC 1.0.0 Documentation](https://www.kasmweb.com/kasmvnc/docs/1.0.0/index.html)

  For beta releases prior to version 1.0.0, use the README in this github project on the tagged commit for that release.

## Installation

**You must disconnect and reconnect to the server after installation, for the group membership to apply.**

### Debian/Ubuntu/Kali
```sh
# Please choose the package for your distro here (under Assets):
# https://github.com/kasmtech/KasmVNC/releases
wget <package_url>

sudo apt-get install ./kasmvncserver_*.deb

# Add your user to the ssl-cert group
sudo addgroup $USER ssl-cert
```

### Oracle 8
```sh
# Please choose the package for your distro here (under Assets):
# https://github.com/kasmtech/KasmVNC/releases
wget <package_url>

# Ensure KasmVNC dependencies are available
sudo dnf config-manager --set-enabled ol8_codeready_builder
sudo dnf install oracle-epel-release-el8

sudo dnf localinstall ./kasmvncserver_*.rpm

# Add your user to the kasmvnc-cert group
sudo usermod -a -G kasmvnc-cert $USER
```

### CentOS 7

```sh
# Please choose the package for your distro here (under Assets):
# https://github.com/kasmtech/KasmVNC/releases
wget <package_url>

# Ensure KasmVNC dependencies are available
sudo yum install epel-release

sudo yum install ./kasmvncserver_*.rpm

# Add your user to the kasmvnc-cert group
sudo usermod -a -G kasmvnc-cert $USER
```

## Getting Started

The following examples provide basic usage of KasmVNC with the tools provided. For full documentation on all the utilities and the runtime environment, see our [KasmVNC Documentation](https://www.kasmweb.com/kasmvnc/docs/latest/index.html)

```sh
# Start a session and be guided to setup a user and select a default desktop environment
vncserver

# Start a session with the mate desktop environment
vncserver -select-de mate

# Add a new user with read/write permissions
vncpasswd -u my_username -w -r

# Tail the logs
tail -f ~/.vnc/*.log

# Get a list of current sessions with display IDs
vncserver -list

# Kill the VNC session with display ID :2
vncserver -kill :2
```

## Configuration

KasmVNC is configured via YAML based configurations. The server level configuration is at `/etc/kasmvnc/kasmvnc.yaml`. Edits to this file apply to all users. Individual users can override server global configurations by specifying them in their configuration file at `~/.vnc/kasmvnc.yaml`.

The following configuration shows all default settings. Many of the encoding settings can be overridden by the client, unless the `runtime_configuration.allow_client_to_override_kasm_server_settings` setting is set tot false. By default the client is allowed to modify encoding settings.

For a full description of each setting see the [configuration reference](https://www.kasmweb.com/kasmvnc/docs/latest/configuration.html).

```yaml
desktop:
  resolution:
    width: 1024
    height: 768
  allow_resize: true
  pixel_depth: 24
  gpu:
    hw3d: false
    drinode: /dev/dri/renderD128

network:
  protocol: http
  interface: 0.0.0.0
  websocket_port: auto
  use_ipv4: true
  use_ipv6: true
  udp:
    public_ip: auto
    port: auto
    stun_server: auto
  ssl:
    pem_certificate: /etc/ssl/certs/ssl-cert-snakeoil.pem
    pem_key: /etc/ssl/private/ssl-cert-snakeoil.key
    require_ssl: true

user_session:
  new_session_disconnects_existing_exclusive_session: false
  concurrent_connections_prompt: false
  concurrent_connections_prompt_timeout: 10
  idle_timeout: never

keyboard:
  remap_keys:
  ignore_numlock: false
  raw_keyboard: false

pointer:
  enabled: true

runtime_configuration:
  allow_client_to_override_kasm_server_settings: true
  allow_override_standard_vnc_server_settings: true
  allow_override_list:
    - pointer.enabled
    - data_loss_prevention.clipboard.server_to_client.enabled
    - data_loss_prevention.clipboard.client_to_server.enabled
    - data_loss_prevention.clipboard.server_to_client.primary_clipboard_enabled

logging:
  log_writer_name: all
  log_dest: logfile
  level: 30

security:
  brute_force_protection:
    blacklist_threshold: 5
    blacklist_timeout: 10

data_loss_prevention:
  visible_region:
    # top: 10
    # left: 10
    # right: 40
    # bottom: 40
    concealed_region:
      allow_click_down: false
      allow_click_release: false
  clipboard:
    delay_between_operations: none
    allow_mimetypes:
      - chromium/x-web-custom-data
      - text/html
      - image/png
    server_to_client:
      enabled: true
      size: unlimited
      primary_clipboard_enabled: false
    client_to_server:
      enabled: true
      size: unlimited
  keyboard:
    enabled: true
    rate_limit: unlimited
  logging:
    level: off

encoding:
  max_frame_rate: 60
  full_frame_updates: none
  rect_encoding_mode:
    min_quality: 7
    max_quality: 8
    consider_lossless_quality: 10
    rectangle_compress_threads: auto

  video_encoding_mode:
    jpeg_quality: -1
    webp_quality: -1
    max_resolution:
      width: 1920
      height: 1080
    enter_video_encoding_mode:
      time_threshold: 5
      area_threshold: 45%
    exit_video_encoding_mode:
      time_threshold: 3
    logging:
      level: off
    scaling_algorithm: progressive_bilinear

  compare_framebuffer: auto
  zrle_zlib_level: auto
  hextile_improved_compression: true

server:
  http:
    headers:
      - Cross-Origin-Embedder-Policy=require-corp
      - Cross-Origin-Opener-Policy=same-origin
    httpd_directory: /usr/share/kasmvnc/www
  advanced:
    x_font_path: auto
    kasm_password_file: ${HOME}/.kasmpasswd
    x_authority_file: auto
  auto_shutdown:
    no_user_session_timeout: never
    active_user_session_timeout: never
    inactive_user_session_timeout: never

command_line:
  prompt: true
```


# New Features!

  - Faster jpeg compression (via statically linked libjpeg-turbo)
  - Webp image compression for better bandwidth usage
  - Automatic mixing of webp and jpeg based on CPU availability on server
  - Multi-threaded image encoding for smoother frame rate for servers with more cores
  - WebRTC UDP Transit
  - Lossless QOI Image format for Local LAN
  - [Full screen video detection](https://github.com/kasmtech/KasmVNC/wiki/Video-Rendering-Options#video-mode), goes into configurable video mode for better full screen video playback performance.
  - [Dynamic jpeg/webp image compression](https://github.com/kasmtech/KasmVNC/wiki/Video-Rendering-Options#dynamic-image-quality) quality settings based on screen change rates
  - Seemless clipboard support (on Chromium based browsers)
  - Binary clipboard support for text, images, and formatted text (on Chromium based browsers)
  - Allow client to set/change most configuration settings
  - [Data Loss Prevention features](https://github.com/kasmtech/KasmVNC/wiki/Data-Loss-Prevention)
    - Key stroke logging
    - Clipboard logging
    - Max clipboard transfer size up and down
    - Min time between clipboard operations required
    - Keyboard input rate limit
    - Screen region selection
  - Deb packages for Debian, Ubuntu, and Kali Linux included in release.
  - RPM packages for CentOS, Oracle, OpenSUSE, Fedora. RPM packages are currently not updatable and not released, though you can build and install them. See build documentation.
  - Web [API](https://github.com/kasmtech/KasmVNC/wiki/API) added for remotely controlling and getting information from KasmVNC
  - Multi-User support with permissions that can be changed via the API
  - Web UI uses a webpack for faster load times.
  - Network and CPU bottleneck statistics
  - Relative cursor support (game pointer mode)
  - Cursor lock
  - IME support for languages with extended characters
  - Better mobile support
  - DRI3 GPU acceleration with open source drivers (AMDGPU,Intel,ATI,ARM)

Future Goals:

  - H264 encoding

### Compiling From Source
See the [builder/README.md](https://github.com/kasmtech/KasmVNC/blob/master/builder/README.md). We containerize our build systems to ensure highly repeatable builds.

### License and Acknowledgements
See the [LICENSE.TXT](https://github.com/kasmtech/KasmVNC/blob/master/LICENSE.TXT) and [ACKNOWLEDGEMENTS.MD](https://github.com/kasmtech/KasmVNC/blob/master/LICENSE.TXT)
