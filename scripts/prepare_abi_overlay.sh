#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
node "$PROJECT_ROOT/scripts/fetch_media_sources.mjs"
overlay="$WPE_SDK_ROOT/abi-overlay"
mkdir -p "$overlay/lib" "$overlay/include"
for library in libicudata.so.72 libicuuc.so.72 libicui18n.so.72; do
  cp -L "$PROJECT_ROOT/assets/wpe-runtime/lib/$library" "$overlay/lib/$library"
  ln -sfn "$library" "$overlay/lib/${library%.72}"
done
sha256sum "$overlay"/lib/*.so.72 > "$overlay/baseline-icu.sha256"
cmake -S "$WPE_SOURCE_ROOT/libjpeg-turbo-2.1.5.1" -B "$WPE_BUILD_ROOT/jpeg" -GNinja \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="$(command -v "$WPE_CLANG")" \
  -DCMAKE_C_COMPILER_TARGET=aarch64-buildroot-linux-gnu -DCMAKE_SYSROOT="$DEB_SYSROOT" \
  -DCMAKE_C_FLAGS="--gcc-toolchain=$BR_TOOLCHAIN -mcpu=cortex-a53 -fuse-ld=lld-18" \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld-18 -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld-18 \
  -DCMAKE_INSTALL_PREFIX="$overlay" -DCMAKE_INSTALL_LIBDIR:PATH="$overlay/lib" \
  -DCMAKE_SKIP_INSTALL_RPATH=ON -DCMAKE_BUILD_TYPE=Release \
  -DWITH_JPEG8=ON -DENABLE_SHARED=ON -DENABLE_STATIC=OFF
cmake --build "$WPE_BUILD_ROOT/jpeg" --parallel "$JOBS"
cmake --install "$WPE_BUILD_ROOT/jpeg"
echo "ABI-compatible ICU and JPEG8 developer overlay: $overlay"
