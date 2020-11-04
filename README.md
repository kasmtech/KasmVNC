# KasmVNC - Linux Web Remote Desktop

[![Kasm Technologies](https://kasm-static-content.s3.amazonaws.com/368_kasm_logo_.jpg "Kasm Logo")](https://kasmweb.com)

[Kasm Technologies LLC](https://www.kasmweb.com) developed Kasm Server, a Containerized Desktop Infrastructure (CDI) solution. Kasm started with TigerVNC and eventually forked it to create KasmVNC. KasmVNC is used within the overal Kasm CDI infrastructure, however, you can use KasmVNC for individual servers. KasmVNC has different goals than TigerVNC:

  - Web-based - KasmVNC is designed to provide a web accessible remote desktop. It comes with a web server and websocket server built in. There is no need to install other components. Simply run and navigate to your desktop's URL on the port you specify. While you can still tun on the legacy VNC port, it is disabled by default.
  - Security - KasmVNC defaults to HTTPS and allows for HTTP Basic Auth. VNC Password authentication is limited by specification to 8 characters and is not suffecient for use on an internet accessible remote desktop. Our goal is to create a by default secure, web based experience.
  - Simplicity - KasmVNC aims at being simple to deploy and configure.

# New Features!

  - Webp image compression for better bandwidth usage
  - Automatic mixing of webp and jpeg based on CPU availability on server
  - Multi-threaded image encoding for smoother frame rate for servers with more cores
  - Full screen video detection, goes into configurable video mode for better full screen videoo playback performance.
  - Dynamic jpeg/webp image coompression quality settings based on screen change rates
  - Seemless clipboard support
  - Allow client to set/change most configuration settings
  - Data Loss Prevention features
    - Key stroke logging
    - Clipboard logging
    - Max clipboard transfer size up and down
    - Min time between clipboard operations required
    - Keyboard input rate limit



Future Goals:

  - Support uploads and downloads
  - Pre-build Packages for all major Linux distributions
  - CI pipelines to create releases

### Installation
We are currently developing releasable packages for major operating sytems. The install script available with releases will install dependencies, compile webp, and pull down and install the pre-compiled KasmVNC tarball. Currently, only Ubuntu 18.04 LTS is pre-compiled.

This installer assumes you already have a desktop environment installed, but have never configured a VNC server. Use the install script found in this project under builder/install/install.sh, currently Ubuntu 18.04LTS is the only operating system with pre-compiled binaries.

```sh
# install dependencies
sudo apt-get -y install libjpeg-dev

# install KasmVNC
wget -qO- https://github.com/kasmtech/KasmVNC/releases/download/v0.9.1-beta/KasmVNC_0.9.1-beta_Ubuntu_18.04.tar.gz | sudo tar xz --strip 1 -C /

# Generate an SSL Cert and change owner
sudo mkdir /usr/local/share/kasmvnc/certs
sudo openssl req -x509 -nodes -days 3650 -newkey rsa:2048 -keyout /usr/local/share/kasmvnc/certs/self.pem -out /usr/local/share/kasmvnc/certs/self.pem -subj "/C=US/ST=VA/L=None/O=None/OU=DoFu/CN=kasm/emailAddress=none@none.none"
sudo chown $USER /usr/local/share/kasmvnc/certs/self.pem

# start kasmvnc and set password for remote access
vncserver :1 -interface 0.0.0.0
# stop kasmvnc to make config changes
vncserver -kill :1

# modify vncstartup to launch your environment of choice, in this example LXDE
echo '/usr/bin/lxsession -s LXDE &' >> ~/.vnc/xstartup

# The KasmVNC username is automatically set to your system username, you can mofify it if you wish
vi ~/.vnc/config

# launch KasmVNC
vncserver $DISPLAY -depth 24 -geometry 1280x1050 -websocketPort 8443 -cert /usr/local/share/kasmvnc/certs/self.pem -sslOnly -FrameRate=24 -interface 0.0.0.0
```

Now navigate to your system at https://[ip-address]:8443/vnc.html

The options for vncserver in the example above:

| Argument | Description |
| -------- | ----------- |
| depth | Color depth, for jpeg/webp should be 24bit |
| geometry | Screensize, this will automatically be adjusted when the client connects. |
| websocketPort | The port to use for the web socket. Use a high port to avoid having to run as root. |
| cert | SSL cert to use for HTTPS |
| sslOnly | Disable HTTP |
| interface | Which interface to bind the web server to. |

### Development
Would you like to contribute to KasmVNC? Please reachout to us at info@kasmweb.com

We need help, especially in packaging KasmVNC for various operating systems. We would love to have standard debian or RMP packages and host our own repo, however, that all requires a lot of experience, proper testing, and pipeline development for automated builds.

We also need help with Windows, which is not currently supported. While KasmVNC can technically be built for Windows 10, it is unusably slow, due to all the changes that occured in Windows since the original Windows support was added in the chain of VNC forked projects.

### Compiling From Source
See the builder/README.md. We containerize our build systems to ensure highly repeatable builds. 

License & Legal
----
Incomplete and generally out of date copyright list::

        Copyright (C) 2020 Kasm Technologies LLC
        Copyright (C) 1999 AT&T Laboratories Cambridge
        Copyright (C) 2002-2005 RealVNC Ltd.
        Copyright (C) 2000-2006 TightVNC Group
        Copyright (C) 2005-2006 Martin Koegler
        Copyright (C) 2005-2006 Sun Microsystems, Inc.
        Copyright (C) 2006 OCCAM Financial Technology
        Copyright (C) 2000-2008 Constantin Kaplinsky
        Copyright (C) 2004-2017 Peter Astrand for Cendio AB
        Copyright (C) 2010 Antoine Martin
        Copyright (C) 2010 m-privacy GmbH
        Copyright (C) 2009-2011 D. R. Commander
        Copyright (C) 2009-2011 Pierre Ossman for Cendio AB
        Copyright (C) 2004, 2009-2011 Red Hat, Inc.
        Copyright (C) 2009-2018 TigerVNC Team
        All Rights Reserved.

This software is distributed under the GNU General Public Licence as published
by the Free Software Foundation.  See the file LICENCE.TXT for the conditions
under which this software is made available.  KasmVNC also contains code from
other sources.  See the Acknowledgements section below, and the individual
source files, for details of the conditions under which they are made
available.

### Acknoledgements
This distribution contains zlib compression software.  This is:

  Copyright (C) 1995-2002 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu

  The data format used by the zlib library is described by RFCs (Request for
  Comments) 1950 to 1952 in the files ftp://ds.internic.net/rfc/rfc1950.txt
  (zlib format), rfc1951.txt (deflate format) and rfc1952.txt (gzip format).


This distribution contains public domain DES software by Richard Outerbridge.
This is:

Copyright (c) 1988,1989,1990,1991,1992 by Richard Outerbridge.
(GEnie : OUTER; CIS : [71755,204]) Graven Imagery, 1992.


This distribution contains software from the X Window System.  This is:

 Copyright 1987, 1988, 1998  The Open Group
 
 Permission to use, copy, modify, distribute, and sell this software and its
 documentation for any purpose is hereby granted without fee, provided that
 the above copyright notice appear in all copies and that both that
 copyright notice and this permission notice appear in supporting
 documentation.
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 
 Except as contained in this notice, the name of The Open Group shall not be
 used in advertising or otherwise to promote the sale, use or other dealings
 in this Software without prior written authorization from The Open Group.
 
 
 Copyright 1987, 1988 by Digital Equipment Corporation, Maynard, Massachusetts.
 All Rights Reserved
 
 Permission to use, copy, modify, and distribute this software and its 
 documentation for any purpose and without fee is hereby granted, 
 provided that the above copyright notice appear in all copies and that
 both that copyright notice and this permission notice appear in 
 supporting documentation, and that the name of Digital not be
 used in advertising or publicity pertaining to distribution of the
 software without specific, written prior permission.  
 
 DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
 ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
 DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
 ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 SOFTWARE.
