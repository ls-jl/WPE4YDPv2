#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
bash "$PROJECT_ROOT/scripts/check_build_env.sh" --host
test -s "$CROSS_FILE"
node "$PROJECT_ROOT/scripts/fetch_media_sources.mjs"
if [ -f "$WPE_SDK_ROOT/media-build-receipt.json" ]; then
  node "$PROJECT_ROOT/scripts/sdk_receipt.mjs" check
  echo 'Reusing verified media SDK'
  exit 0
fi
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$FULL_PREFIX/lib/pkgconfig:$FULL_PREFIX/lib/aarch64-linux-gnu/pkgconfig:$FULL_PREFIX/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
unset PKG_CONFIG_SYSROOT_DIR
mkdir -p "$PREFIX" "$WEBRTC_WORK"
build_component() {
  local name="$1"
  shift
  local source="$WPE_SOURCE_ROOT/$name-1.22.12" build="$WEBRTC_WORK/$name"
  if [ -e "$build" ]; then
    echo "Preserving existing media build: $build; validate its inputs before resuming" >&2
    exit 1
  fi
  meson setup "$build" "$source" --cross-file "$CROSS_FILE" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release \
    -Dauto_features=disabled -Dtests=disabled -Dexamples=disabled \
    -Dintrospection=disabled "$@"
  ninja -C "$build" -j"$JOBS" install
}
build_component gstreamer -Dtools=enabled -Dgst_debug=true -Dcheck=disabled
build_component gst-plugins-base -Dapp=enabled -Daudioconvert=enabled -Daudioresample=enabled \
  -Daudiotestsrc=enabled -Dvideoconvertscale=enabled -Dplayback=enabled -Dtypefind=enabled \
  -Dvolume=enabled -Dalsa=enabled -Dopus=enabled -Dgl=enabled \
  -Dgl_api=gles2 -Dgl_platform=egl -Dgl_winsys=egl,gbm
echo 'GStreamer core/base 1.22.12 development overlay built'
