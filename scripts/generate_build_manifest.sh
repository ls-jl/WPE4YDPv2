#!/usr/bin/env bash
set -euo pipefail

ROOT="${PROJECT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
STAGE="${WEBRTC_STAGE:?WEBRTC_STAGE is required}"
PATCH_ROOT="$ROOT/wpe-drm/webkit-patches"
OUTPUT="$STAGE/build-manifest.json"
REVISION="$(tr -d '[:space:]' < "$PATCH_ROOT/revision")"

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

SERIES_INPUT=$(mktemp "${TMPDIR:-/tmp}/wpe-patch-series.XXXXXX")
trap 'rm -f "$SERIES_INPUT"' EXIT
cat "$PATCH_ROOT/series" > "$SERIES_INPUT"
while IFS= read -r patch || [ -n "$patch" ]; do
    case "$patch" in
        ''|'#'*) continue ;;
    esac
    hash_file "$PATCH_ROOT/$patch" >> "$SERIES_INPUT"
done < "$PATCH_ROOT/series"
SERIES_HASH=$(hash_file "$SERIES_INPUT")

WEBKIT_ELF="$STAGE/lib/libWPEWebKit-2.0.so.1"
LAUNCHER_ELF="$STAGE/wpe-drm-minimal"
test -f "$WEBKIT_ELF"
test -f "$LAUNCHER_ELF"

cat > "$OUTPUT.tmp" <<EOF
{
  "schema": 1,
  "webkit_revision": "$REVISION",
  "patch_series_sha256": "$SERIES_HASH",
  "build_flags": {
    "ENABLE_WEB_RTC": true,
    "USE_GSTREAMER_WEBRTC": true,
    "ENABLE_MEDIA_STREAM": true,
    "ENABLE_VIDEO": true,
    "ENABLE_WEBGL": true,
    "USE_AVIF": true,
    "USE_JPEGXL": false,
    "USE_GBM": true,
    "USE_SKIA": true,
    "ENABLE_GPU_PROCESS": false
  },
  "artifacts": {
    "lib/libWPEWebKit-2.0.so.1": "$(hash_file "$WEBKIT_ELF")",
    "wpe-drm-minimal": "$(hash_file "$LAUNCHER_ELF")"
  }
}
EOF
mv "$OUTPUT.tmp" "$OUTPUT"
chmod 600 "$OUTPUT"
echo "Build manifest: $OUTPUT"
