# FFmpeg adapter build inputs

This prototype compiles adapters for libavcodec families 60 and 61 into one
KasmVNC application. The corresponding FFmpeg 6.1 and 7.1 headers are isolated at
`/opt/kasm-ffmpeg-headers/60/include` and `/opt/kasm-ffmpeg-headers/61/include`.
`sources.json` pins official release archives by SHA-256. `install-headers`
configures each release on the native target architecture/libc and installs only
public headers, including generated `libavutil/avconfig.h`; it does not compile
FFmpeg or an encoder library.

The canonical glibc prototype build is:

```sh
builder/build-www
KASMVNC_PORTABLE_VERSION=1.4.0 builder/build-portable --ffmpeg-mode adapters
```

Choose the appropriate release version for the branch. As with static portable
builds, tracked source changes must be committed before packaging: the build
archives that exact revision and records it in corresponding-source evidence.
There is no dirty-tree packaging bypass. Packages and their image tags include
the FFmpeg mode so static and adapter outputs can be distinguished.

The Alpine path uses the same interface:

```sh
KASMVNC_PORTABLE_VERSION=1.4.0 KASMVNC_APK_ALLOW_TEST_KEY=1 \
  builder/build-portable --libc musl --ffmpeg-mode adapters
```

The test key is for local validation only; release builds require the normal APK
signing configuration. Header preparation occurs above the existing immutable
dependency image and does not invalidate its expensive compiler/library cache.
For an inner-loop build, prepare only that cheap image layer:

```sh
docker build --target ffmpeg-headers -f builder/portable/Dockerfile \
  --build-arg BUILDER_IMAGE=your-existing-portable-dependency-image \
  --build-arg KASMVNC_FFMPEG_MODE=adapters \
  -t kasmvnc-ffmpeg-adapters-headers:local .
```

Pass `KASMVNC_FFMPEG_MODE=adapters` to the container's build entry point. This
selects `KASMVNC_FFMPEG_ADAPTERS=ON`, disables `KASMVNC_STATIC_FFMPEG`, and supplies
`KASMVNC_FFMPEG_HEADERS_ROOT`. The ordinary distro build remains version-specific
dynamic FFmpeg; `--ffmpeg-mode static` preserves the original portable profile.

The source inventory verifies every installed adapter header hash and includes
the exact upstream archives. Its link-evidence check rejects an adapter artifact
that embeds FFmpeg, x264, x265 or SVT-AV1 libraries. Runtime libraries are obtained
from the host and remain subject to that repository's encoder selection, patch
support, and performance. The cached dependency image may contain static codec
archives for the other build mode; presence in that builder is not evidence they
are shipped in the application.

Qualify the same built application on FFmpeg 6.1 and 7.1 runtimes with actual
browser video encoding, forced keyframes, resize/reconfiguration, unsupported or
missing FFmpeg, and file benchmark replay. GPU support requires separate hardware
validation. The initial two-family prototype does not establish video support on
the rest of the portable distro matrix or on untested architectures.
