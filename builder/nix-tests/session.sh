#!/usr/bin/env bash
# Run in an isolated disposable container, with server + test closures at /nix.
set -euo pipefail
: "${PACKAGE:?}" "${TOOLS:?}"
export PATH="$PACKAGE/bin:$TOOLS/bin:$PATH"
export HOME=/tmp/vnc680-home DISPLAY=:1 XDG_RUNTIME_DIR=/tmp/vnc680-runtime
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" /tmp/.X11-unix
chmod 700 "$XDG_RUNTIME_DIR"
# Only vendor GPU libraries enter this directory. Never expose host glibc here.
mkdir -p /run/opengl-driver/lib /run/opengl-driver/share/glvnd/egl_vendor.d
if [ "${GPU:-0}" = 1 ]; then
  shopt -s nullglob
  for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /usr/lib /usr/local/nvidia/lib64; do
    for file in "$dir"/libnvidia-*.so* "$dir"/libcuda.so* "$dir"/libnvcuvid.so* "$dir"/libEGL_nvidia.so* "$dir"/libGLX_nvidia.so* "$dir"/libGLESv2_nvidia.so*; do
      ln -sf "$file" /run/opengl-driver/lib/
    done
  done
  printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_nvidia.so.0"}}\n' > /run/opengl-driver/share/glvnd/egl_vendor.d/10_nvidia.json
else
  mesa=$(cat "$TOOLS/mesa-path")
  for file in "$mesa"/lib/libGLX_mesa.so* "$mesa"/lib/libEGL_mesa.so* "$mesa"/lib/libgallium*.so*; do
    [ ! -e "$file" ] || ln -sf "$file" /run/opengl-driver/lib/
  done
  ln -s "$mesa/lib/dri" /run/opengl-driver/lib/dri
fi
printf 'kasmRulez\nkasmRulez\n' | "$PACKAGE/bin/kasmvncpasswd" -u kasm_user -wo
"$PACKAGE/bin/Xvnc" :1 -geometry 1024x768 -depth 24 \
  -interface 0.0.0.0 -KasmPasswordFile "$HOME/.kasmpasswd" -SecurityTypes None -sslOnly 0 \
  -httpd "$PACKAGE/share/kasmvnc/www" -websocketPort 6901 \
  -PublicIP 127.0.0.1 -videoCodec h264,h264_nvenc -FreeKeyMappings -Log '*:stdout:100' > /tmp/xvnc.log 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
curl --retry-all-errors --retry 20 --retry-delay 1 --retry-connrefused -u kasm_user:kasmRulez -fsS http://127.0.0.1:6901/ -o /tmp/index.html
xdpyinfo > /tmp/xdpyinfo.txt
openbox > /tmp/openbox.log 2>&1 &
xterm -geometry 80x24+10+10 -e bash -c 'while :; do date; sleep 1; done' > /tmp/xterm.log 2>&1 &
if [ "${GPU:-0}" = 1 ]; then
  export LD_LIBRARY_PATH=/run/opengl-driver/lib
  export __EGL_VENDOR_LIBRARY_FILENAMES=/run/opengl-driver/share/glvnd/egl_vendor.d/10_nvidia.json
  vglrun -d egl glxinfo -B > /tmp/renderer.txt 2>&1
  grep -q 'NVIDIA' /tmp/renderer.txt
  vglrun -d egl glxgears > /tmp/gears.log 2>&1 &
  ffmpeg -hide_banner -f lavfi -i testsrc2=size=640x480:rate=30 -t 2 -c:v h264_nvenc -y /tmp/nvenc.mp4 > /tmp/ffmpeg.log 2>&1
else
  glxinfo -B > /tmp/renderer.txt 2>&1
  grep -qi 'llvmpipe' /tmp/renderer.txt
  glxgears > /tmp/gears.log 2>&1 &
  ffmpeg -hide_banner -f lavfi -i testsrc2=size=640x480:rate=30 -t 2 -c:v libx264 -y /tmp/software.mp4 > /tmp/ffmpeg.log 2>&1
fi
printf 'READY\n' > /tmp/session-ready
wait "$server"
