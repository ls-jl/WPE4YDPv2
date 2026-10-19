#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
output="${GPU_BUILD_SOURCE:-$WPE_BUILD_ROOT/gpu}"
mkdir -p "$output"
flags=(--target=aarch64-buildroot-linux-gnu --gcc-toolchain="$BR_TOOLCHAIN" \
  --sysroot="$DEB_SYSROOT" -O2 -fuse-ld=lld-18)
mali=$(node "$PROJECT_ROOT/scripts/fetch_mali_driver.mjs" "$WPE_WORK_ROOT/downloads/mali")
for soname in libEGL.so.1 libGLESv2.so.2 libgbm.so.1; do
  "$WPE_CLANG" "${flags[@]}" -fPIC -shared \
    "$PROJECT_ROOT/wpe-drm/mali-api-wrapper.c" \
    -Wl,-soname,"$soname" -Wl,--no-as-needed "$mali" \
    -o "$output/$soname"
done
"$WPE_CLANG" "${flags[@]}" -fPIC -shared \
  "$PROJECT_ROOT/wpe-drm/mali-gbm-compat.c" -o "$output/libwpe-mali-gbm-compat.so" \
  -Wl,-soname,libwpe-mali-gbm-compat.so -ldl -lpthread
"$WPE_CLANG" "${flags[@]}" -I"$DEB_SYSROOT/usr/include/libdrm" \
  "$PROJECT_ROOT/wpe-drm/wpe-gpu-probe.c" -o "$output/wpe-gpu-probe" \
  -L"$DEB_SYSROOT/usr/lib/aarch64-linux-gnu" \
  -Wl,-rpath-link,"$DEB_SYSROOT/usr/lib/aarch64-linux-gnu" \
  -lEGL -lGLESv2 -lgbm -ldrm -ldl -lm
node "$PROJECT_ROOT/scripts/verify_elf_abi.mjs" "$output"
