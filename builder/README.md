## REQIUREMENTS
Docker CE

# Build a deb/rpm package
```
# builder/build-package <os> <os_codename> <build_tag>
# os_codename is what "lsb_release -c" outputs, e.g. buster, focal.
#
# build_tag allows building multiple versions of deb package targeting a single
# distro release (e.g. Ubuntu Bionic). If build_tag is given, the package name
# will include build_tag as part of Debian revision. For example:
# * with build_tag: kasmvncserver_0.9.1~beta-1+libjpeg-turbo-latest_amd64.deb
# * without build_tag: kasmvncserver_0.9.1~beta-1_amd64.deb
# You need to have .build and .deb.build for the build_tag.
#
# Packages will be placed under builder/build/

builder/build-package ubuntu bionic
builder/build-package ubuntu focal
builder/build-package debian buster
builder/build-package debian bullseye
builder/build-package kali kali-rolling
builder/build-package centos core # CentOS 7
builder/build-package fedora thirtythree
```

# Build and test a package
```
builder/build-and-test-deb ubuntu focal
builder/build-and-test-rpm centos core
```

Open browser and point to https://localhost:443/ or https://\<ip-address\>:443/

3 default users are created:
* 'foo' with default password 'foobar'. It can use mouse and keyboard.
* 'foo-ro' with default password 'foobar'. It can only view.
* 'foo-owner' with default password 'foobar'. It can manage other users.

# Test a package

If you want to test deb/rpm package you've already built, please use this:
```
builder/test-deb ubuntu focal
```
It will install the package inside a new container and run KasmVNC.

Open browser and point to https://localhost:443/ or https://\<ip-address\>:443/

# Package development

## deb/rpm package building and testing

First, a tarball is built, and then its files are copied to deb/rpm package as
it is being built.
Package testing stage installs the deb/rpm package in a fresh docker container
and runs KasmVNC.

```
builder/build-tarball debian buster
builder/build-deb debian buster
builder/test-deb debian buster
```

Use `build-and-test-deb` to perform the whole dev lifecycle, but to iterate
quickly, you'll need to skip building the tarball (which takes a long time), and
just build your deb/rpm with `build-deb` and test with `test-deb`.

`build-rpm` and `test-rpm` are also available.

## Ensuring packages have all dependencies they need.

If you're working on a deb/rpm package, testing that it has all the necessary
dependencies is done via testing in a barebones environment (read: no XFCE). In
this way we can be sure that runtime dependencies aren't met accidentally by
packages installed with XFCE.

```
builder/test-deb-barebones ubuntu focal
```
# CI development

S3 upload code is extracted to various files in `.ci`. It's possible to iterate
locally by doing stuff like this:

```
bash -c '
. .ci/upload.sh;
prepare_upload_filename "bionic/kasmvncserver_0.9.1~beta-1+libjpeg-turbo-latest_amd64.deb";
echo $upload_filename;'
```
