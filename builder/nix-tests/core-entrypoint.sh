#!/bin/bash
set -e
# NVIDIA toolkit injects these at container start, after the image build.
# Publish vendor libraries only, leaving the core desktop's native libc alone.
shopt -s nullglob
for directory in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/local/nvidia/lib64; do
  for library in "$directory"/libnvidia-*.so* "$directory"/libcuda.so* "$directory"/libnvcuvid.so* "$directory"/libEGL_nvidia.so* "$directory"/libGLX_nvidia.so*; do
    ln -sf "$library" /run/opengl-driver/lib/
  done
done
exec /dockerstartup/kasm_default_profile.sh /dockerstartup/vnc_startup.sh /dockerstartup/kasm_startup.sh "$@"
