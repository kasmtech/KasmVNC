#!/bin/bash

detect_distro() {
  if [ -f /etc/oracle-release ]; then
    DISTRO=oracle
  elif [ -f /etc/fedora-release ]; then
    DISTRO=fedora
  elif [ -f /usr/bin/zypper ]; then
    DISTRO=opensuse
  elif [ -f /etc/alpine-release ]; then
    DISTRO=alpine
  else
    DISTRO=debian
  fi
}

install_packages() {
  local install_cmd=no-command-defined

  case "$DISTRO" in
    oracle) install_cmd="dnf install -y" ;;
    fedora) install_cmd="dnf install -y" ;;
    opensuse) install_cmd="zypper install -y" ;;
    alpine) install_cmd="apk add" ;;
    *) install_cmd="apt-get update && apt-get install -y"
  esac

  eval "$install_cmd $*"
}

detect_distro
