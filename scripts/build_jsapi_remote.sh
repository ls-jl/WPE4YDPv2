#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/build-env.sh"
BUILD_ROOT="$JSAPI_BUILD_ROOT"
TOOLCHAIN_ROOT="$JSAPI_TOOLCHAIN_ROOT"
CC="$TOOLCHAIN_ROOT/bin/aarch64-buildroot-linux-gnu-gcc"
CXX="$TOOLCHAIN_ROOT/bin/aarch64-buildroot-linux-gnu-g++"
OUTPUT="$BUILD_ROOT/build/libjsapi_browser.so"
DESTINATION="${JSAPI_STAGE:-$WPE_BUILD_ROOT/stage/jsapi/libjsapi_browser.so}"

for tool in "$CC" "$CXX"; do
    if [ ! -x "$tool" ]; then
        echo "error: local AArch64 toolchain is missing: $tool" >&2
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
node "$PROJECT_ROOT/scripts/verify_elf_abi.mjs" "$DESTINATION"
node "$PROJECT_ROOT/scripts/jsapi_manifest.mjs" write "$DESTINATION" "$(dirname "$DESTINATION")/build-manifest.json" "$CXX"
sha256sum "$DESTINATION"
