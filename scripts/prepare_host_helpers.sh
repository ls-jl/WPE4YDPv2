#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
directory="$WPE_DOWNLOADS_ROOT/host-packages"
prefix="$WPE_SDK_ROOT/host/packages"
mkdir -p "$directory" "$prefix" "$WPE_SDK_ROOT/host/bin"
cd "$directory"
# apt-get download verifies against the host's authenticated package indexes.
# Extract only; never install packages or change the host package database.
for package in unifdef libwayland-bin; do
  apt-get download "$package"
  for deb in "${package}"_*.deb; do
    test "$(dpkg-deb -f "$deb" Architecture)" = amd64
    dpkg-deb --extract "$deb" "$prefix"
  done
done
for tool in unifdef unifdefall wayland-scanner; do
  test -x "$prefix/usr/bin/$tool"
  ln -sfn "$prefix/usr/bin/$tool" "$WPE_SDK_ROOT/host/bin/$tool"
done
sha256sum ./*.deb > "$WPE_SDK_ROOT/host-packages.sha256"
