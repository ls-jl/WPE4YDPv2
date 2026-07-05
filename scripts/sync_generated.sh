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

require_file "$ROOT/wpe-drm/run.sh"
require_file "$ROOT/libs/arm64-orange/libjsapi_browser.so"
reject_lfs_pointer "$ROOT/libs/arm64-orange/libjsapi_browser.so"

cp "$ROOT/wpe-drm/run.sh" "$ROOT/assets/wpe-runtime/run.sh"
chmod +x "$ROOT/assets/wpe-runtime/run.sh"

cp "$ROOT/libs/arm64-orange/libjsapi_browser.so" "$ROOT/libs/libjsapi_browser_12345.so"
chmod +x "$ROOT/libs/libjsapi_browser_12345.so"
