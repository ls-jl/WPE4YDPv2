#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
bundle="${WPE_BUNDLE_STAGE:-$WPE_BUILD_ROOT/stage/bundle}"
test -s "$WEBRTC_STAGE/build-manifest.json"
if [ -e "$bundle" ]; then
  echo "Preserving existing bundle $bundle; select a fresh WPE_BUNDLE_STAGE" >&2
  exit 1
fi
mkdir -p "$bundle"
printf 'wpe-bundle-staging-v1\n' > "$bundle/.wpe-bundle-staging"
cp -a "$PROJECT_ROOT/assets/wpe-runtime/." "$bundle/"
cp -a "$WEBRTC_STAGE/." "$bundle/"
install -m 0755 "$PROJECT_ROOT/wpe-drm/run.sh" "$bundle/run.sh"
mkdir -p "$bundle/runtime"
for module in "$PROJECT_ROOT"/wpe-drm/runtime/*.sh; do
  install -m 0755 "$module" "$bundle/runtime/"
done
if [ -d "$WPE_BUILD_ROOT/stage/gpu-runtime" ]; then
  # Replace the userspace stack as a unit; never retain old firmware hooks.
  rm -rf "$bundle/gpu/mali"
  cp -a "$WPE_BUILD_ROOT/stage/gpu-runtime/." "$bundle/"
fi
# Include the rebuilt core/base libraries as well as the transport overlay.
find "$PREFIX/lib" -maxdepth 1 -type f -name '*.so*' -exec cp -a {} "$bundle/lib/" \;
cp -a "$PREFIX/lib/gstreamer-1.0/." "$bundle/lib/gstreamer-1.0/"
for helper in gst-plugin-scanner gst-ptp-helper; do
  if [ -f "$PREFIX/libexec/gstreamer-1.0/$helper" ]; then
    cp -a "$PREFIX/libexec/gstreamer-1.0/$helper" "$bundle/libexec/gstreamer-1.0/"
  fi
done
# Preserve the firmware ABI used by existing optional codec plugins. These are
# build-time inputs; runtime never falls back to this firmware directory.
firmware="${WPE_FIRMWARE_LIB:-$DICTPEN_ROOT/emulater/x7/simulator/build/audit-firmware/usr/lib}"
: > "$bundle/firmware-inputs.sha256"
for library in libfaad.so.2 libFLAC.so.8 libmpg123.so.0 libogg.so.0 \
  libopus.so.0 libvorbis.so.0 libvorbisenc.so.2 librockchip_mpp.so.1 libz.so.1; do
  test -f "$firmware/$library"
  cp -L "$firmware/$library" "$bundle/lib/$library"
  sha256sum "$firmware/$library" >> "$bundle/firmware-inputs.sha256"
done
cp -L "$FULL_PREFIX/lib/aarch64-linux-gnu/libgudev-1.0.so.0" "$bundle/lib/libgudev-1.0.so.0"
cp -L "$FULL_PREFIX/lib/aarch64-linux-gnu/libxkbcommon.so.0" "$bundle/lib/libxkbcommon.so.0"
# Materialize each new library's actual SONAME, not just its versioned filename.
node "$PROJECT_ROOT/scripts/materialize_sonames.mjs" "$bundle/lib"
node "$PROJECT_ROOT/scripts/verify_elf_abi.mjs" "$bundle" --closure
node "$PROJECT_ROOT/scripts/verify_runtime_symbols.mjs" "$bundle"
PROJECT_ROOT="$PROJECT_ROOT" WEBRTC_STAGE="$bundle" \
  bash "$PROJECT_ROOT/scripts/generate_build_manifest.sh"
echo "Validated full runtime bundle (not promoted): $bundle"
