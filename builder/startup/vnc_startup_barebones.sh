#!/bin/bash

set -euo pipefail

set_xterm_to_run() {
  mkdir "$config_dir"
  echo '/usr/bin/xterm &' >> "$xstartup"
  chmod +x "$xstartup"
}

create_kasm_user() {
  echo -e "$VNC_PW\n$VNC_PW\n" | kasmvncpasswd -w -u "$VNC_USER"
}

config_dir="$HOME/.vnc"
xstartup="$config_dir/xstartup"

set_xterm_to_run
create_kasm_user

vncserver -select-de manual -websocketPort "$VNC_PORT"
vncserver_exit_code=$?
if [ "$RUN_TEST" = 1 ]; then
  exit "$vncserver_exit_code"
fi

tail -f "$config_dir"/*.log
