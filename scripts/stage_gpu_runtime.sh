#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MALI_SOURCE="${MALI_SOURCE:-/Users/me/Documents/dictpen-rootfs/x7/rootfs-dump/gpu-driver-from-5.14.1/usr/lib}"
MODULE_SOURCE="${MODULE_SOURCE:-/Users/me/Documents/MESA_for_pen/X7_COMMON_KERNEL_MODULES_20260712/release}"
BUILD_SOURCE="${GPU_BUILD_SOURCE:-$ROOT/.cache/gpu-build}"
RUNTIME="$ROOT/assets/wpe-runtime"
MALI_DEST="$RUNTIME/gpu/mali/lib"
MODULE_DEST="$RUNTIME/gpu/modules/5.10.160"

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

copy_checked() {
    source_file="$1"
    expected="$2"
    destination="$3"
    actual=$(hash_file "$source_file")
    if [ "$actual" != "$expected" ]; then
        echo "GPU staging hash mismatch: $source_file expected=$expected actual=$actual" >&2
        exit 1
    fi
    install -m 0755 "$source_file" "$destination"
}

mkdir -p "$MALI_DEST" "$MODULE_DEST" "$RUNTIME/libexec"
for stale_name in \
    libEGL.so libGLESv2.so libgbm.so \
    libmali.so libmali.so.1.9.0 libmali-bifrost-g52-g13p0-dummy-gbm.so \
    libmali_hook.so libmali_hook.so.1.9.0; do
    rm -f "$MALI_DEST/$stale_name"
done
copy_checked "$MALI_SOURCE/libEGL.so.1" \
    67a28c4f732e247e865da7406e6dda043f591c16500be85a7d42db43b73a9808 \
    "$MALI_DEST/libEGL.so.1"
copy_checked "$MALI_SOURCE/libGLESv2.so.2" \
    67ff688cc4f941a32d9067400fde9b4e520d66e921b657adefe7d8304a02b474 \
    "$MALI_DEST/libGLESv2.so.2"
copy_checked "$MALI_SOURCE/libgbm.so.1" \
    3a6500953e33741f0d0b80ebf57daac95f546ba909950201bd6a84802786d2c4 \
    "$MALI_DEST/libgbm.so.1"
copy_checked "$MALI_SOURCE/libmali.so.1.9.0" \
    17bc5343e7281d8641deadd0519fa7726b1788f221b029dc2c2a41c6b42dd4a6 \
    "$MALI_DEST/libmali.so.1"
copy_checked "$MALI_SOURCE/libmali_hook.so.1.9.0" \
    12f1008f72c50153702e32bc8ef8fa92c819c225254647f37689f09090d1b3c8 \
    "$MALI_DEST/libmali_hook.so.1"
copy_checked "$MODULE_SOURCE/bifrost_kbase.ko" \
    a06aeccd04f13038b4473c30dfb3073e4126354a03945a99509954cbe7e5607f \
    "$MODULE_DEST/bifrost_kbase.ko"
copy_checked "$MODULE_SOURCE/x7_gpu_dt_enable.ko" \
    07c6e10025fa7e6e14b61a1d259c04d4b18236db485def4d6280da946934a938 \
    "$MODULE_DEST/x7_gpu_dt_enable.ko"

install -m 0755 "$BUILD_SOURCE/libwpe-mali-gbm-compat.so" \
    "$MALI_DEST/libwpe-mali-gbm-compat.so"
install -m 0755 "$BUILD_SOURCE/wpe-gpu-probe" \
    "$RUNTIME/libexec/wpe-gpu-probe"

echo "GPU runtime staged: mali=$MALI_DEST modules=$MODULE_DEST probe=$RUNTIME/libexec/wpe-gpu-probe"
