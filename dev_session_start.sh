#!/usr/bin/env bash

# unset SESSION_MANAGER
# unset DBUS_SESSION_BUS_ADDRESS

export GTK_MODULES=gail:atk-bridge
export QT_ACCESSIBILITY=1
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
export SAL_ACCESSIBILITY_ENABLED=1
export GNOME_ACCESSIBILITY=1

while ( true ); do
  /usr/bin/xfce4-session --display :0
done;
