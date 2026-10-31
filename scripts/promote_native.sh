#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
bundle="${1:?Usage: promote_native.sh VERIFIED_BUNDLE}"
jsapi="$WPE_BUILD_ROOT/stage/jsapi/libjsapi_browser.so"
WPE_VERIFY_RUNTIME="$bundle" node "$PROJECT_ROOT/scripts/verify_build_manifests.mjs" --runtime-only
node "$PROJECT_ROOT/scripts/verify_elf_abi.mjs" "$bundle" --closure
node "$PROJECT_ROOT/scripts/verify_runtime_symbols.mjs" "$bundle"
node "$PROJECT_ROOT/scripts/jsapi_manifest.mjs" check "$jsapi" "$(dirname "$jsapi")/build-manifest.json"
mkdir -p "$PROJECT_ROOT/.cache" "$PROJECT_ROOT/.rollback"
backup=$(mktemp -d "$PROJECT_ROOT/.rollback/native-before-local.XXXXXX")
temporary=$(mktemp -d "$PROJECT_ROOT/.cache/runtime-promote.XXXXXX")
cp -a "$bundle/." "$temporary/"
cp -a "$PROJECT_ROOT/libs/arm64-orange" "$backup/jsapi"
cp "$PROJECT_ROOT/release-manifest.json" "$backup/release-manifest.json"
moved_old=0
completed=0
rollback() {
  if [ "$completed" = 0 ] && [ "$moved_old" = 1 ]; then
    if [ -e "$PROJECT_ROOT/assets/wpe-runtime" ]; then
      mv "$PROJECT_ROOT/assets/wpe-runtime" "$backup/rejected-runtime"
    fi
    mv "$backup/runtime" "$PROJECT_ROOT/assets/wpe-runtime"
    mv "$PROJECT_ROOT/libs/arm64-orange" "$backup/rejected-jsapi"
    mv "$backup/jsapi" "$PROJECT_ROOT/libs/arm64-orange"
    echo "Native promotion failed; previous artifacts restored from $backup" >&2
  fi
}
trap rollback EXIT
mv "$PROJECT_ROOT/assets/wpe-runtime" "$backup/runtime"
moved_old=1
mv "$temporary" "$PROJECT_ROOT/assets/wpe-runtime"
install -m 0644 "$jsapi" "$PROJECT_ROOT/libs/arm64-orange/libjsapi_browser.so.tmp"
mv "$PROJECT_ROOT/libs/arm64-orange/libjsapi_browser.so.tmp" "$PROJECT_ROOT/libs/arm64-orange/libjsapi_browser.so"
install -m 0644 "$(dirname "$jsapi")/build-manifest.json" "$PROJECT_ROOT/libs/arm64-orange/build-manifest.json"
completed=1
echo "Verified native artifacts promoted; rollback: $backup"
echo 'Rebuild the CPU AMR before publishing; the previous release manifest still describes the baseline.'
