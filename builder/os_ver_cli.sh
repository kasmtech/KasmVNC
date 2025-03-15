#!/bin/bash

default_os=${default_os:-ubuntu}
default_os_codename=${default_os_codename:-noble}

os=${1:-$default_os}
os_codename=${2:-$default_os_codename}
build_tag="$3"
if [[ -n "$build_tag" ]]; then
   build_tag_for_images="_${build_tag#+}"
   build_debian_revision="$(echo $build_tag | tr _ -)"
fi
os_image="$os:$os_codename"

echo "Building for $os_image"
