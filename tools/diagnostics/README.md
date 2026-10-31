# Device Diagnostics

These programs are optional field-diagnostics helpers. They are not part of the
MiniApp or WPE runtime build.

Build host-native binaries with `make -C tools/diagnostics`. Cross-compile for
the dictionary pen by overriding `CC`, for example:

```sh
make -C tools/diagnostics CC=aarch64-buildroot-linux-gnu-gcc
```

Touch injectors and DRM readers require device permissions. Do not run them
while the production browser owns the same input or DRM resources.

When explicitly diagnosing the production browser, retain/freeze the owned
source and scanout buffers first, and arrange an unconditional resume trap.
`dmabuf_dump PID FD WIDTH HEIGHT STRIDE OUTPUT.ppm` reads an exported ARGB
DMA-BUF with paired CPU cache synchronization. Never capture a buffer that can
be recycled concurrently. PPM files may contain private webpage content and
must remain outside Git. `evdev_touch_injector` is single-contact only; use a
disposable page and do not inject concurrently with a user's physical touches.

`video_overlay_probe` is a C++11 ARM64 diagnostic for the v3 video socket. It
creates two NV12 dumb buffers, verifies the current buffer is not released
early, then checks replacement release and ordered hide/close confirmation.
Run only against a disposable test browser/workdir and stop that browser after
the probe. Override `CXX` and libdrm include/link flags when cross-compiling.

For barcode checks, deploy `frame-integrity.html` as
`frame-integrity-freeze.html`. After its title reports frame 150 and the active
FB remains unchanged, capture the retained test framebuffer and run:

```sh
node tools/diagnostics/frame_integrity_check.mjs capture.ppm 270 44
```

Rotation and inset must match that test run. Coherent owned pixels do not by
themselves prove the absence of a physical scanout artifact.
