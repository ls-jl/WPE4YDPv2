#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE="${1:-optional}"
RUNTIME="$ROOT/assets/wpe-runtime"

fail() {
    echo "GPU runtime validation failed: $*" >&2
    exit 1
}

case "$MODE" in
    optional|required) ;;
    *) fail "usage: $0 [optional|required]" ;;
esac

required_files="
gpu/mali/lib/libEGL.so.1
gpu/mali/lib/libGLESv2.so.2
gpu/mali/lib/libgbm.so.1
gpu/mali/lib/libmali.so.1
gpu/mali/lib/libmali_hook.so.1
gpu/mali/lib/libwpe-mali-gbm-compat.so
gpu/modules/5.10.160/x7_gpu_dt_enable.ko
gpu/modules/5.10.160/bifrost_kbase.ko
libexec/wpe-gpu-probe
"

present=0
for relative in $required_files; do
    [ ! -e "$RUNTIME/$relative" ] || present=$((present + 1))
done

if [ "$present" -eq 0 ]; then
    [ "$MODE" = optional ] || fail "GPU packaging was requested but no GPU assets are staged"
    echo "GPU runtime validation: cpu-only package"
    exit 0
fi

for relative in $required_files; do
    [ -f "$RUNTIME/$relative" ] || fail "partial package, missing $relative"
    if head -n 1 "$RUNTIME/$relative" | grep -q 'git-lfs.github.com/spec'; then
        fail "LFS pointer is not a usable runtime file: $relative"
    fi
done

for relative in \
    gpu/mali/lib/libEGL.so.1 \
    gpu/mali/lib/libGLESv2.so.2 \
    gpu/mali/lib/libgbm.so.1 \
    gpu/mali/lib/libmali.so.1 \
    gpu/mali/lib/libmali_hook.so.1 \
    gpu/mali/lib/libwpe-mali-gbm-compat.so \
    libexec/wpe-gpu-probe; do
    description=$(file -b "$RUNTIME/$relative")
    case "$description" in
        *ELF*"ARM aarch64"*) ;;
        *) fail "$relative is not an AArch64 ELF: $description" ;;
    esac
done

for relative in \
    gpu/modules/5.10.160/x7_gpu_dt_enable.ko \
    gpu/modules/5.10.160/bifrost_kbase.ko; do
    description=$(file -b "$RUNTIME/$relative")
    case "$description" in
        *ELF*"ARM aarch64"*) ;;
        *) fail "$relative is not an AArch64 kernel object: $description" ;;
    esac
    strings "$RUNTIME/$relative" | grep -q 'vermagic=5\.10\.160' \
        || fail "$relative does not target kernel 5.10.160"
done

[ -x "$RUNTIME/libexec/wpe-gpu-probe" ] \
    || fail "libexec/wpe-gpu-probe is not executable"

echo "GPU runtime validation: complete ($present files)"
