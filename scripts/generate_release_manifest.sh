#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

AMR="8001779591038449.1_0_0.amr"
JSAPI="libs/arm64-orange/libjsapi_browser.so"
LAUNCHER="assets/wpe-runtime/wpe-drm-minimal"
WEBKIT="assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1"
RUNTIME_MANIFEST="assets/wpe-runtime/build-manifest.json"
OUTPUT="release-manifest.json"

for file in "$AMR" "$JSAPI" "$LAUNCHER" "$WEBKIT" "$RUNTIME_MANIFEST"; do
  if [ ! -f "$file" ]; then
    echo "error: release artifact missing: $file" >&2
    exit 1
  fi
done

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

PATCH_HASH=$(node -e 'const fs=require("fs"); const m=JSON.parse(fs.readFileSync(process.argv[1])); process.stdout.write(m.patch_series_sha256)' "$RUNTIME_MANIFEST")
WEBKIT_REVISION=$(tr -d '[:space:]' < wpe-drm/webkit-patches/revision)

cat > "$OUTPUT.tmp" <<EOF
{
  "schema": 1,
  "webkit_revision": "$WEBKIT_REVISION",
  "patch_series_sha256": "$PATCH_HASH",
  "artifacts": {
    "$AMR": "$(hash_file "$AMR")",
    "$JSAPI": "$(hash_file "$JSAPI")",
    "$LAUNCHER": "$(hash_file "$LAUNCHER")",
    "$WEBKIT": "$(hash_file "$WEBKIT")"
  }
}
EOF
mv "$OUTPUT.tmp" "$OUTPUT"
chmod 644 "$OUTPUT"
echo "Release manifest: $OUTPUT"
