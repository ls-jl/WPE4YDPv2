#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD_ROOT="${JSAPI_BUILD_ROOT:-/home/pve/web-browser-jsapi-build}"
TOOLCHAIN_ROOT="${JSAPI_TOOLCHAIN_ROOT:-/home/pve/wpe_lite/toolchains/aarch64--glibc--stable-2018.11-1}"
JOBS="${JOBS:-72}"
CC="$TOOLCHAIN_ROOT/bin/aarch64-buildroot-linux-gnu-gcc"
CXX="$TOOLCHAIN_ROOT/bin/aarch64-buildroot-linux-gnu-g++"
OUTPUT="$BUILD_ROOT/build/libjsapi_browser.so"
DESTINATION="$PROJECT_ROOT/libs/arm64-orange/libjsapi_browser.so"

for tool in "$CC" "$CXX"; do
    if [ ! -x "$tool" ]; then
        echo "error: PVE AArch64 toolchain is missing: $tool" >&2
        exit 1
    fi
done

mkdir -p "$BUILD_ROOT/build" "$(dirname "$DESTINATION")"
cmake -S "$PROJECT_ROOT/jsapi" -B "$BUILD_ROOT/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX"
cmake --build "$BUILD_ROOT/build" -j"$JOBS"

file "$OUTPUT"
case "$(file -b "$OUTPUT")" in
    *ELF*shared\ object*ARM\ aarch64*) ;;
    *)
        echo "error: unexpected JSAPI output architecture: $OUTPUT" >&2
        exit 1
        ;;
esac

temporary="$DESTINATION.tmp.$$"
install -m 0644 "$OUTPUT" "$temporary"
mv -f "$temporary" "$DESTINATION"
sha256sum "$DESTINATION"
