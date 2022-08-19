### logging.log_dest

Log to the instance's log file in `~/.vnc`, or syslog. You can find the right
log file by the display number. For example, in `9b1d402e204c:1.log`, `:1` is
the display number. When you start a new instance with `vncserver`, it prints
the display number it uses.

### logging.level

For the initial setup, it's a good idea to use 100. With 100, you're able to see
login attempts and reasons they fail.
