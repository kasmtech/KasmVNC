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

wait_for_core_to_be_dumped() {
  if [ "$vncserver_exit_code" -eq 0 ]; then
    return
  fi

  local timeout=2
  local elapsed=0
  local interval=1
  while [[ ! -f core && "$elapsed" -lt "$timeout" ]]; do
    sleep $interval
    elapsed=$(($elapsed + $interval))
  done
}

copy_core_to_host() {
  mkdir -p "$CORE_DUMP_DIR_INSIDE_CONTAINER"
  cp core "$CORE_DUMP_DIR_INSIDE_CONTAINER"
}

allow_core_to_be_dumped() {
  ulimit -c unlimited
  cd "$HOME"
}

clean_up_old_core_dir() {
  if [ -d "$CORE_DUMP_DIR_INSIDE_CONTAINER" ]; then
    rm -r "$CORE_DUMP_DIR_INSIDE_CONTAINER"
  fi
}

core_was_dumped() {
  [ -f core ]
}

say_where_to_find_core_on_host() {
  echo "Core dumped to $CORE_DUMP_DIR_ON_HOST"
}

config_dir="$HOME/.vnc"
xstartup="$config_dir/xstartup"

set_xterm_to_run
create_kasm_user

allow_core_to_be_dumped
clean_up_old_core_dir
set +e
vncserver -select-de manual -websocketPort "$VNC_PORT"
vncserver_exit_code=$?
set -e

wait_for_core_to_be_dumped
if core_was_dumped; then
  copy_core_to_host
  say_where_to_find_core_on_host
fi
if [ "$RUN_TEST" = 1 ]; then
  exit "$vncserver_exit_code"
fi

tail -f "$config_dir"/*.log
