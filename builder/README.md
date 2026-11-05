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

# Build an apk package

## Requirements
Building an apk package requires a signing key to be present in environment.

For example:
```sh
KASMVNC_ALPINE_PRIVATE_KEY=$(cat <<EOF
-----BEGIN PRIVATE KEY-----
...
-----END PRIVATE KEY-----
EOF
)

KASMVNC_ALPINE_PUBLIC_KEY=$(cat <<EOF
-----BEGIN PUBLIC KEY-----
...
-----END PUBLIC KEY-----
EOF
)

export KASMVNC_ALPINE_PRIVATE_KEY
export KASMVNC_ALPINE_PUBLIC_KEY
```

You can create a signing key by using:
```sh
apk add abuild
abuild-keygen -a
```
## Building
```sh
builder/build-package alpine 321
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

For incremental builds inside a developer container,
`builder/build.sh --no-force-autoreconf --no-servertarball` lets Autotools
regenerate only outputs whose inputs changed. Portable profile entrypoints also
forward this option; keep using the same profile for those rebuilds.
The option runs `autoreconf -i`. Cold and package builds keep the default forced
regeneration (`autoreconf -i -f`) needed by the portable EL9 builder. Omit the
opt-out when changing the build toolchain or recreating the build environment.

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
set -e;
. .ci/helpers.sh;
upload_packages ubuntu jammy;'
```

If ran outside CI, it's in local dev mode. Local dev mode omits calling actual
upload code (for example, it doesn't call `curl`).

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
The resulting deb package can be found under `~/KasmVNC/builder/build/ubuntu_focal`.
Replace ```focal``` with ```noble``` to build for Ubuntu 24.04LTS.


## Portable application packages

This profile replaces eligible distro builds in the default pipeline.
From a clean checkout on a native x86_64 or ARM64 Docker host with Bash, Git
and Python 3 installed, first prepare `builder/www` using the existing web build
workflow, then run:

```sh
builder/build-portable --ffmpeg-mode adapters
```

The Oracle Linux 9 builder compiles the application and embedded dependencies,
then produces `.deb`, `.rpm`, `.tar.gz`, checksums, provenance and an ELF audit
under `builder/build/portable_amd64/` or `builder/build/portable_arm64/`.
`KASMVNC_PORTABLE_VERSION` can override the inferred three-part release version;
`BUILD_JOBS` controls compilation concurrency. Do not reuse an in-source build
from a different distro or architecture in the Docker context.
Adapter mode uses host FFmpeg ABI families 58–63 and their encoder libraries.
The local command still defaults to the original static FFmpeg profile when
no mode is specified; CI explicitly requests adapters for both libcs and
architectures. See [the runtime family table](portable/README.md#ffmpeg-runtime-adapters).

CI jobs `build_portable_amd64` and `build_portable_arm64` reuse `build_www`,
upload to the existing immutable `kasmvnc/<commit>/` S3 prefix, and retain
GitLab artifacts. Full build logs are retained as
`output/portable_<arch>/build.log` even when a build fails; they are not
published to S3. Console output contains bounded progress and failure details.
`test_portable_amd64` and `test_portable_arm64` install those
packages and tar archives in the eligible core-image distro matrix. These jobs
are required when enabled. `BUILD_PORTABLE=false` disables both portable profiles and restores the non-Alpine legacy matrix;
`BUILD_DISTROS_REGEX=none` disables it alongside the other packaging profiles.

To reproduce one matrix case locally:

```sh
builder/test-portable builder/build/portable_amd64 --arch amd64 \
  --case ubuntu-jammy --results /tmp/kasm-portable-jammy
```

The matrix is recorded in `builder/portable-tests/matrix.json`, including its
source revision and exclusions. Tests exercise the installed Perl launcher,
TLS authentication, browser software H.264, keyboard input and package/config
lifecycle. Pass `--previous-package /path/to/older.deb` (or `.rpm`) to test an
actual package upgrade; without it, lifecycle coverage is reinstall/removal,
not an upgrade from an older release. GPU, hardware diversity and older host
kernels require separate testing. Containers share the runner kernel.

See [installation and runtime requirements](portable/README.md) before using
these artifacts, and [implementation details](STATIC_BUILD.md) for the static
library boundary and security maintenance implications. Do not retire existing
packages until manual end-to-end distro, hardware and kernel qualification is
complete. Converting the Perl launcher to C++ is deferred until this approach
has been validated.

## Portable CI and legacy fallback

The default pipeline builds glibc DEB/RPM/tar packages and separate musl APKs
for amd64 and arm64. Native distro builds remain for Ubuntu Focal, Oracle 8
and openSUSE 15. The 31 glibc cases and eight Alpine 3.21–3.24 cases are mandatory.
Functional and vncserver specification tests consume the portable amd64 DEB.
Set `BUILD_FULL_LEGACY=true` to additionally build/test the other non-Alpine
traditional distros. `BUILD_PORTABLE=false` disables both portable profiles,
including Alpine CI, and restores the non-Alpine traditional matrix and Jammy
test artifacts. Old Alpine recipes remain available locally for comparison;
per-release Alpine build jobs have been retired.
`BUILD_DISTROS_REGEX` still filters legacy builds; it does not narrow the portable
test matrix. Its special `none` value disables portable builds as before.

This reduces feature-branch jobs from 101 to 59 and package builds from 34 to 10
compared with the combined portable/legacy pipeline after Nix was removed.
Compare total job duration (runner time), pipeline wall time, and artifact bytes
against the prior successful pipeline; counts alone do not predict time savings.
Public rollout remains subject to manual host/kernel/GPU validation.

Separate build-ID debug symbols are published in DEB, RPM and tar formats, with
checksums and a `.debug.json` mapping. See `DEBUGGING.md` for exact-build matching
and the existing crashpad lookup. Portable SBOM, source-bundle and advisory scan
artifacts are described in `builder/portable/README.md`.

Portable builds reuse content-keyed, precompiled native dependency images. See
[portable cache maintenance](portable/README.md#reusing-compiled-dependencies)
for the complete build context, security refresh and registry retention policy.

## Portable musl APKs

Use `builder/build-portable --libc musl --ffmpeg-mode adapters` for the native Alpine profile. It shares
source recipes and cache orchestration with glibc, but builds separate musl archives.
See [musl builds, installation, signing and maintenance](musl/README.md).

### Cached launcher spec environment

`spec_dependencies` prepares a registry image in parallel with package builds.
Its key covers the architecture, `builder/spec-dependencies/` recipes and refresh
marker, and the root `Pipfile` and `Pipfile.lock`. It caches desktop prerequisites,
Pipenv, and the locked Python environment; it never contains a KasmVNC package or
test checkout. Increment the refresh marker to refresh distro packages/base image.

`builder/test-vncserver` resolves that image by immutable digest, installs the
current build's DEB, and mounts the current checkout for every run. Registry
errors fail closed; only a missing manifest triggers a build. Local invocation
uses Docker's local layer cache without publishing. Portable `spec_test` waits
only for this environment and `build_portable_amd64`; legacy mode retains its
legacy package dependencies. `builder/portable/test-spec-cache.py` exercises
cache hits, misses, registry errors and recipe-label rejection.

Keep `spec-dependencies` tags matching `^spec-(amd64|arm64)-[0-9a-f]{64}$`
when configuring registry cleanup; deleting one safely causes a cold rebuild.
