#!/bin/bash

detect_distro() {
  if [ -f /etc/centos-release ]; then
    DISTRO=centos
  elif [ -f /etc/oracle-release ]; then
    DISTRO=oracle
  elif [ -f /usr/bin/zypper ]; then
    DISTRO=opensuse
  else
    DISTRO=debian
  fi
}

install_packages() {
  local install_cmd=no-command-defined

  case "$DISTRO" in
    centos) install_cmd="yum install -y" ;;
    oracle) install_cmd="dnf install -y" ;;
    opensuse) install_cmd="zypper install -y" ;;
    *) install_cmd="apt-get update && apt-get install -y"
  esac

  eval "$install_cmd $*"
}

detect_distro
