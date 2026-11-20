# DRI3 pixmap lifetime regression

`lifetime.py` starts its own Xvnc, exercises real DRI3 requests, and audits the
server's GBM allocations. Run it inside a dedicated GPU test container with a
render node passed through, a locally built Xvnc, and `cc`, Python 3,
`x11-utils`, `libgbm-dev`, `libxcb1-dev`, and `libxcb-dri3-dev` installed:

```sh
python3 builder/dri3-tests/lifetime.py /src/xorg.build/bin/Xvnc /dev/dri/renderD128
```

Select the actual render node. Use a GPU/driver supported by the current DRI3
implementation (for example Intel/Mesa); this regression does not add NVIDIA
support. The test reserves display `:72` and ports 5972/6972 inside the container.
Use `--display` if needed, and keep the container separate from user sessions.

For each of three server lifetimes the test performs 100 imports and 100 exports,
including repeated exports and imported buffers re-exported through the texture
tracking list. After the deferred cleanup timer, only one buffer may remain:
a pixmap retained as the root window background. Shutdown must release that
buffer before destroying the GBM device. The server must exit successfully.
Logs and compiled probes are saved under `/tmp/dri3-lifetime` by default; use
`--build-dir` to preserve them on a mounted directory.

Unpatched master retains all 201 buffers instead of one before shutdown.
This is an integration regression because the bug involves Xorg pixmap reference
counts, DRI3 fd transport, the texture timer and driver-owned GBM buffers; the
C++ unit container does not supply a running Xorg server or GPU. It complements
`builder/test-unit` and `builder/test-unit sanitizers` rather than replacing them.
