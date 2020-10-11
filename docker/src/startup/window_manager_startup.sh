#!/usr/bin/env bash
set -e

echo -e "\n------------------ Xfce4 window manager startup------------------"

### disable screen saver and power management
xset -dpms &
xset s noblank &
xset s off &

if [ "$SINGLE_APPLICATION" -eq "1" ]; then
    echo "Configured of Single Application Mode"
    sed -i "s/O|SHMC/|/g" $HOME/.config/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml
    #xfwm4 --daemon
fi
