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
