# Shared dependency build order and pkg-config/include paths.
python3 "$recipes/run-phase.py" runtime "$recipes/build-static-runtime"
python3 "$recipes/run-phase.py" desktop "$recipes/build-static-desktop"
export PKG_CONFIG_PATH="$KASMVNC_STATIC_RUNTIME_PREFIX/lib/pkgconfig:$KASMVNC_CODEC_PREFIX/lib/pkgconfig:$KASMVNC_STATIC_DESKTOP_PREFIX/lib/pkgconfig"
python3 "$recipes/run-phase.py" codecs "$recipes/build-static-ffmpeg"
export CPATH="$KASMVNC_STATIC_RUNTIME_PREFIX/include:$KASMVNC_CODEC_PREFIX/include"
export PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$HOME/.local/lib64/pkgconfig:$HOME/.local/share/pkgconfig:$PKG_CONFIG_PATH"
export CPATH="$HOME/.local/include:$CPATH"
python3 "$recipes/run-phase.py" helpers "$recipes/build-deps.sh"
