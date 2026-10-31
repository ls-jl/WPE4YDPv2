#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
if [ ! -w "$WPE_WORK_ROOT" ]; then
  echo "Create writable $WPE_WORK_ROOT before provisioning host tools" >&2
  exit 1
fi
mkdir -p "$WPE_DOWNLOADS_ROOT/node" "$WPE_SDK_ROOT/host"
archive=node-v20.15.1-linux-x64.tar.xz
base=https://nodejs.org/dist/v20.15.1
curl --proto '=https' -fSL --retry 3 "$base/SHASUMS256.txt" -o "$WPE_DOWNLOADS_ROOT/node/SHASUMS256.txt.tmp"
mv "$WPE_DOWNLOADS_ROOT/node/SHASUMS256.txt.tmp" "$WPE_DOWNLOADS_ROOT/node/SHASUMS256.txt"
if [ ! -f "$WPE_DOWNLOADS_ROOT/node/$archive" ]; then
  curl --proto '=https' -fSL --retry 3 "$base/$archive" -o "$WPE_DOWNLOADS_ROOT/node/$archive.tmp"
  mv "$WPE_DOWNLOADS_ROOT/node/$archive.tmp" "$WPE_DOWNLOADS_ROOT/node/$archive"
fi
(
  cd "$WPE_DOWNLOADS_ROOT/node"
  grep "  $archive\$" SHASUMS256.txt | sha256sum --check --strict
)
if [ ! -x "$WPE_NODE_ROOT/bin/node" ]; then
  tar -xJf "$WPE_DOWNLOADS_ROOT/node/$archive" -C "$WPE_SDK_ROOT/host"
fi
test "$("$WPE_NODE_ROOT/bin/node" --version)" = v20.15.1
"$WPE_NODE_ROOT/bin/npm" --version
echo 'Host Node ready. Enable with: source scripts/build-env.sh'
