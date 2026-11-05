# Shared by the glibc and musl dependency entry points. Source after libc setup.
case $(uname -m) in
 x86_64) baseline='-march=x86-64 -mtune=generic' ;;
 aarch64) baseline='-march=armv8-a -mtune=generic' ;;
 *) exit 1 ;;
esac
export CFLAGS="-O2 -fPIC $baseline -g" CXXFLAGS="-O2 -fPIC $baseline -g"
export KASMVNC_STATIC_RUNTIME_PREFIX=/opt/kasm-runtime
export KASMVNC_CODEC_PREFIX=/opt/kasm-codecs
export KASMVNC_STATIC_DESKTOP_PREFIX=/opt/kasm-desktop
export KASMVNC_DYNAMIC_DESKTOP=ON
export LIBYUV_COMMIT=eb6e7bb63738e29efd82ea3cf2a115238a89fa51
KASMVNC_PORTABLE_DRIVER_PATH=$(/opt/kasm-dependency-recipes/portable-driver-path)
export KASMVNC_PORTABLE_DRIVER_PATH
cd /tmp
recipes=/opt/kasm-dependency-recipes
