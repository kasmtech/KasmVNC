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

detect_distro
