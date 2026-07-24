# Optional Mali Runtime

This directory is populated locally by `scripts/stage_gpu_runtime.sh`.
Proprietary Mali libraries and kernel modules are intentionally ignored by Git.
When the files are absent, `WPE_GPU_MODE=auto` uses the bundled Skia CPU path.

Supported local input is pinned by SHA-256 in the staging script. The runtime
loader accepts only ARM64 kernel `5.10.160`, verifies module vermagic, and probes
EGL, GLES, linear DMA-BUF export, CPU mapping, and DRM framebuffer creation
before launching WebKit with GPU acceleration.

Only runtime SONAME files such as `libmali.so.1` are staged. Falcon materializes
symlinks while packaging, so shipping unversioned/canonical aliases would create
multiple 42 MB copies without changing dynamic-loader behavior.

The resulting GPU-enabled AMR also contains these proprietary files. Keep that
AMR local; do not stage or push it to the public repository even though the
repository normally tracks CPU-only release AMRs through Git LFS.
