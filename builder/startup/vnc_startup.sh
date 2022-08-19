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


[ -n "$KASMVNC_VERBOSE_LOGGING" ] && verbose_logging_option="-debug"

echo -e "start vncserver with param: VNC_COL_DEPTH=$VNC_COL_DEPTH, VNC_RESOLUTION=$VNC_RESOLUTION\n..."
vncserver $DISPLAY -select-de xfce -depth $VNC_COL_DEPTH -geometry $VNC_RESOLUTION -FrameRate=$MAX_FRAME_RATE -websocketPort $VNC_PORT $VNCOPTIONS $verbose_logging_option #&> $STARTUPDIR/no_vnc_startup.log

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
