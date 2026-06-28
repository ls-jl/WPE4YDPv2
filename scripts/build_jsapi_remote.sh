#!/usr/bin/env bash
set -euo pipefail

cd /home/parallels/miniapp-jsapi-build

rm -rf toolchain build-jsapi cc-aarch64.sh cxx-aarch64.sh
mkdir -p toolchain
tar -xf aarch64--glibc--stable-2018.11-1.tar.bz2 -C toolchain

TC=/home/parallels/miniapp-jsapi-build/toolchain/aarch64--glibc--stable-2018.11-1
cd "$TC"
./relocate-sdk.sh

cd /home/parallels/miniapp-jsapi-build
printf '%s\n' '#!/bin/sh' "exec qemu-x86_64 \"$TC/bin/aarch64-buildroot-linux-gnu-gcc\" \"\$@\"" > cc-aarch64.sh
printf '%s\n' '#!/bin/sh' "exec qemu-x86_64 \"$TC/bin/aarch64-buildroot-linux-gnu-g++\" \"\$@\"" > cxx-aarch64.sh
chmod +x cc-aarch64.sh cxx-aarch64.sh

cmake -S jsapi -B build-jsapi \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=/home/parallels/miniapp-jsapi-build/cc-aarch64.sh \
  -DCMAKE_CXX_COMPILER=/home/parallels/miniapp-jsapi-build/cxx-aarch64.sh

cmake --build build-jsapi -j8
file build-jsapi/libjsapi_browser.so
