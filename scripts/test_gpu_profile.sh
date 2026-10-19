#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
. "$ROOT/wpe-drm/runtime/gpu-runtime.sh"
MALI_LIB=/test/mali
DIR=/test/runtime
GPU_SHIM=/test/shim.so
WPE_FORCE_FULL_REPAINT_OVERRIDE_SET=0
WPE_CPU_PAINTING_THREADS=3
configure_gpu_profile
[ "$WPE_RENDER_PROFILE" = gpu ]
[ "$WEBKIT_SKIA_CPU_COMPOSITOR" = 0 ]
[ "$WEBKIT_SKIA_ENABLE_CPU_RENDERING" = 1 ]
[ "$WEBKIT_SKIA_CPU_PAINTING_THREADS" = 3 ]
[ "$WEBKIT_SKIA_GPU_PAINTING_THREADS" = 0 ]
[ "$WEBKIT_SKIA_USE_LINEAR_TILE_TEXTURES" = 0 ]
[ "$WEBKIT_WEBGL_DISABLE_GBM" = 0 ]
[ "$WPE_DMABUF_BUFFER_FORMAT" = AR24:0:scanout ]
[ "$WPE_DRM_FORCE_SHM" = 0 ]
[ "$LD_LIBRARY_PATH" = /test/mali:/test/runtime/lib ]
unset LD_PRELOAD
WPE_CPU_PAINTING_THREADS=2
configure_gpu_profile
[ "$WEBKIT_SKIA_CPU_PAINTING_THREADS" = 2 ]
unset LD_PRELOAD
echo 'GPU profile passed: CPU tiles, Mali compositor/WebGL and linear scanout'
