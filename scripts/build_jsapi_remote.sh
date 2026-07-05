#!/usr/bin/env bash
set -euo pipefail

# 构建主机上的工作目录与工具链包，可用环境变量覆盖
BUILD_ROOT="${JSAPI_BUILD_ROOT:-/home/parallels/miniapp-jsapi-build}"
TOOLCHAIN_TARBALL="${JSAPI_TOOLCHAIN_TARBALL:-aarch64--glibc--stable-2018.11-1.tar.bz2}"

cd "$BUILD_ROOT"

TC="$BUILD_ROOT/toolchain/aarch64--glibc--stable-2018.11-1"

# 工具链不变，仅在缺失时解包并 relocate（此步最耗时）
if [ ! -x "$TC/bin/aarch64-buildroot-linux-gnu-gcc" ]; then
  rm -rf toolchain
  mkdir -p toolchain
  tar -xf "$TOOLCHAIN_TARBALL" -C toolchain
  (cd "$TC" && ./relocate-sdk.sh)
fi

printf '%s\n' '#!/bin/sh' "exec qemu-x86_64 \"$TC/bin/aarch64-buildroot-linux-gnu-gcc\" \"\$@\"" > cc-aarch64.sh
printf '%s\n' '#!/bin/sh' "exec qemu-x86_64 \"$TC/bin/aarch64-buildroot-linux-gnu-g++\" \"\$@\"" > cxx-aarch64.sh
chmod +x cc-aarch64.sh cxx-aarch64.sh

# 保留 build-jsapi 目录以获得增量编译；configure 仅在缓存缺失时执行
if [ ! -f build-jsapi/CMakeCache.txt ]; then
  cmake -S jsapi -B build-jsapi \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$BUILD_ROOT/cc-aarch64.sh" \
    -DCMAKE_CXX_COMPILER="$BUILD_ROOT/cxx-aarch64.sh"
fi

cmake --build build-jsapi -j8
file build-jsapi/libjsapi_browser.so
