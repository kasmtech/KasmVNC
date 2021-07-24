#!/bin/bash

debug() {
  if [ -z "$debug" ]; then return; fi

  echo "$@"
}

enable_debug() {
  debug=1
  log_option="-log *:stderr:100"
}

kill_vnc_server() {
  vncserver -kill $display
}

process_cli_options() {
  for option in "$@"; do
    case "$option" in
      --help)
        show_help
        exit
        ;;
      -d|--debug)
        enable_debug
        ;;
      -k|--kill)
        kill_vnc_server
        exit
        ;;
      -s|--select-de)
        action=select-de-and-start
        ;;
      *)
        echo >&2 "Unsupported argument: $option"
        exit 1
    esac
  done
}

show_help() {
  cat >&2 <<-USAGE
Usage: $(basename "$0") [options]
  -d, --debug      Debug output
  -k, --kill       Kill vncserver
  -s, --select-de  Select desktop environent to run
  --help           Show this help
USAGE
}
