#!/bin/bash

usage() {
  echo >&2 "Usage: $(basename "$0") [-s|--shell] [-p|--perf-test] [-h|--help] <distro> <distro_version>"
  exit
}

process_options() {
   local sorted_options=$(getopt -o prsh --long perf-test --long run-test --long shell --long help -- "$@")
   eval set -- $sorted_options

   while : ; do
      case "$1" in
         -p|--perf-test)
            entrypoint_args='-interface 0.0.0.0 -selfBench :1'
            entrypoint_executable="--entrypoint=/usr/bin/Xvnc"
            shift
            ;;
         -r|--run-test)
            run_test=1
            shift
            ;;
         -s|--shell)
            entrypoint_executable="--entrypoint=bash"
            shift
            ;;
         -h|--help)
            print_usage=1
            ;;
         --)
            shift
            break
            ;;
      esac
   done

   leftover_options=("$@")
}

declare -a leftover_options

if [ "$#" -eq 0 ]; then
   usage
   exit
fi

process_options "$@"
set -- "${leftover_options[@]}"

if [ -n "$print_usage" ]; then
   usage
   exit
fi
