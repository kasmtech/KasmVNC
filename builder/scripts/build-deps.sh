#!/bin/bash

set -e

source_dir=$(dirname "$0")
"${source_dir}"/build-libjpeg-turbo
"${source_dir}"/build-webp
"${source_dir}"/build-tbb