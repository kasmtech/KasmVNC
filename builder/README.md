## REQIUREMENTS
Docker CE

# Build a deb/rpm package
```
# builder/build-package <os> <os_codename> <build_tag>
# os_codename is what "lsb_release -c" outputs, e.g. buster, focal.
#
# build_tag allows building multiple versions of deb package (rpm not supported)
# targeting a single distro release (e.g. Ubuntu Focal). If build_tag is given,
# the package name will include build_tag as part of Debian revision. For
# example:
# * with build_tag: kasmvncserver_0.9.1~beta-1+libjpeg-turbo-latest_amd64.deb
# * without build_tag: kasmvncserver_0.9.1~beta-1_amd64.deb
# You need to have .build and .deb.build for the build_tag.
#
# Packages will be placed under builder/build/

builder/build-package ubuntu focal
builder/build-package debian buster
builder/build-package debian bullseye
builder/build-package kali kali-rolling
builder/build-package fedora thirtythree
```

# Build and test a package
```
builder/build-and-test-deb ubuntu focal
builder/build-and-test-rpm oracle 8
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

## Debugging

To debug a test container, run `test-deb ubuntu focal -s` or `test-deb ubuntu
focal --shell`. It runs a shell instead of starting KasmVNC.

## Testing performance

To run a performance test, add `-p` option:
```
builder/test-deb ubuntu focal -p
```

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

# Preparing a release

Deb and rpm packages need their versions bumped to the new release version. It
can be done with:

```
builder/bump-package-version 0.9.4-beta
```

This will update corresponding package files, use `git diff` to see changes.

If you've ran the command and curious about Debian version specifics, here's an
explanation:
Deb version will be `0.9.4~beta-1`. `~` (and not `-`) is required by packaging
guidelines, and `-1` is Debian package revision for `0.9.4` upstream release. If
a Debian-specific patch was later added on top of `0.9.4`, it'd be `-2` for the
next Debian version. Rpm has a corresponding revision in its .spec file.

# CI development

S3 upload code is extracted to various files in `.ci`. It's possible to iterate
locally by doing stuff like this:

```
bash -c '
. .ci/helpers.sh;
prepare_upload_filename "focal/kasmvncserver_0.9.1~beta-1+libjpeg-turbo-latest_amd64.deb";
echo $upload_filename;'
```

# vncserver development

## Building a Ubuntu Focal package
```
builder/build-package ubuntu focal
```

You don't need to `build-package` every time you change something, but you need
`Xvnc` and other files.

## Running a development environment

```
builder/devenv-vncserver
```

`devenv-vncserver` starts a dockerized development environment. Edit
`vncserver`, its modules, and configuration files as usual. When you
run`vncserver`, not `/usr/bin/vncserver` will be used, but `./unix/vncserver`.
Same for configuration files.

Python specs can be run with `ty` alias inside devenv.

### Debug output
To see `vncserver` output, run `ty -d`. Anything you print inside `vncserver`
with `say` should be visible that way.

### Finding which spec produces a warning
By default, each passing test is represented as a dot. `-v` flag causes tests to
emit test descriptions alongside the program's output. Run `ty -v`.

# ARM

KasmVNC is supported on ARM, however, the build process needs to be broken into two parts with one occuring on a x64 system and the other on an ARM system. All our testing and official builds are done on AWS Graviton instances.

### Build www code on x86 System
The www code is webpacked for performance and thus requires building. There are NPM packages, phantomjs, which do not have an ARM build. Therefore, this must be built on x86 and then copied over to the ARM system for final packaging.

```
cd ~/KasmVNC
mkdir builder/www
sudo docker build -t kasmweb/www -f builder/dockerfile.www.build .
sudo docker run --rm -v $PWD/builder/www:/build kasmweb/www:latest
cd builder
tar -zcvf /tmp/kasm_www.tar.gz www
```

Now transfer ```kasm_www.tar.gz``` to the ARM system.

### Build KasmVNC ARM
These instructions assume KasmVNC has been cloned at $HOME and ```kasm_www.tar.gz``` has been placed at $HOME as well, adjust for your environment.

```
cd ~
tar -zxf kasm_www.tar.gz -C KasmVNC/builder/
cd KasmVNC
sudo builder/build-package ubuntu focal
```
The resulting deb package can be found under ~/KasmVNC/builder/build/focal
Replace ```focal``` with ```noble``` to build for Ubuntu 24.04LTS.
