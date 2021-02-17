default_os=ubuntu
default_os_codename=bionic

os=${1:-$default_os}
os_codename=${2:-$default_os_codename}
os_image="$os:$os_codename"

echo "Building for $os_image"
