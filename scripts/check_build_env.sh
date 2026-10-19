#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
mode="${1:---all}"
case "$mode" in --host|--all) ;; *) echo 'Usage: check_build_env.sh --host|--all' >&2; exit 2 ;; esac
errors=0
missing() { printf 'MISSING: %s\n' "$*" >&2; errors=$((errors + 1)); }
for tool in node npm git cmake ninja "$WPE_CLANG" "$WPE_CLANGXX" ld.lld-18 llvm-profdata-18 \
    meson pkg-config cc c++ readelf rsync curl unzip zip bison flex gperf ruby python3; do
  command -v "$tool" >/dev/null 2>&1 || missing "host command $tool"
done
git lfs version >/dev/null 2>&1 || missing 'git-lfs'
for dependency in glib-2.0 sqlite3 freetype2; do
  pkg-config --exists "$dependency" || missing "host development package $dependency"
done
for value in "$JOBS" "$LINK_JOBS" "$THINLTO_JOBS"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || missing "invalid concurrency $value"
done
[ -d "$WPE_WORK_ROOT" ] && [ -w "$WPE_WORK_ROOT" ] || missing "writable $WPE_WORK_ROOT"
if [ "$mode" != --host ]; then
  for file in "$BR_TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-gcc" \
      "$DEB_SYSROOT/usr/include/stdio.h" "$DEVICE_CXX/libstdc++.so.6.0.29" \
      "$CROSS_FILE" "$PGO_PROFILE_PATH"; do
    [ -e "$file" ] || missing "$file"
  done
  git -C "$WEBKIT_BASE" cat-file -e "$(cat "$PROJECT_ROOT/wpe-drm/webkit-patches/revision")^{commit}" 2>/dev/null || missing 'WebKit baseline commit'
  for source in libnice-0.1.22 gst-plugins-good-1.22.12 gst-plugins-bad-1.22.12; do
    [ -f "$WPE_SOURCE_ROOT/$source/meson.build" ] || missing "independent media source $WPE_SOURCE_ROOT/$source"
  done
  target_pc="$WEBRTC_PREFIX/lib/pkgconfig:$FULL_PREFIX/lib/pkgconfig:$FULL_PREFIX/lib/aarch64-linux-gnu/pkgconfig:$FULL_PREFIX/share/pkgconfig"
  for dependency in gstreamer-1.0 gstreamer-base-1.0; do
    version=$(PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR= PKG_CONFIG_LIBDIR="$target_pc" \
      pkg-config --modversion "$dependency" 2>/dev/null || true)
    [ "$version" = 1.22.12 ] || missing "target $dependency 1.22.12 (found: ${version:-none}); base Debian sysroot is not a complete SDK"
  done
fi
printf 'build root=%s sdk=%s jobs=%s link_jobs=%s thinlto_jobs=%s\n' \
  "$WPE_BUILD_ROOT" "$WPE_SDK_ROOT" "$JOBS" "$LINK_JOBS" "$THINLTO_JOBS"
if [ "$errors" -ne 0 ]; then
  echo "Preflight failed: $errors prerequisites missing; no build started" >&2
  exit 1
fi
available=$(df -Pk "$WPE_WORK_ROOT" | awk 'NR==2 { print $4 }')
if [ "$mode" = --all ] && [ "${available:-0}" -lt 62914560 ]; then
  echo 'Preflight failed: at least 60 GiB free required for a fresh native build' >&2
  exit 1
fi
echo 'Build environment preflight passed'
