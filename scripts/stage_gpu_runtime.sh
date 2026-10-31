#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source "$ROOT/scripts/build-env.sh"
MALI_DRIVER=$(node "$ROOT/scripts/fetch_mali_driver.mjs" "$WPE_WORK_ROOT/downloads/mali")
MODULE_SOURCE="${MODULE_SOURCE:-$DICTPEN_ROOT/wpe-runtime/gpu/modules/5.10.160}"
BUILD_SOURCE="${GPU_BUILD_SOURCE:-$WPE_BUILD_ROOT/gpu}"
RUNTIME="${GPU_RUNTIME_DESTINATION:-$WPE_BUILD_ROOT/stage/gpu-runtime}"
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

test -f "$BUILD_SOURCE/libwpe-mali-gbm-compat.so"
test -x "$BUILD_SOURCE/wpe-gpu-probe"
mkdir -p "$MALI_DEST" "$MODULE_DEST" "$RUNTIME/libexec"
for stale_name in \
    libEGL.so libGLESv2.so libgbm.so \
    libmali.so libmali.so.1.9.0 libmali-bifrost-g52-g13p0-dummy-gbm.so \
    libmali_hook.so libmali_hook.so.1 libmali_hook.so.1.9.0; do
    rm -f "$MALI_DEST/$stale_name"
done
for soname in libEGL.so.1 libGLESv2.so.2 libgbm.so.1; do
    install -m 0755 "$BUILD_SOURCE/$soname" "$MALI_DEST/$soname"
done
copy_checked "$MALI_DRIVER" \
    123514cc2e91c52fdb3bf0ac544b0bd2ad2052d359e642ab2e88f5fda2415991 \
    "$MALI_DEST/libmali.so.1"
mkdir -p "$RUNTIME/share/licenses/mali"
cp "$ROOT/licenses/mali/"* "$RUNTIME/share/licenses/mali/"
install -m 0644 "$ROOT/LICENSE" "$RUNTIME/share/licenses/mali/WRAPPERS-MIT.txt"
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
