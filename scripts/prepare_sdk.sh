#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
bash "$PROJECT_ROOT/scripts/check_build_env.sh" --host
if [ -e "$DEB_SYSROOT" ]; then
  echo "Preserving existing sysroot $DEB_SYSROOT; choose a fresh DEB_SYSROOT" >&2
  exit 1
fi
mkdir -p "$WPE_SDK_ROOT/toolchains" "$WPE_DOWNLOADS_ROOT" "$DEVICE_CXX"
archive="${BR_TOOLCHAIN_ARCHIVE:-$DICTPEN_ROOT/miniapp-projects/netease_cloud_gaming/tools/aarch64--glibc--stable-2018.11-1.tar.bz2}"
device_cxx="${DEVICE_CXX_SOURCE:-$DICTPEN_ROOT/emulater/x7/simulator/build/audit-firmware/usr/lib/libstdc++.so.6.0.29}"
test -f "$archive"
test -f "$device_cxx"
if [ ! -d "$BR_TOOLCHAIN" ]; then
  tar -xjf "$archive" -C "$WPE_SDK_ROOT/toolchains"
fi
"$BR_TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-gcc" --version
install -m 0644 "$device_cxx" "$DEVICE_CXX/libstdc++.so.6.0.29"
ln -sfn libstdc++.so.6.0.29 "$DEVICE_CXX/libstdc++.so.6"
ln -sfn libstdc++.so.6 "$DEVICE_CXX/libstdc++.so"
sha256sum "$archive" "$device_cxx" > "$WPE_SDK_ROOT/local-inputs.sha256"

# APT authenticates InRelease and package hashes in private state. No host
# foreign architecture is enabled and no downloaded target executable is run.
bash "$PROJECT_ROOT/scripts/prepare_debian_keys.sh"
keyring="$WPE_SDK_ROOT/keyrings/bookworm.gpg"
test -s "$keyring"
apt_root="$WPE_SDK_ROOT/apt-arm64"
mkdir -p "$apt_root/etc/apt" "$apt_root/var/lib/apt/lists/partial" \
  "$apt_root/var/cache/apt/archives/partial" "$apt_root/var/lib/dpkg" "$apt_root/var/log/apt"
touch "$apt_root/var/lib/dpkg/status"
printf 'deb [arch=arm64 signed-by=%s] https://deb.debian.org/debian bookworm main\n' "$keyring" > "$apt_root/etc/apt/sources.list"
apt_args=(-o "Dir=$apt_root" -o "APT::Architecture=arm64" \
  -o "APT::Architectures::=arm64" -o "APT::Sandbox::User=$(id -un)" \
  -o 'Acquire::AllowInsecureRepositories=false' -o 'APT::Get::AllowUnauthenticated=false')
apt-get "${apt_args[@]}" update
apt-get "${apt_args[@]}" --download-only --no-install-recommends -y install \
  libc6-dev libstdc++-12-dev libgcc-12-dev linux-libc-dev \
  libglib2.0-dev libsoup-3.0-dev libicu-dev libfreetype-dev libharfbuzz-dev \
  libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev libwayland-dev \
  libxkbcommon-dev libinput-dev libudev-dev libsqlite3-dev libxml2-dev libxslt1-dev \
  libjpeg-dev libpng-dev libwebp-dev libopenjp2-7-dev libwoff-dev liblcms2-dev \
  libepoxy-dev libgcrypt20-dev libtasn1-6-dev libsecret-1-dev libenchant-2-dev \
  libatk1.0-dev libatk-bridge2.0-dev libwpe-1.0-dev libwpebackend-fdo-1.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
  libasound2-dev libopus-dev libvpx-dev liborc-0.4-dev zlib1g-dev libbrotli-dev libzstd-dev
temporary="$DEB_SYSROOT.preparing"
test ! -e "$temporary"
mkdir -p "$temporary"
for deb in "$apt_root"/var/cache/apt/archives/*.deb; do
  test -s "$deb"
  dpkg-deb --extract "$deb" "$temporary"
done
printf 'wpe-sdk-sysroot-v1\n' > "$temporary/.wpe-sdk-owned"
mv "$temporary" "$DEB_SYSROOT"
sha256sum "$apt_root"/var/cache/apt/archives/*.deb > "$WPE_SDK_ROOT/debian-packages.sha256"
node "$PROJECT_ROOT/scripts/relocate_sdk_metadata.mjs" "$DEB_SYSROOT"
cat > "$CROSS_FILE" <<EOF
[binaries]
c = '$WPE_CLANG'
cpp = '$WPE_CLANGXX'
ar = '$BR_TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-ar'
strip = '$BR_TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['--target=aarch64-buildroot-linux-gnu', '--gcc-toolchain=$BR_TOOLCHAIN', '--sysroot=$DEB_SYSROOT', '-mcpu=cortex-a53']
cpp_args = ['--target=aarch64-buildroot-linux-gnu', '--gcc-toolchain=$BR_TOOLCHAIN', '--sysroot=$DEB_SYSROOT', '-mcpu=cortex-a53']
c_link_args = ['--target=aarch64-buildroot-linux-gnu', '--gcc-toolchain=$BR_TOOLCHAIN', '--sysroot=$DEB_SYSROOT', '-fuse-ld=lld-18']
cpp_link_args = ['--target=aarch64-buildroot-linux-gnu', '--gcc-toolchain=$BR_TOOLCHAIN', '--sysroot=$DEB_SYSROOT', '-fuse-ld=lld-18', '-L$DEVICE_CXX']
EOF
echo 'Base ARM64 sysroot prepared; media/development overlay validation is still required.'
