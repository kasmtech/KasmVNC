### desktop.resolution.width, desktop.resolution.height
Set a fixed desktop resolution. Set `desktop.allow_resize` to `false` for
this to work.

Unit: pixels
<br>
Default: 1024x768

### desktop.allow_resize
If turned on, resizing browser window changes desktop resolution to fit the
window.

Set to `false` to have a fixed resolution (you need to set
[desktop.resolution.width and
desktop.resolution.heigh](#desktop.resolution.width%2C-desktop.resolution.height)).

Unit: boolean
<br>
Default: `true`

### desktop.pixel_depth

Pixel depth in bits. Possible values are 16, 24, 32. These values are fail-safe.
You can specify any bit value via command-line.

Unit: bits
<br>
Default: `24`

### network.protocol
With KasmVNC, you can use a web browser client (set value to `http`) or a
traditional VNC client (set value to `vnc`).

If you use `vnc` protocol, you're on your own. Passing `Xvnc` options via
command-line is required in this case. Most config settings are ignored.

Unit: `http`, `vnc`
<br>
Default: `http`

### network.interface
IP address or host name to listen on. Set to `0.0.0.0` to listen on all network
interfaces.

Unit: ip address or host name
<br>
Default: `0.0.0.0`

### network.websocket_port

Listen for websocket connections on this port. `auto` translates to 8443 + X
display number.

Unit: integer
<br>
Default: `auto`

### network.use_ipv4

Use IPv4 for incoming and outgoing connections.

Unit: boolean
<br>
Default: `true`

### network.use_ipv6
Use IPv6 for incoming and outgoing connections.

Unit: boolean
<br>
Default: `true`

### network.udp.public_ip

KasmVNC server needs to know its IP from the perspective of the client. With `auto`,
it finds that out automatically (by using a public STUN server).

If the server and client are on the same private network, and not connected
to internet, you need to specify a private IP address of the server, accessible
to the client.

Unit: IP address, `auto`.
<br>
Default: `auto`

### network.udp.port

Set UDP port to use. With `auto`,
value is inherited from [network.websocket_port](#network.websocket_port).

Unit: `auto`, integer
<br>
Default: `auto`

### network.ssl.pem_certificate

SSL pem certificate to use for websocket connections.

Unit: path
<br>
Default: standard snake oil certificate on Debian-based distros. Auto-generated
on Fedora-based distros.

### network.ssl.pem_key

SSL pem key to use for websocket connections. If you have private and public
keys in one file, using
[network.ssl.pem_certificate](#network.ssl.pem_certificate) is enough to set
both.

Unit: path
<br>
Default: standard snake oil certificate on Debian-based distros. Auto-generated
on Fedora-based distros.

### network.ssl.require_ssl

Require SSL for websocket connections.

Unit: boolean
<br>
Default: `true`

### user_session.session_type

* `shared` - allow multiple sessions for a single desktop.
* `exclusive` - only one user session can work with the desktop.

If set, overrides client settings.

When combined with
[concurrent_connections_prompt](#user_session.concurrent_connections_prompt) ,
the user is asked to let the incoming connection in.

Unit: `shared`, `exclusive`
<br>
Default: not set, client settings are used

## user_session.new_session_disconnects_existing_exclusive_session

Only applies if [session_type](#user_session.session_type) is set to
`exclusive`. The user working with the desktop is kicked out by a new session.
New session takes over the desktop.

When combined with
[concurrent_connections_prompt](#user_session.concurrent_connections_prompt),
the user is asked to confirm session takeover.

Unit: boolean
<br>
Default: `false`

### user_session.concurrent_connections_prompt

Prompts the user of the desktop to explicitly accept or reject incoming
connections.

The vncconfig(1) program must be running on the desktop.

Unit: boolean
<br>
Default: `false`

### user_session.concurrent_connections_prompt_timeout

Active only if
[concurrent_connections_prompt](#user_session.concurrent_connections_prompt),
is turned on.

Number of seconds to show the Accept Connection dialog before rejecting the
connection.

Unit: seconds
<br>
Default: `10`

### user_session.idle_timeout
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The number of seconds after which an idle session is dropped.

Unit: seconds
<br>
Default: `never`

### keyboard.remap_keys

Set up 1-to-1 character replacement. For example, to exchange the " and @
symbols you would specify the following: `0x22->0x40`. Similar to tr(1).

Unit: hex numbers in the format <from_character>-><to_character>
<br>
Default: not set

### keyboard.ignore_numlock
-- TODO

Key affected by NumLock often require a fake Shift to be inserted in order for
the correct symbol to be generated. Turning on this setting avoids these extra
fake Shift events but may result in a slightly different symbol (for example, a
Return instead of a keypad Enter).

Unit: boolean
<br>
Default: `false`

### keyboard.raw_keyboard

Send keyboard events straight through and avoid mapping them to the current
keyboard layout. This effectively makes the keyboard behave according to the
layout configured on the server instead of the layout configured on the client.

Unit: boolean
<br>
Default: `false`

### pointer.enabled

Allows clicks from mice, trackpads, etc.

Unit: boolean
<br>
Default: `true`

### runtime_configuration.allow_client_to_override_kasm_server_settings

KasmVNC exposes a few settings to the client the standard VNC does not. You can
let the client override these settings or forbid overriding.

Unit: boolean
<br>
Default: `true`

### runtime_configuration.allow_override_standard_vnc_server_settings

If turned on, VNC settings defined in
[runtime_configuration.allow_override_list](#runtime_configuration.allow_override_list)
can be changed at runtime.

Unit: boolean
<br>
Default: `true`

### runtime_configuration.allow_override_list

You can modify listed settings at runtime. Settings can be modified, for
example, using vncconfig(1) program from inside a running session.

The list must contain absolute config keys like
[pointer.enable](#pointer.enable). To actually change
[pointer.enable](#pointer.enable), you need to pass the corresponding
command-line option `AcceptPointerEvents` to vncconfig(1).

Unit: list of absolute config keys
<br>
Default:
```
  pointer.enabled
  data_loss_prevention.clipboard.server_to_client.enabled
  data_loss_prevention.clipboard.client_to_server.enabled
  data_loss_prevention.clipboard.server_to_client.primary_clipboard_enabled
```

### logging.log_writer_name
Log all subsystems of KasmVNC by setting to `all`. To log a specific subsystem,
consult source code for `LogWriter` instances. For example, `static LogWriter
vlog("STrayIcon");`. `STrayIcon` is the log writer name here.

Unit: `all`, \<log writer name found in source code\>
<br>
Default: `all`

### logging.log_dest

Log to the instance's log file in `~/.vnc`, or syslog.

Unit: `logfile`, `syslog`
<br>
Default: `logfile`

### logging.level

Logging verbosity level. Can be in the 0.\.100 range. 100 meaning most verbose
output, 0 meaning most concise.

Unit: integer in the 0.\.100 range
<br>
Default: `30`

### security.brute_force_protection.blacklist_threshold

The number of unauthenticated connection attempts allowed from any individual
host before that host is black-listed.

Unit: integer
<br>
Default: `5`

### security.brute_force_protection.blacklist_timeout

The initial timeout applied when a host is first black-listed by failing to
authenticate
[blacklist_threshold](#security.brute_force_protection.blacklist_threshold)
times. The host cannot re-attempt a connection until the timeout expires.

Unit: seconds
<br>
Default: `10`

### data_loss_prevention.visible_region.top, data_loss_prevention.visible_region.left, data_loss_prevention.visible_region.right, data_loss_prevention.visible_region.bottom

The regions feature allows you to select a region of the screen to render to the
user. Concealed portions of the screen are blacked out.

#### Absolute coordinates

Select a region using pixels:
```
top: 10
left: 10
right: 40
bottom: 40
```

#### Offset coordinates

Use negative numbers to offset from the screen boundary. For `left` and `top`,
this means 0 plus the provided number. In the below example that would be 10.
For `right` and `bottom`, that means the maximum horizontal or vertical
resolution minus the provided number.

If the resolution was 1080x720 in the below example that would equate to a
`right` of 1070 and `bottom` of 710. Therefore, the example below would be
translated to 10, 10, 1070, 710. Using offset coordinates has an advantage of
scaling with screen size changes versus using absolute values.

Select a region using pixel offsets:
```
top: -10
left: -10
right: -10
bottom: -10
```

You can combine absolute values with offset values, such as the following
example:

```
top: 50
left: 10
right: -10
bottom: -10
```

#### Percentages

Regions does support percent values, which are evaluated as a border that is a
percent of the total width and height respectively. Regions does not support
mixing percent values and absolute or offset values.

For example:

```
top: 10%
left: 10%
right: 20%
bottom: 20%
```

Unit: pixels, offset pixels, percentages
<br>
Default: not set

### data_loss_prevention.visible_region.concealed_region.allow_click_down
Allow mouse button down events within the concealed regions, by default they are
blocked.

Unit: boolean
<br>
Default: `false`

### data_loss_prevention.visible_region.concealed_region.allow_click_release

Allow mouse button releases within the concealed regions, by default they are
blocked until the cursor returns to the visible region.

Unit: boolean
<br>
Default: `false`

### data_loss_prevention.clipboard.delay_between_operations

This many milliseconds must pass between clipboard actions.

Unit: milliseconds, `none`
<br>
Default: `none`

### data_loss_prevention.clipboard.allow_mimetypes

Allowed binary clipboard mimetypes.

Unit: mimetype
<br>
Default:
```
chromium/x-web-custom-data
text/html
image/png`
```

### data_loss_prevention.clipboard.server_to_client.enabled

⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

Whether to send desktop clipboard changes to clients.

Unit: boolean
<br>
Default: `false`

### data_loss_prevention.clipboard.server_to_client.size

Limit clipboard bytes to send to clients in one transaction.

Unit: number of bytes, `unlimited`
<br>
Default: `unlimited`

### data_loss_prevention.clipboard.server_to_client.primary_clipboard_enabled

Send the primary selection to the client. Meaning, mouse-selected text is copied
to clipboard. Only works in Chromium-based browsers.

Unit: boolean
<br>
Default: `false`

### data_loss_prevention.clipboard.client_to_server.enabled
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

Accept clipboard updates from clients.

Unit: boolean
<br>
Default: `false`

### data_loss_prevention.clipboard.client_to_server.size

Limit clipboard bytes to receive from clients in one transaction.

Unit: number of bytes, `unlimited`
<br>
Default: `unlimited`

### data_loss_prevention.keyboard.enabled

Accept key press and release events from clients.

Unit: boolean
<br>
Default: `true`

### data_loss_prevention.keyboard.rate_limit

Reject keyboard presses over this many per second.

Unit: integer, `unlimited`
<br>
Default: `unlimited`

### data_loss_prevention.logging.level

Log clipboard and keyboard actions. `info` logs just clipboard direction and
size, `verbose` adds the contents for both.

Unit: `off`, `info`, `verbose`
<br>
Default: `off`

### encoding.max_frame_rate
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The maximum number of updates per second sent to clients. If the screen
updates any faster then those changes are aggregated and sent in a single
update to the client. Note that this only controls the maximum rate and a client
may get a lower rate when resources are limited.

Unit: integer
<br>
Default: `60`

### encoding.full_frame_updates

KasmVNC cuts the screen up into smaller rectangles and only sends sections of
the screen that change. When using UDP, some rectangles can be dropped, so this
option forces a full screen update every X frames.

Unit: `none`, positive integer
<br>
Default: `none`

### encoding.rect_encoding_mode.min_quality
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The minimum quality setting for JPEG/WEBP encoding. Rendering automatically
degrades JPEG quality when there is a lot of motion in a particular block. This
setting controls the minimum quality to use when there is a high degree of
change. The accepted values are 0.\.9 where 0 is low and 9 is high.

Unit: integer in the 0.\.9 range
<br>
Default: `7`

### encoding.rect_encoding_mode.max_quality
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The maximum quality setting for JPEG/WEBP encoding. Rendering automatically
degrades JPEG quality when there is a lot of motion in a particular block. This
setting controls the maximum quality to use when there is no or little motion.
The accepted values are 0.\.9 where 0 is low and 9 is high.

Unit: integer in the 0.\.9 range
<br>
Default: `8`

### encoding.rect_encoding_mode.consider_lossless_quality
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

KasmVNC dynamically adjusts the JPEG/WEBP quality based on how much change
there is in a particular section. Every x number of frames it sends a
'lossless' update. That way, if you are scrolling, the text blurs a little while
you scroll but as soon as you stop, it should clear up on the next lossless
update. `consider_lossless_quality` means "treat this quality as lossless."
Assuming the min quality of 3, the max of 7 and treat lossless of 5. KasmVNC
would constantly adjust the quality of images sent anywhere from 3 to 7
depending on the rate of change. If the last rectangle sent was at 5 then it
would not send a lossless update for that part of the screen.

Unit: integer in the 0.\.10 range
<br>
Default: `10`

### encoding.rect_encoding_mode.rectangle_compress_threads

Use this many threads to compress rectangles in parallel. `auto` sets threads to
match the core count.

Unit: integer, `auto`.
<br>
Default: `auto`

### encoding.video_encoding_mode.jpeg_quality
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The JPEG quality to use when in video mode. The accepted values are 0.\.9 where
0 is low and 9 is high. A value of -1 keeps the quality level used in normal
mode.

Unit: integer in the -1..9 range
<br>
Default: `-1`

### encoding.video_encoding_mode.webp_quality
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The WEBP quality to use when in video mode. The accepted values are 0.\.9 where 0
is low and 9 is high. A value of -1 keeps the quality level used in normal mode.

Unit: integer in the -1..9 range
<br>
Default: `-1`

### encoding.video_encoding_mode.max_resolution.width, encoding.video_encoding_mode.max_resolution.height
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

When in Video Mode, downscale the screen to this maximum size. Keeps aspect
ratio with client's actual resolution.

Unit: pixels
<br>
Default: 1920x1080

### encoding.video_encoding_mode.enter_video_encoding_mode.time_threshold
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

Number of seconds that a high rate of change most occur before switching to
video mode. Setting to 0 forces Video Mode at all times.

Unit: seconds
<br>
Default: `5`

### encoding.video_encoding_mode.enter_video_encoding_mode.area_threshold
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The percent of the screen that must be seeing high rates of change to meet the
threshold of Video Mode. This percentage of the screen must see rapid changes
for the amount of time specified by [encoding.video_encoding_mode.enter_video_encoding_mode.time_threshold](#encoding.video_encoding_mode.enter_video_encoding_mode.time_threshold).

Unit: percentage of screen
<br>
Default: `45%`

### encoding.video_encoding_mode.exit_video_encoding_mode.time_threshold
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

When in Video Mode, high rates of change must subside for this many seconds
before dropping out of video mode.

Unit: seconds
<br>
Default: `3`

### encoding.video_encoding_mode.logging.level

Print the detected video area % value. This is useful when trying to tune your
settings for your particular use case.

Unit: `info`, `off`
<br>
Default: `off`

### encoding.video_encoding_mode.scaling_algorithm
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

The scaling method to use in video mode.

Unit: `nearest`, `bilinear`, `progressive_bilinear`
<br>
Default: `progressive_bilinear`

### encoding.compare_framebuffer

Perform pixel comparison on frame buffer to reduce unnecessary updates.

Unit: `off`, `always`, `auto`
<br>
Default: `auto`

### encoding.zrle_zlib_level
⚠️ This setting only applies, when
[allow_client_to_override_kasm_server_settings](#runtime_configuration.allow_client_to_override_kasm_server_settings)
is turned off.

Zlib compression level for ZRLE encoding (it does not affect Tight encoding).
Acceptable values are between 0.\.9. Set to `auto` to use the standard
compression level provided by the zlib(3) compression library.

Unit: integer in the 0.\.9 range, `auto`
<br>
Default: `auto`

### encoding.hextile_improved_compression

Use improved compression algorithm for Hextile encoding which achieves better
compression ratios by the cost of using slightly more CPU time.

Unit: boolean
<br>
Default: `true`

### server.advanced.x_font_path

Specify X font path.

Unit: path, `auto`
<br>
Default: `auto`

### server.advanced.httpd_directory

Run a mini-HTTP server which serves files from the given directory. Normally the
directory contains the kasmweb client.

Unit: path
<br>
Default: `/usr/share/kasmvnc/www`

### server.advanced.kasm_password_file

Password file for Basic authentication, created with the `kasmvncpasswd(1)`
utility.

Unit: path
<br>
Default: `${HOME}/.kasmpasswd`

### server.advanced.x_authority_file

Set to X authority file. X authority file stores credentials in cookies used by
`xauth(1)` for authentication of X sessions.

`auto` means using file defined in `$XAUTHORITY` environment variable. If the
variable isn't defined, fallback to using `${HOME}/.Xauthority` file.

Unit: path
<br>
Default: `auto`

### server.auto_shutdown.no_user_session_timeout

Terminate KasmVNC when no client has been connected for this many
seconds.

Unit: seconds, `never`
<br>
Default: `never`

### server.auto_shutdown.active_user_session_timeout

Terminate KasmVNC when a client has been connected for this many
seconds.

Unit: seconds, `never`
<br>
Default: `never`

### server.auto_shutdown.inactive_user_session_timeout

Terminate KasmVNC after this many seconds of user inactivity.

Unit: seconds, `never`
<br>
Default: `never`

### command_line.prompt

Guide the user (by prompting), to ensure that KasmVNC is usable. For example,
prompt to create a KasmVNC user if no users exist or no users can control the
desktop.

Unit: boolean
<br>
Default: `true`
