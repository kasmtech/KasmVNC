set -e

OS_ID='unknown'
OS_VERSION_ID='unknown'
SUPPORTED='false'

if [[ $EUID -ne 0 ]]; then
   echo "This script must ran with sudo"
   exit 1
fi

function install_deps_ubuntu_18(){
	# install deps and build tools
	sudo apt-get update
	sudo apt-get -y install libjpeg-dev libpng-dev libtiff-dev libgif-dev build-essential cmake libxfont-dev

	wget http://launchpadlibrarian.net/347526424/libxfont1-dev_1.5.2-4ubuntu2_amd64.deb
	wget http://launchpadlibrarian.net/347526425/libxfont1_1.5.2-4ubuntu2_amd64.deb
	sudo dpkg -i libxfont1*.deb
	rm /tmp/libxfont1*.deb
}

function build_webp(){
	# build webp
	wget https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.0.2.tar.gz
	tar -xzvf /tmp/libwebp-*
	cd /tmp/libwebp-1.0.2
	./configure
	make
	sudo make install
	cd /
	rm -rf /tmp/libwebp*
	sudo ldconfig
}

function install_kasmvnc(){
	# install kasmvnc
	wget -qO- https://kasmweb-build-artifacts.s3.amazonaws.com/kasmvnc/c0ab0111ae47a39720f26a7dd7ac54a3681540f8/kasmvnc_1.9.c0ab0111ae47a39720f26a7dd7ac54a3681540f8.tar.gz | sudo tar xz --strip 1 -C /
	#install cert
	sudo mkdir /usr/local/share/kasmvnc/certs
	sudo openssl req -x509 -nodes -days 3650 -newkey rsa:2048 -keyout /usr/local/share/kasmvnc/certs/self.pem -out /usr/local/share/kasmvnc/certs/self.pem -subj "/C=US/ST=VA/L=None/O=None/OU=DoFu/CN=kasm/emailAddress=none@none.none"
}

cd /tmp

# Get the OS and version
if [ -f /etc/os-release ] ; then
    OS_ID="$(awk -F= '/^ID=/{print $2}' /etc/os-release)"
    OS_VERSION_ID="$(awk -F= '/^VERSION_ID/{print $2}' /etc/os-release)"
fi


if [ "${OS_ID}" == "ubuntu" ] && ( [ "${OS_VERSION_ID}" == '"16.04"' ] || [ "${OS_VERSION_ID}" == '"18.04"' ] || [ "${OS_VERSION_ID}" == '"20.04"' ]) ; then
   
   if [ "${OS_VERSION_ID}" == '"18.04"' ] ; then
   	 SUPPORTED='true'
   	 install_deps_ubuntu_18
   	 build_webp
   	 install_kasmvnc
   fi
fi

if [ "${OS_ID}" == "debian" ] && ( [ "${OS_VERSION_ID}" == '"9"' ] || [ "${OS_VERSION_ID}" == '"10"' ] ) ; then
   #TODO: Add support for debian 
   echo 'Debian is currently not supported'
fi

if [ "${OS_ID}" == '"centos"' ] && ( [ "${OS_VERSION_ID}" == '"7"' ] || [ "${OS_VERSION_ID}" == '"8"' ] ) ; then
   #TODO: Add support for Centos
   echo 'CentOS is currently not supported'
fi

if [ "${SUPPORTED}" == "false" ] ; then
   echo "Installation Not Supported for this Operating System. You must compile KasmVNC from source."
   exit -1
fi

echo "Installation is complete"
echo "Follow the instructions to complete setup"
