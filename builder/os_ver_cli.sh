default_os=ubuntu
default_os_ver=18.04

os=${1:-$default_os}
os_ver=${2:-$default_os_ver}
os_ver_dockerfile=`echo $os_ver | sed 's/\.//g'`
os_image="$os:$os_ver"

echo "Building for $os_image"
