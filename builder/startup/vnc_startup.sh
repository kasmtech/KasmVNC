#!/bin/bash
### every exit != 0 fails the script
set -e

# should also source $STARTUPDIR/generate_container_user
source $HOME/.bashrc

## correct forwarding of shutdown signal
cleanup () {
    kill -s SIGTERM $!
    exit 0
}
trap cleanup SIGINT SIGTERM

detect_www_dir() {
  local package_www_dir="/usr/share/kasmvnc/www"
  if [[ -d "$package_www_dir" ]]; then
    package_www_dir_option="-httpd $package_www_dir"
  fi
}

detect_cert_location() {
  local tarball_cert="$HOME/.vnc/self.pem"
  local deb_cert="/etc/ssl/certs/ssl-cert-snakeoil.pem"
  local deb_key="/etc/ssl/private/ssl-cert-snakeoil.key"
  local rpm_cert="/etc/pki/tls/private/kasmvnc.pem"

  if [[ -f "$deb_cert" ]]; then
    cert_option="-cert $deb_cert -key $deb_key"
  elif [[ -f "$rpm_cert" ]]; then
    cert_option="-cert $rpm_cert"
  else
    cert_option="-cert $tarball_cert"
  fi
}

add_vnc_user() {
  local username="$1"
  local password="$2"
  local permission_option="$3"

  echo "Adding user $username"
  echo -e "$password\n$password" | kasmvncpasswd $permission_option \
    -u "$username" $HOME/.kasmpasswd
}

## resolve_vnc_connection
VNC_IP=$(hostname -i)

# first entry is control, second is view (if only one is valid for both)
mkdir -p "$HOME/.vnc"
PASSWD_PATH="$HOME/.vnc/passwd"
add_vnc_user "$VNC_USER" "$VNC_PW" "-w"
add_vnc_user "$VNC_USER-ro" "$VNC_PW"
add_vnc_user "$VNC_USER-owner" "$VNC_PW" "-o"
add_vnc_user "$VNC_USER-to-delete" "$VNC_PW"

kasmvncpasswd -n -u "$VNC_USER-owner" -w -o $HOME/.kasmpasswd
kasmvncpasswd -d -u "$VNC_USER-to-delete" $HOME/.kasmpasswd

chmod 0600 $HOME/.kasmpasswd
openssl req -x509 -nodes -days 3650 -newkey rsa:2048 -keyout $HOME/.vnc/self.pem -out $HOME/.vnc/self.pem -subj "/C=US/ST=VA/L=None/O=None/OU=DoFu/CN=kasm/emailAddress=none@none.none"

vncserver :1 -interface 0.0.0.0
vncserver -kill :1

if [[ -f $PASSWD_PATH ]]; then
    rm -f $PASSWD_PATH
fi

#echo "$VNC_PW" | vncpasswd -f > $PASSWD_PATH
#echo "$VNC_VIEW_ONLY_PW" | vncpasswd -f >> $PASSWD_PATH
#chmod 600 $PASSWD_PATH

unset VNC_VIEW_ONLY_PW
unset VNC_PW

if [[ $DEBUG == true ]]; then
  echo -e "\n------------------ start VNC server ------------------------"
  echo "remove old vnc locks to be a reattachable container"
fi
vncserver -kill $DISPLAY &> $HOME/.vnc/vnc_startup.log \
    || rm -rfv /tmp/.X*-lock /tmp/.X11-unix &> $HOME/.vnc/vnc_startup.log \
    || echo "no locks present"


detect_www_dir
detect_cert_location
[ -n "$KASMVNC_VERBOSE_LOGGING" ] && verbose_logging_option="-log *:stderr:100"

echo -e "start vncserver with param: VNC_COL_DEPTH=$VNC_COL_DEPTH, VNC_RESOLUTION=$VNC_RESOLUTION\n..."
vncserver $DISPLAY -depth $VNC_COL_DEPTH -geometry $VNC_RESOLUTION -FrameRate=$MAX_FRAME_RATE -websocketPort $VNC_PORT $cert_option -sslOnly -interface 0.0.0.0 $VNCOPTIONS $package_www_dir_option $verbose_logging_option #&> $STARTUPDIR/no_vnc_startup.log

PID_SUN=$!

echo -e "start window manager\n..."
$STARTUPDIR/window_manager_startup.sh #&> $STARTUPDIR/window_manager_startup.log

## log connect options
echo -e "\n\n------------------ VNC environment started ------------------"
echo -e "\nVNCSERVER started on DISPLAY= $DISPLAY \n\t=> connect via VNC viewer with $VNC_IP:$VNC_PORT"
echo -e "\nnoVNC HTML client started:\n\t=> connect via http://$VNC_IP:$NO_VNC_PORT/?password=...\n"
echo "WEB PID: $PID_SUB"

# tail vncserver logs
tail -f $HOME/.vnc/*$DISPLAY.log &

eval "$@"

wait $PID_SUB

echo "Exiting Kasm container"
