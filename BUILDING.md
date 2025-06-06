# Building KasmVNC

## Building the KasmVNC Server using Docker

```bash
git submodule init
git submodule update --remote --merge
sudo docker build -t kasmvnc:dev -f builder/dockerfile.ubuntu_jammy.dev .
sudo docker run -it --rm -v ./:/src -p 6901:6901 -p 8443:8443 --name kasmvnc_dev kasmvnc:dev
```

**The above assumes you are UID 1000 on the host as the container UID is 1000.**
Ensure you switch to the user associated to UID 1000 on the host.

Now from inside the container.

```bash
# build frontend
cd kasmweb
npm install
npm run build # <-- only run this on subsequent changes to front-end code
cd ..
# build dependencies, this is optional as they are pre-built in the docker image. Only rebuild if you made version changes and need to test.
# sudo builder/scripts/build-webp
# sudo builder/scripts/build-libjpeg-turbo
# Build KasmVNC
builder/build.sh
```

Now run Xvnc and Xfce4 from inside the container

```bash
/src/xorg.build/bin/Xvnc -interface 0.0.0.0 -PublicIP 127.0.0.1 -disableBasicAuth -RectThreads 0 -Log *:stdout:100 -httpd /src/kasmweb/dist -sslOnly 0 -SecurityTypes None -websocketPort 6901 -FreeKeyMappings :1 &
/usr/bin/xfce4-session --display :1
```

Now open a browser and navigate to your dev VM on port 6901.

## Running noVNC from source
If you need to debug or make changes to the UI code, use the following procedures to use npm to serve the web code. The code will automatically rebuild when changes are made and the code will not be packaged.
These steps assume you are inside the kasmvnc:dev container started in the above steps.

Now from inside the container. **This assumes KasmVNC is already built, follow steps above if you need to build KasmVNC**

```bash
# Run KasmVNC
/src/xorg.build/bin/Xvnc -interface 0.0.0.0 -PublicIP 127.0.0.1 -disableBasicAuth -RectThreads 0 -Log *:stdout:100 -httpd /src/kasmweb/dist -sslOnly 0 -SecurityTypes None -websocketPort 6901 -FreeKeyMappings :1 &
/usr/bin/xfce4-session --display :1 &

sudo nginx
cd kasmweb
npm install # only needs done first time
npm run serve # <-- Needs to run in foreground
```

Now open a browser and navigate to your dev VM on port 8443 over https. 

NGINX is proxying the websocket to KasmVNC and all other requests go to the node server. NGINX listens on 8443 with ssl.

Since `npm run serve` needs to run in the foreground, you may need to exec into the container from another terminal to run additional commands like stopping Xvnc, rebuilding KasmVNC, etc.

```bash
sudo docker exec -it kasmvnc_dev /bin/bash
```

## Building in CI

### Achieve a faster feedback loop in CI

To achieve a faster feedback loop in CI, you can limit built distros to a few
distros you're interested in.

#### Best way to debug CI in a fast feedback loop

Specify `BUILD_DISTROS_REGEX: "jammy"`, and build only Ubuntu Jammy. That's
only 2 distro package build jobs, one for amd64 and one for arm64.

Specify `ALLOW_RUN_TESTS_TO_FAIL: true` to ignore `run_test` job failures and
debug the upload stage.

#### Build only a few distros

To build only distros you want, specify a regex in `$BUILD_DISTROS_REGEX`
variable.  For example, build Ubuntu Jammy and Focal with `BUILD_DISTROS_REGEX:
"jammy|focal"`. Or simply a single distro Ubuntu Jammy with
`BUILD_DISTROS_REGEX: "jammy"`.<br>
To build all distros, specify `BUILD_DISTROS_REGEX: all`.<br>
To build no distros, and no www, specify `BUILD_DISTROS_REGEX: none`.

##### Required distros to build

Functional tests and spec tests use Ubuntu Jammy. If Jammy is not built, those
stages fail. If you want to debug pipeline stages after `functional_test`,
building Ubuntu Jammy is required.

Specify `BUILD_DISTROS_REGEX: "jammy|<distro_you're_interested_in>"`.

##### Heed, when writing regex

Regex placed in `$BUILD_DISTROS_REGEX` are [Bash
regex](https://tldp.org/LDP/Bash-Beginners-Guide/html/sect_04_01.html) and are
processed by Bash.

Any whitespace in `$BUILD_DISTROS_REGEX` must be escaped. For example,
`BUILD_DISTROS_REGEX: "\ jammy"`.

#### Debug upload stage, while building only a few distros

`run_test` jobs fail the pipeline, whenever packages for a distro weren't
built. When building only a few distros, packages for the rest of distros aren't
built. Not finding the package to test, a `run_test` job fails, pipeline fails,
and the upload stage doesn't get executed.

To still execute and debug upload stage in a faster feedback loop (building only
a few distros), specify `ALLOW_RUN_TESTS_TO_FAIL: true`.  Gitlab CI's
`allow_failure: true` is used to allow the pipeline to ignore failed `run_test`
jobs and continue to the upload stage.

## Building the KasmVNC Server on Modern Unix/Linux Systems

Building the KasmVNC Server (Xvnc) is a bit trickier.  On newer systems
containing Xorg 7.4 or later (such as Fedora), Xvnc is typically built to use
the X11 shared libraries provided with the system.  The procedure for this is
system-specific, since it requires specifying such things as font directories,
but the general outline is as follows.

```bash
  cd {build_directory}

  # If performing an out-of-tree build:
  mkdir unix
  cp -R {source_directory}/unix/xserver unix/

  cp -R {xorg_source}/* unix/xserver/
  # (NOTE: {xorg_source} is the directory containing the Xorg source for the
  # machine on which you are building KasmVNC.  The most recent versions of
  # Red Hat/Fedora, for instance, provide an RPM called
  # "xorg-x11-server-source", which installs the Xorg source under
  # /usr/share/xorg-x11-server-source.)

  cd unix/xserver/
  patch -p1 < {source_directory}/unix/xserver{version}.patch
  (where {version} matches the X server version you are building, such as
  "17" for version 1.7.x.)
  autoreconf -fiv

  ./configure --with-pic --without-dtrace --disable-static --disable-dri \
    --disable-xinerama --disable-xvfb --disable-xnest --disable-xorg \
    --disable-dmx --disable-xwin --disable-xephyr --disable-kdrive \
    --disable-config-dbus --disable-config-hal --disable-config-udev \
    --disable-dri2 --enable-install-libxf86config --enable-glx \
    --with-default-font-path="catalogue:/etc/X11/fontpath.d,built-ins" \
    --with-fontdir=/usr/share/X11/fonts \
    --with-xkb-path=/usr/share/X11/xkb \
    --with-xkb-output=/var/lib/xkb \
    --with-xkb-bin-directory=/usr/bin \
    --with-serverconfig-path=/usr/lib[64]/xorg \
    --with-dri-driver-path=/usr/lib[64]/dri \
    {additional configure options}
    # (NOTE: This is merely an example that works with Red Hat Enterprise
    # and recent Fedora releases.  You should customize it for your particular
    # system.  In particular, it will be necessary to customize the font, XKB,
    # and DRI directories.)

  make KASMVNC_SRCDIR={source_directory}
```


## Building the KasmVNC Server on Legacy Unix/Linux Systems

Those using systems with older versions of Xorg must build a "legacy-friendly"
version of the KasmVNC Server.  This is accomplished by downloading and
building the more recent Xorg modules in a local directory and then building
Xvnc such that it links against the local build of these libraries, not the X11
libraries installed on the system.  The "build-xorg" script in the KasmVNC
source distribution (located under contrib/xorg/) automates this process.

The following procedure will build a
"legacy-friendly" version of the KasmVNC Server:

```bash
  cd {build_directory}
  sh {source_directory}/contrib/xorg/build-xorg init
  sh {source_directory}/contrib/xorg/build-xorg build [additional CMake flags]
```

build-xorg generates a version of Xvnc that has no external dependencies on the
X11 shared libraries or any other distribution-specific shared libraries.  This
version of Xvnc should be transportable across multiple O/S distributions.
build-xorg should work on Red Hat Enterprise 4, its contemporaries, and later
systems.  It probably will not work on older systems.  It has not been tested
on non-Linux systems (yet).

build-xorg can also be used to rebuild just the KasmVNC Server,
once the X11 modules and other dependencies have been built for the first time.
This is convenient for testing changes that just apply to the KasmVNC source
code.  To accomplish this, run:

```sh
  sh {source_directory}/contrib/xorg/build-xorg rebuild [additional make flags]
```

For instance,

```sh
  sh {source_directory}/contrib/xorg/build-xorg rebuild clean
```

will clean the Xvnc build without destroying any of the
build configuration or module dependencies.


## Debug Build

Add `-DCMAKE_BUILD_TYPE=Debug` to the CMake command line.


## Portable (semi-static) Build

KasmVNC can under favourble circumstances be built in a way that allows
the resulting binaries to run on any system without having to also install
all the dynamic libraries it depends on. Enable this mode by adding:

  `-DBUILD_STATIC=1`

to the CMake command line.

Note that the method used to achieve this is very fragile and it may be
necessary to tweak cmake/StaticBuild.cmake to make things work on your
specific system.

# Build Requirements (Windows)

-- MinGW or MinGW-w64

-- Inno Setup (needed to build the KasmVNC installer)
   Inno Setup can be downloaded from http://www.jrsoftware.org/isinfo.php.
   You also need the Inno Setup Preprocessor, which is available in the
   Inno Setup QuickStart Pack.

   Add the directory containing iscc.exe (for instance,
   C:\Program Files\Inno Setup 5) to the system or user PATH environment
   variable prior to building KasmVNC.


# Out-of-Tree Builds

Binary objects, libraries, and executables are generated in the same directory
from which cmake was executed (the "binary directory"), and this directory need
not necessarily be the same as the KasmVNC source directory.  You can create
multiple independent binary directories, in which different versions of
KasmVNC can be built from the same source tree using different compilers or
settings.  In the sections below, {build_directory} refers to the binary
directory, whereas {source_directory} refers to the KasmVNC source directory.
For in-tree builds, these directories are the same.


# Building TLS Support

TLS requires GnuTLS, which is supplied with most Linux distributions and
with MinGW for Windows and can be built from source on OS X and other
Unix variants. However, GnuTLS versions > 2.12.x && < 3.3.x should be
avoided because of potential incompatibilities during initial handshaking.

You can override the `GNUTLS_LIBRARY` and `GNUTLS_INCLUDE_DIR` CMake variables
to specify the locations of libgnutls and any dependencies.  For instance,
adding

```bash
  -DGNUTLS_INCLUDE_DIR=/usr/local/include \
  -DGNUTLS_LIBRARY=/usr/local/lib/libgnutls.a
```

to the CMake command line would link KasmVNC against a static version of
libgnutls located under /usr/local.


# Building Native Language Support (NLS)

NLS requires gettext, which is supplied with most Linux distributions and
with MinGW for Windows and which can easily be built from source on OS X and
other Unix variants.

You can override the ICONV_LIBRARIES and LIBINTL_LIBRARY CMake variables to
specify the locations of libiconv and libintl, respectively.  For instance,
adding

  `-DLIBINTL_LIBRARY=/opt/gettext/lib/libintl.a`

to the CMake command line would link KasmVNC against a static version of
libintl located under /opt/gettext.  Adding

```bash
  -DICONV_INCLUDE_DIR=/mingw/include \
    -DICONV_LIBRARIES=/mingw/lib/libiconv.a \
    -DGETTEXT_INCLUDE_DIR=/mingw/include \
    -DLIBINTL_LIBRARY=/mingw/lib/libintl.a
```

to the CMake command line would link KasmVNC against the static versions of
libiconv and libintl included in the MinGW Developer Toolkit.


# Installing KasmVNC

You can use the build system to install KasmVNC into a directory of your
choosing.  To do this, add:

  `-DCMAKE_INSTALL_PREFIX={install_directory}`

to the CMake command line.  Then, you can run 'make install' to build and
install it.

If you don't specify `CMAKE_INSTALL_PREFIX`, then the default is
`c:\Program Files\KasmVNC` on Windows and `/usr/local` on Unix.


# Creating Release Packages

The following commands can be used to create various types of release packages:


## Unix

`make tarball`

  Create a binary tarball containing the utils

`make servertarball`

  Create a binary tarball containing both the KasmVNC Server and utils

`make dmg`

  Create Macintosh disk image file that contains an application bundle of the
  utils

`make udmg`

  On 64-bit OS X systems, this creates a version of the Macintosh package and
  disk image which contains universal i386/x86-64 binaries.  You should first
  configure a 32-bit out-of-tree build of KasmVNC, then configure a 64-bit
  out-of-tree build, then run 'make udmg' from the 64-bit build directory.  The
  build system will look for the 32-bit build under {source_directory}/osxx86
  by default, but you can override this by setting the OSX_X86_BUILD CMake
  variable to the directory containing your configured 32-bit build.  Either
  the 64-bit or 32-bit build can be configured to be backward compatible by
  using the instructions in the "Build Recipes" section.


## Windows

`make installer`

  Create a Windows installer using Inno Setup.  The installer package
  (KasmVNC[64].exe) will be located under {build_directory}.


# Build Recipes


## 32-bit Build on 64-bit Linux/Unix (including OS X)

Set the following environment variables before building KasmVNC.

```bash
  CFLAGS='-O3 -m32'
  CXXFLAGS='-O3 -m32'
  LDFLAGS=-m32
```

If you are building the KasmVNC Server on a modern Unix/Linux system, then
you will also need to pass the appropriate --host argument when configuring the
X server source (for instance, --host=i686-pc-linux-gnu).


## 64-bit Backward-Compatible Build on 64-bit OS X

Add

```bash
  -DCMAKE_OSX_SYSROOT=/Developer/SDKs/MacOSX10.5.sdk \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.5
```

to the CMake command line.  The OS X 10.5 SDK must be installed.


## 32-bit Backward-Compatible Build on 64-bit OS X

Set the following environment variables:

```bash
  CC=gcc-4.0
  CXX=g++-4.0
  CFLAGS='-O3 -m32'
  CXXFLAGS='-O3 -m32'
  LDFLAGS=-m32
```

and add

```bash
  -DCMAKE_OSX_SYSROOT=/Developer/SDKs/MacOSX10.4u.sdk \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.4
```

to the CMake command line.  The OS X 10.4 SDK must be installed.


## 64-bit MinGW Build on Cygwin

```bash
  cd {build_directory}
  CC=/usr/bin/x86_64-w64-mingw32-gcc CXX=/usr/bin/x86_64-w64-mingw32-g++ \
    RC=/usr/bin/x86_64-w64-mingw32-windres \
    cmake -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_AR=/usr/bin/x86_64-w64-mingw32-ar \
    -DCMAKE_RANLIB=/usr/bin/x86_64-w64-mingw32-ranlib {source_directory}
  make
```

This produces a 64-bit build of KasmVNC that does not depend on cygwin1.dll or
other Cygwin DLL's.  The mingw64-x86_64-gcc-core and mingw64-x86_64-gcc-g++
packages (and their dependencies) must be installed.


## 32-bit MinGW Build on Cygwin

```bash
  cd {build_directory}
  CC=/usr/bin/i686-w64-mingw32-gcc CXX=/usr/bin/i686-w64-mingw32-g++ \
    RC=/usr/bin/i686-w64-mingw32-windres \
    cmake -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=Windows \
    -DDCMAKE_AR=/usr/bin/i686-w64-mingw32-ar \
    -DCMAKE_RANLIB=/usr/bin/i686-w64-mingw32-ranlib {source_directory}
  make
```

This produces a 32-bit build of KasmVNC that does not depend on cygwin1.dll or
other Cygwin DLL's.  The mingw64-i686-gcc-core and mingw64-i686-gcc-g++
packages (and their dependencies) must be installed.


## MinGW-w64 Build on Windows

This produces a 64-bit build of KasmVNC using the "native" MinGW-w64 toolchain
(which is faster than the Cygwin version):

```bash
  cd {build_directory}
  CC={mingw-w64_binary_path}/x86_64-w64-mingw32-gcc \
    CXX={mingw-w64_binary_path}/x86_64-w64-mingw32-g++ \
    RC={mingw-w64_binary_path}/x86_64-w64-mingw32-windres \
    cmake -G "MSYS Makefiles" \
    -DCMAKE_AR={mingw-w64_binary_path}/x86_64-w64-mingw32-ar \
    -DCMAKE_RANLIB={mingw-w64_binary_path}/x86_64-w64-mingw32-ranlib \
    {source_directory}
  make
```


## MinGW Build on Linux

```bash
  cd {build_directory}
  CC={mingw_binary_path}/i386-mingw32-gcc \
    CXX={mingw_binary_path}/i386-mingw32-g++ \
    RC={mingw_binary_path}/i386-mingw32-windres \
    cmake -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_AR={mingw_binary_path}/i386-mingw32-ar \
    -DCMAKE_RANLIB={mingw_binary_path}/i386-mingw32-ranlib \
    {source_directory}
  make
```


# Distribution-Specific Packaging


## RPM Packages for RHEL

The RPM spec files and patches used to create the nightly builds
and releases can be found in the "contrib/rpm/el{5,6}" directories
of the KasmVNC subversion trunk.  All external source tarballs
must be fetched manually and placed into the 'SOURCES' directory
under the rpmbuild root.  Additionally, the following macros need
to be defined:

```
  EL6:
    %debug_package %{nil}

  EL5:
    %dist .el5
    %_smp_mflags  -j3
    %debug_package %{nil}
    %__arch_install_post   /usr/lib/rpm/check-rpaths   /usr/lib/rpm/check-buildroot
```


## Debian packages for Ubuntu 12.04LTS

The debian folder used to create the nightly builds and releases
can be found in the "contrib/deb/ubuntu-precise" directory of the
KasmVNC subversion trunk.
