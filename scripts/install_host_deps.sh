#!/usr/bin/env bash
set -euo pipefail
if [ "$(id -u)" -ne 0 ]; then
  echo "Run interactively: sudo bash scripts/install_host_deps.sh" >&2
  exit 1
fi
owner="${SUDO_USER:?Run through sudo as the repository owner}"
install -d -o "$owner" -g "$(id -gn "$owner")" -m 0755 /srv/wpe-build
apt-get update
apt-get install --no-install-recommends \
  build-essential git-lfs ripgrep cmake ninja-build clang-18 lld-18 llvm-18 \
  meson pkg-config libglib2.0-dev libsqlite3-dev libfreetype6-dev \
  bison flex gperf unifdef libwayland-bin ruby perl python3 python3-venv python3-jinja2 \
  libxml2-utils gettext autoconf automake libtool patch rsync \
  curl ca-certificates gnupg debian-archive-keyring unzip zip xz-utils bzip2
