#!/bin/bash

set -euo pipefail

export JPEG_TURBO_RELEASE="${JPEG_TURBO_RELEASE:-3.2.0}"
export LIBCPUID_RELEASE="${LIBCPUID_RELEASE:-v0.8.1}"
export FMT_RELEASE="${FMT_RELEASE:-12.2.0}"
export LIBSTATGRAB_RELEASE="${LIBSTATGRAB_RELEASE:-LIBSTATGRAB_0_92_1}"
if grep -q 'Ubuntu 20.04\|Debian GNU/Linux 11' /etc/os-release 2>/dev/null; then
    export TBB_RELEASE="${TBB_RELEASE:-v2020.3.3}"
else
    export TBB_RELEASE="${TBB_RELEASE:-v2023.0.0}"
fi
export LIBYUV_BRANCH="${LIBYUV_BRANCH:-stable}"

source_dir=$(dirname "$0")
echo ">> Building libjpeg-turbo..."
"${source_dir}"/build-libjpeg-turbo
echo ">> Building WebP..."
"${source_dir}"/build-webp
echo ">> Building Intel TBB..."
"${source_dir}"/build-tbb
echo ">> Building cpuid..."
"${source_dir}"/build-cpuid
echo ">> Building fmt..."
"${source_dir}"/build-fmt
echo ">> Building libyuv..."
"${source_dir}"/build-libyuv
echo ">> Building libstatgrab..."
"${source_dir}"/build-libstatgrab
