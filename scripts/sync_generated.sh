#!/bin/sh
# 打包前同步"单源文件"到打包位置，消除双份手工维护：
#   wpe-drm/run.sh                        -> assets/wpe-runtime/run.sh
#   libs/arm64-orange/libjsapi_browser.so -> libs/libjsapi_browser_12345.so（模块加载入口命名，已 gitignore）
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

require_file() {
  if [ ! -f "$1" ]; then
    echo "error: required file missing: $1" >&2
    exit 1
  fi
}

reject_lfs_pointer() {
  if head -n 1 "$1" | grep -q "git-lfs.github.com/spec"; then
    echo "error: $1 is a Git LFS pointer, not a usable binary" >&2
    exit 1
  fi
}

validate_webkit_elf() {
  path="$1"
  # The production library is stripped with --strip-unneeded and is currently
  # about 99 MiB. Keep a conservative floor while validating the ELF identity
  # instead of coupling integrity to the old unstripped file size.
  min_size=$((90 * 1024 * 1024))
  size="$(wc -c <"$path" | tr -d ' ')"
  description="$(file -b "$path")"

  case "$description" in
    *ELF*"shared object"*"ARM aarch64"*) ;;
    *)
      echo "error: WebKit runtime is not an AArch64 ELF shared object: $description" >&2
      exit 1
      ;;
  esac
  case "$description" in
    *"missing section headers"*|*"section extending past end of file"*)
      echo "error: WebKit runtime is truncated: $description" >&2
      exit 1
      ;;
  esac
  if [ "$size" -lt "$min_size" ]; then
    echo "error: WebKit runtime is unexpectedly small: $size bytes" >&2
    exit 1
  fi
}

require_file "$ROOT/wpe-drm/run.sh"
require_file "$ROOT/wpe-drm/runtime/gpu-runtime.sh"
require_file "$ROOT/libs/arm64-orange/libjsapi_browser.so"
require_file "$ROOT/assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1.10.2"
require_file "$ROOT/assets/wpe-runtime/build-manifest.json"
for runtime_file in \
  lib/libcrypto.so.3 \
  lib/libssl.so.3 \
  lib/libsrtp2.so.2.8.0 \
  lib/libnice.so.10.14.0 \
  lib/libgstsctp-1.0.so.0.2212.0 \
  lib/libgstwebrtc-1.0.so.0.2212.0 \
  lib/libgstwebrtcnice-1.0.so.0.2212.0 \
  lib/gstreamer-1.0/libgstnice.so \
  lib/gstreamer-1.0/libgstrtp.so \
  lib/gstreamer-1.0/libgstrtpmanager.so \
  lib/gstreamer-1.0/libgstsrtp.so \
  lib/gstreamer-1.0/libgstdtls.so \
  lib/gstreamer-1.0/libgstsctp.so \
  lib/gstreamer-1.0/libgstwebrtc.so
do
  require_file "$ROOT/assets/wpe-runtime/$runtime_file"
done
reject_lfs_pointer "$ROOT/libs/arm64-orange/libjsapi_browser.so"
reject_lfs_pointer "$ROOT/assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1.10.2"
validate_webkit_elf "$ROOT/assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1.10.2"

cp "$ROOT/wpe-drm/run.sh" "$ROOT/assets/wpe-runtime/run.sh"
chmod +x "$ROOT/assets/wpe-runtime/run.sh"
rm -rf "$ROOT/assets/wpe-runtime/runtime"
mkdir -p "$ROOT/assets/wpe-runtime/runtime"
cp "$ROOT"/wpe-drm/runtime/*.sh "$ROOT/assets/wpe-runtime/runtime/"
chmod +x "$ROOT"/assets/wpe-runtime/runtime/*.sh

PLUGIN_DIR="$ROOT/assets/wpe-runtime/lib/gstreamer-1.0"
PLUGIN_FINGERPRINT="$ROOT/assets/wpe-runtime/share/gstreamer-plugin-set.sha256"
mkdir -p "$(dirname "$PLUGIN_FINGERPRINT")"
(
  cd "$PLUGIN_DIR"
  find . -maxdepth 1 -type f -name '*.so' | LC_ALL=C sort | while IFS= read -r plugin; do
    if command -v sha256sum >/dev/null 2>&1; then
      sha256sum "$plugin"
    else
      shasum -a 256 "$plugin"
    fi
  done
) >"$PLUGIN_FINGERPRINT.tmp"
mv "$PLUGIN_FINGERPRINT.tmp" "$PLUGIN_FINGERPRINT"
chmod 600 "$PLUGIN_FINGERPRINT"

cp "$ROOT/libs/arm64-orange/libjsapi_browser.so" "$ROOT/libs/libjsapi_browser_12345.so"
chmod +x "$ROOT/libs/libjsapi_browser_12345.so"
