# Portable musl APKs

This profile produces one signed KasmVNC APK for x86_64 and one for aarch64.
The baseline is Alpine 3.21 / musl 1.2.5; the runtime matrix covers Alpine
3.21, 3.22, 3.23 and 3.24. APK v2 permits installation with both apk-tools 2
and apk-tools 3. The glibc DEB/RPM/tar files are a different build and cannot
be used as substitutes on Alpine.

Portable CI uses runtime FFmpeg adapters for libavcodec families 58–63 and
system OpenSSL 3. Desktop C libraries and compiler runtimes use host shared
libraries, including libbsd, libstdc++ and libgcc. musl remains dynamic, preserving
runtime loading of graphics drivers. The host provides FFmpeg and its encoder
libraries, OpenSSL 3, musl, Mesa/graphics drivers, timezone data, desktop tools and
keyboard data. Private zlib, prior performance libraries such as WebP/JPEG, and
launcher YAML remain embedded. See the [desktop/compiler boundary](../portable/README.md#system-desktop-and-compiler-libraries). The package installs the C++20 launcher and has no Perl runtime
requirement. The certificate stays at `/etc/ssl/private/kasmvnc.pem`.

## Build and install

Build on a native architecture runner with Docker, Git and Python available:

```sh
builder/build-www
builder/build-portable --libc musl --ffmpeg-mode adapters
```

Alpine 3.21/3.22 use the FFmpeg 6.1 adapter and 3.23/3.24 use the FFmpeg 8.x
adapter. Install the distro's FFmpeg package for video encoding; the shared
libraries must provide the requested encoder. Runtime package updates provide
FFmpeg security fixes; restart sessions after updates. The explicit
`--ffmpeg-mode static` fallback embeds the existing FFmpeg/encoder dependency
closure for explicit experiments; local and CI commands default to adapters.
See [the ABI-family table](../portable/README.md#ffmpeg-runtime-adapters).

OpenSSL is dynamically linked in both FFmpeg modes. The APK explicitly requires
`libssl3` and `libcrypto3`; no TLS library or provider is bundled. Compilation
uses a verified OpenSSL 3.0 SDK and the ELF audit limits imports to its original
3.0.0 ABI, allowing the distro's newer 3.x runtime to supply security fixes.
The host owns provider paths and crypto policy. Restart sessions after updating
OpenSSL, and refresh immutable container images. The SDK version is recorded as
build provenance, never as the installed host version. See
[the OpenSSL runtime boundary](../portable/README.md#system-openssl-3).

Commit source changes before packaging so the source archive and recorded
revision identify the actual code. Output is in
`builder/build/portable_musl_amd64/` or `builder/build/portable_musl_arm64/`.
The filename begins `kasmvncserver_portable_musl_`; the installed package
remains `kasmvncserver`. Runtime and matching debug APKs, debug archives,
SBOMs, source inputs, notices, advisory scan results and checksums are published
through the existing S3 pipeline. Build logs are retained in
`output/portable_musl_<arch>/build.log`, including failures.

CI supplies `KASMVNC_ALPINE_PRIVATE_KEY` and `KASMVNC_ALPINE_PUBLIC_KEY` to
the disposable packaging container. Keys are never Docker build arguments,
cached image contents or source-bundle inputs. The public key accompanies the
artifacts. A local development-only key can be requested explicitly with
`KASMVNC_APK_ALLOW_TEST_KEY=1`; CI refuses that mode.

After obtaining the trusted KasmVNC public signing key and matching APK:

```sh
install -m644 kasmvncserver_portable_musl_VERSION_BRANCH_REV_ARCH.rsa.pub /etc/apk/keys/kasmvnc_signing_key.rsa.pub
apk add --no-cache ./kasmvncserver_portable_musl_VERSION_BRANCH_REV_ARCH.apk
```

Use the actual downloaded filenames. Do not use `--allow-untrusted`; the matrix
tests verify the signatures as well as installation, configuration preservation,
removal/reinstallation, launcher startup, authenticated TLS, software GLX,
software H.264 and keyboard input. The browser runs in a separate test container.

## Dependency image caching

The native jobs use `$CI_REGISTRY_IMAGE/portable-musl-deps` with tags
`portable-musl-deps-<amd64|arm64>-<sha256>`. The complete Docker context consists
of shared recipes from `builder/scripts/portable-dependencies/` and musl-specific
inputs from `builder/musl/dependencies/`. The hash covers the architecture,
filenames, Git executable modes and file contents, including the pinned base
image and dependency versions. Application/packaging changes do not alter it.

Each architecture has its own resource lock. An authenticated manifest 404
triggers a build and push; other registry errors fail rather than being treated
as misses. Existing images are consumed by immutable digest. The same lookup,
canonical context handling and phase logging are used by the glibc profile.
Compiled glibc archives cannot be reused: the native musl toolchain builds its
own archives. No new image tag is created for an ordinary application commit.

For a scoped cleanup policy covering both profiles, the reserved tag pattern is
`^portable-(musl-)?deps-(amd64|arm64)-[0-9a-f]{64}$`. Keep recent builders and
preserve release builders with separately protected release tags. Cleanup is
an administrator setting; the pipeline does not delete images or change the
project-wide policy. Image recipe keys do not make live package repositories
bit-reproducible: exact installed package and source evidence is recorded too.

## Baseline maintenance and limits

Alpine 3.21 was the oldest supported stable release at implementation time
and avoids introducing musl 1.2.6-only symbols. Its main support ended on
November 1, 2026; this is now an EOL build baseline, not a supported distro.
The runtime matrix retains it as an ABI compatibility check. The ABI baseline is useful,
but refreshing a cache against an old repository does not provide security
maintenance. Embedded source pins and any distro-supplied static archives must
be maintained explicitly, as with the glibc portable build. Review moving the
builder forward while preserving the musl 1.2.5 target as support needs change.

Adding a new Alpine release normally requires a runtime matrix entry rather
than another compiler job. Do not promise untested future releases or arbitrary
old host kernels. Container tests share their host kernel; diverse host/kernel
and GPU validation remains separate before public release. Host graphics
libraries must themselves support musl. This build does not make glibc-only
proprietary NVIDIA userspace libraries compatible with Alpine.

Baseline references: [Alpine releases](https://alpinelinux.org/releases/),
[musl distribution ABI guidance](https://wiki.musl-libc.org/guidelines-for-distributions.html),
and [musl loader/threading differences](https://wiki.musl-libc.org/functional-differences-from-glibc.html).

### ELF audit baseline

`audit.py` declares `MUSL_BASELINE` and verifies the audit environment's actual
musl loader version before accepting any ELF files. It also checks architecture,
PIE, interpreter, the direct host-library allowlist, and forbidden relocation,
search-path and glibc symbol-version dependencies. The dependency and application
builders independently require musl 1.2.5.

OpenSSL has a separate symbol boundary: `.so.3` only and the `OPENSSL_3.0.0`
imports available in the verified SDK. If an ELF omits symbol-version metadata,
the audit compares its OpenSSL imports with the SDK's original-ABI exports.
The final source inventory rejects static OpenSSL archive members and hidden
OpenSSL definitions in the unstripped binaries. It also rejects converted desktop
and full compiler-runtime archives; hidden private zlib cannot interpose on the
host font/PNG dependency closure. The APK explicitly lists all required host
providers, and installed provenance records their SONAMEs without builder versions.

Musl does not version its public symbols like glibc. This is a controlled-build
and runtime qualification guarantee, not a claim that ELF metadata alone proves
an arbitrary binary's minimum musl version. Do not audit newer-built binaries in
a baseline container as a substitute for rebuilding and runtime testing them.
