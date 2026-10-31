#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
directory="$WPE_SDK_ROOT/keyrings"
mkdir -p "$directory"
export GNUPGHOME
GNUPGHOME=$(mktemp -d "$directory/verify.XXXXXX")
trap 'rm -rf "$GNUPGHOME"' EXIT
verify_key() {
  local name="$1" expected="$2"
  curl --proto '=https' -fsSL --retry 3 "https://ftp-master.debian.org/keys/$name.asc" -o "$directory/$name.asc.tmp"
  local actual
  actual=$(gpg --batch --show-keys --with-colons "$directory/$name.asc.tmp" 2>/dev/null | awk -F: '$1=="fpr" {print $10; exit}')
  test "$actual" = "$expected" || { echo "Debian signing key fingerprint mismatch: $name" >&2; exit 1; }
  mv "$directory/$name.asc.tmp" "$directory/$name.asc"
  gpg --batch --yes --dearmor --output "$directory/$name.gpg" "$directory/$name.asc"
}
# Fingerprints published at https://ftp-master.debian.org/keys.html.
verify_key archive-key-12 B8B80B5B623EAB6AD8775C45B7C5D7D6350947F8
verify_key archive-key-12-security 05AB90340C0C5E797F44A8C8254CF3B5AEC0A8F0
verify_key release-12 4D64FEC119C2029067D6E791F8D2585B8783D481
cat "$directory/archive-key-12.gpg" "$directory/archive-key-12-security.gpg" \
  "$directory/release-12.gpg" > "$directory/bookworm.gpg.tmp"
mv "$directory/bookworm.gpg.tmp" "$directory/bookworm.gpg"
