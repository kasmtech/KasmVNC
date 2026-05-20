#!/bin/bash

set -euo pipefail

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
