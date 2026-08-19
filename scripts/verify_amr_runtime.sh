#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

AMR=${1:-8001779591038449.1_0_0.amr}
RUNTIME_ELF=assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1

[ -f "$AMR" ] || {
  echo "verify_amr_runtime: AMR missing: $AMR" >&2
  exit 1
}
[ -f "$RUNTIME_ELF" ] && [ ! -L "$RUNTIME_ELF" ] || {
  echo "verify_amr_runtime: canonical WebKit ELF missing or is a symlink" >&2
  exit 1
}

entries=$(unzip -Z1 "$AMR" | grep '^assets/wpe-runtime/lib/libWPEWebKit-2\.0\.so' || true)
[ "$entries" = "$RUNTIME_ELF" ] || {
  echo "verify_amr_runtime: expected exactly one canonical WebKit entry" >&2
  printf '%s\n' "$entries" >&2
  exit 1
}

source_size=$(wc -c < "$RUNTIME_ELF" | tr -d ' ')
archive_size=$(unzip -l "$AMR" "$RUNTIME_ELF" | awk -v path="$RUNTIME_ELF" '$4 == path { print $1 }')
[ "$archive_size" = "$source_size" ] || {
  echo "verify_amr_runtime: size mismatch source=$source_size archive=$archive_size" >&2
  exit 1
}

hash_stream() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  else
    shasum -a 256 | awk '{print $1}'
  fi
}

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

source_hash=$(hash_file "$RUNTIME_ELF")
archive_hash=$(unzip -p "$AMR" "$RUNTIME_ELF" | hash_stream)
[ "$archive_hash" = "$source_hash" ] || {
  echo "verify_amr_runtime: hash mismatch source=$source_hash archive=$archive_hash" >&2
  exit 1
}

echo "verify_amr_runtime: one WebKit ELF, size=$source_size sha256=$source_hash"
