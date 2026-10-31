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
if [ -f "$PATCH_ROOT/inputs" ]; then
    cat "$PATCH_ROOT/inputs" >> "$SERIES_INPUT"
    while IFS= read -r input || [ -n "$input" ]; do
        case "$input" in
            ''|'#'*) continue ;;
        esac
        hash_file "$ROOT/$input" >> "$SERIES_INPUT"
    done < "$PATCH_ROOT/inputs"
fi
SERIES_HASH=$(hash_file "$SERIES_INPUT")

WEBKIT_ELF="$STAGE/lib/libWPEWebKit-2.0.so.1"
LAUNCHER_ELF="$STAGE/wpe-drm-minimal"
test -f "$WEBKIT_ELF"
test -f "$LAUNCHER_ELF"

PROJECT_ROOT="$ROOT" node "$ROOT/scripts/write_build_manifest.mjs" \
  "$OUTPUT" "$STAGE" "${WEBKIT_BUILD:?WEBKIT_BUILD is required}/CMakeCache.txt" \
  "$REVISION" "$SERIES_HASH"
