FROM ubuntu:18.04

ENV DISPLAY=:1 \
    VNC_PORT=8443 \
    VNC_RESOLUTION=1280x720 \
    MAX_FRAME_RATE=24 \
    VNCOPTIONS="-PreferBandwidth -DynamicQualityMin=4 -DynamicQualityMax=7" \
    HOME=/home/user \
    TERM=xterm \
    STARTUPDIR=/dockerstartup \
    INST_SCRIPTS=/dockerstartup/install \
    KASM_RX_HOME=/dockerstartup/kasmrx \
    DEBIAN_FRONTEND=noninteractive \
    VNC_COL_DEPTH=24 \
    VNC_RESOLUTION=1280x1024 \
    VNC_PW=vncpassword \
    VNC_USER=user \
    VNC_VIEW_ONLY_PW=vncviewonlypassword \
    LD_LIBRARY_PATH=/usr/local/lib/ \
    OMP_WAIT_POLICY=PASSIVE \
    SHELL=/bin/bash \
    SINGLE_APPLICATION=1

EXPOSE $VNC_PORT

WORKDIR $HOME

### REQUIRED STUFF ###

RUN apt-get update && apt-get install -y supervisor xfce4 xfce4-terminal xterm libnss-wrapper gettext libjpeg-dev wget
RUN apt-get purge -y pm-utils xscreensaver*

RUN mkdir -p $STARTUPDIR
COPY src/startup/ $STARTUPDIR
RUN mkdir -p $HOME/.config/xfce4/xfconf/xfce-perchannel-xml
COPY src/xfce/ $HOME/.config/xfce4/xfconf/xfce-perchannel-xml
# overwite default with single app config
RUN mv $HOME/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop-single-app.xml $HOME/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml
RUN echo 'source $STARTUPDIR/generate_container_user' >> $HOME/.bashrc

# KasmVNC install
RUN wget -qO- https://github.com/kasmtech/KasmVNC/releases/download/v0.9.1-beta/KasmVNC_0.9.1-beta_Ubuntu_18.04.tar.gz | tar xz --strip 1 -C /

### START CUSTOM STUFF ####

# We need the server to use a fixed resulution and have the client scale, which is not the default behavior of KasmVNC
RUN sed -i "s#UI.initSetting('resize', 'remote');#UI.initSetting('resize', 'scale');#" /usr/local/share/kasmvnc/www/app/ui.js

RUN apt-get install -y chocolate-doom doom-wad-shareware prboom-plus freedoom

# Use software rendering, comment this out if you have a GPU
#RUN mkdir -p $HOME/.local/share/chocolate-doom && \
#	echo 'force_software_renderer    1' > $HOME/.local/share/chocolate-doom/chocolate-doom.cfg


### END CUSTOM STUFF ###

RUN chown -R 1000:0 $HOME
USER 1000
WORKDIR $HOME

ENTRYPOINT [ "/dockerstartup/vnc_startup.sh", "xfce4-terminal", "-e", "/usr/games/chocolate-doom" ]
