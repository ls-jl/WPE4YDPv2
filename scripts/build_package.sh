#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
flavor="${1:-cpu}"
case "$flavor" in cpu|gpu|dev) ;; *) echo 'Usage: build_package.sh cpu|gpu|dev' >&2; exit 2 ;; esac
test -w "$WPE_WORK_ROOT" || { echo "Build root is not writable: $WPE_WORK_ROOT" >&2; exit 1; }
node "$PROJECT_ROOT/scripts/verify_build_manifests.mjs" --runtime-only
if [ "$flavor" != dev ]; then
node - "$PROJECT_ROOT/assets/wpe-runtime/build-manifest.json" <<'NODE'
const fs = require('fs')
const manifest = JSON.parse(fs.readFileSync(process.argv[2]))
if (manifest.schema < 2 || !manifest.source_inputs || !manifest.toolchain) {
  console.error('Packaging blocked: native runtime must be rebuilt with local source/toolchain provenance first')
  process.exit(1)
}
NODE
fi
if [ "$flavor" != dev ]; then
  node "$PROJECT_ROOT/scripts/jsapi_manifest.mjs" check \
    "$PROJECT_ROOT/libs/arm64-orange/libjsapi_browser.so" "$PROJECT_ROOT/libs/arm64-orange/build-manifest.json"
fi
stage=$(mktemp -d "$WPE_WORK_ROOT/package-$flavor.XXXXXX")
echo "Isolated package stage: $stage"
for name in src scripts wpe-drm libs licenses; do
  mkdir -p "$stage/$name"
  rsync -a "$PROJECT_ROOT/$name/" "$stage/$name/"
done
cp "$PROJECT_ROOT/package.json" "$PROJECT_ROOT/package-lock.json" "$PROJECT_ROOT/icon.png" "$PROJECT_ROOT/LICENSE" "$stage/"
mkdir -p "$stage/assets"
if [ "$flavor" = cpu ]; then
  rsync -a --exclude='/wpe-runtime/gpu/mali/' --exclude='/wpe-runtime/gpu/modules/' \
    --exclude='/wpe-runtime/libexec/wpe-gpu-probe' --exclude='/wpe-runtime/tests/' \
    --exclude='/wpe-runtime/*.html' --exclude='/wpe-runtime/lib/gstreamer-1.0/include/' \
    --exclude='/wpe-runtime/.wpe-bundle-staging' --exclude='/wpe-runtime/.source-inputs.json' \
    "$PROJECT_ROOT/assets/" "$stage/assets/"
else
  if [ "$flavor" = gpu ]; then bash "$PROJECT_ROOT/scripts/validate_gpu_runtime.sh" required; fi
  rsync -a --exclude='/wpe-runtime/tests/' --exclude='/wpe-runtime/*.html' \
    --exclude='/wpe-runtime/lib/gstreamer-1.0/include/' --exclude='/wpe-runtime/.wpe-bundle-staging' \
    --exclude='/wpe-runtime/.source-inputs.json' "$PROJECT_ROOT/assets/" "$stage/assets/"
fi
if [ "$flavor" = cpu ]; then
  node - "$stage/assets/wpe-runtime/build-manifest.json" <<'NODE'
const fs = require('fs')
const file = process.argv[2]
const manifest = JSON.parse(fs.readFileSync(file))
manifest.package_flavor = 'cpu'
manifest.excluded_private_artifacts = []
for (const key of Object.keys(manifest.artifacts)) {
  if (key.startsWith('gpu/') || key === 'libexec/wpe-gpu-probe') {
    manifest.excluded_private_artifacts.push(key)
    delete manifest.artifacts[key]
  }
}
fs.writeFileSync(file, JSON.stringify(manifest, null, 2))
NODE
fi
ln -s "$PROJECT_ROOT/node_modules" "$stage/node_modules"
(
  cd "$stage"
  PROJECT_ROOT="$stage" bash scripts/sync_generated.sh
  if [ "$flavor" = dev ]; then
    "$PROJECT_ROOT/node_modules/.bin/aiot-cli"
  else
    "$PROJECT_ROOT/node_modules/.bin/aiot-cli" -p
    bash scripts/verify_amr_runtime.sh
    node scripts/verify_packaged_manifest.mjs
  fi
  if [ "$flavor" = cpu ]; then node scripts/verify_public_archive.mjs; fi
)
if [ "$flavor" = dev ]; then
  echo "Development output: $stage/.falcon_ (existing release AMR untouched)"
  exit 0
fi
name=8001779591038449.1_0_0.amr
if [ "$flavor" = cpu ]; then
  destination="$PROJECT_ROOT/$name"
  if [ -f "$destination" ]; then
    mkdir -p "$PROJECT_ROOT/.rollback/packages"
    old_hash=$(sha256sum "$destination" | awk '{print $1}')
    [ -f "$PROJECT_ROOT/.rollback/packages/$old_hash.amr" ] || \
      cp --reflink=auto "$destination" "$PROJECT_ROOT/.rollback/packages/$old_hash.amr"
  fi
else
  mkdir -p "$PROJECT_ROOT/.cache/releases/gpu"
  destination="$PROJECT_ROOT/.cache/releases/gpu/$name"
fi
install -m 0644 "$stage/$name" "$destination.tmp"
mv "$destination.tmp" "$destination"
if [ "$flavor" = cpu ]; then bash "$PROJECT_ROOT/scripts/generate_release_manifest.sh"; fi
sha256sum "$destination"
