#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/wpe-bookworm}"
WORK="${WORK:-$HOME/wpe-hostabi-work}"
PREFIX="/opt/wpe-hostabi"
TARBALL="${TARBALL:-$HOME/wpe-hostabi-runtime.tar.gz}"
JOBS="${JOBS:-1}"
HOST_LIBSTDCXX="${HOST_LIBSTDCXX:-$HOME/libstdc++.so.6.0.29}"
DEBIAN_MIRROR="${DEBIAN_MIRROR:-http://deb.debian.org/debian}"
SECURITY_MIRROR="${SECURITY_MIRROR:-http://deb.debian.org/debian-security}"

if [ "$(uname -m)" != "aarch64" ]; then
  echo "This script must run on an aarch64 Linux build host." >&2
  exit 1
fi

run_sudo() {
  sudo "$@"
}

if [ ! -d "$ROOT/bin" ]; then
  run_sudo debootstrap --arch=arm64 bookworm "$ROOT" "$DEBIAN_MIRROR"
fi

run_sudo tee "$ROOT/etc/apt/sources.list" >/dev/null <<EOF
deb $DEBIAN_MIRROR bookworm main
deb-src $DEBIAN_MIRROR bookworm main
deb $SECURITY_MIRROR bookworm-security main
deb-src $SECURITY_MIRROR bookworm-security main
EOF

for mountpoint in proc sys dev dev/pts; do
  if ! mountpoint -q "$ROOT/$mountpoint"; then
    case "$mountpoint" in
      proc) run_sudo mount -t proc proc "$ROOT/proc" ;;
      sys) run_sudo mount --rbind /sys "$ROOT/sys" ;;
      dev) run_sudo mount --rbind /dev "$ROOT/dev" ;;
      dev/pts) run_sudo mount -t devpts devpts "$ROOT/dev/pts" || true ;;
    esac
  fi
done

run_sudo mkdir -p "$ROOT$WORK"
if [ ! -f "$HOST_LIBSTDCXX" ]; then
  echo "Missing host ABI libstdc++: $HOST_LIBSTDCXX" >&2
  echo "Copy device /usr/lib/libstdc++.so.6.0.29 there before running." >&2
  exit 4
fi
run_sudo mkdir -p "$ROOT/opt/wpe-hostabi-link/lib"
run_sudo cp "$HOST_LIBSTDCXX" "$ROOT/opt/wpe-hostabi-link/lib/libstdc++.so.6.0.29"
run_sudo ln -sf libstdc++.so.6.0.29 "$ROOT/opt/wpe-hostabi-link/lib/libstdc++.so.6"
run_sudo ln -sf libstdc++.so.6 "$ROOT/opt/wpe-hostabi-link/lib/libstdc++.so"
run_sudo tee "$ROOT/tmp/build-wpe-hostabi-inner.sh" >/dev/null <<'INNER'
#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export LC_ALL=C.UTF-8

PREFIX="/opt/wpe-hostabi"
LINK_LIBSTDCXX="/opt/wpe-hostabi-link/lib"
WORK="${WORK:-/root/wpe-hostabi-work}"
JOBS="${JOBS:-1}"

# 依赖不变时跳过 apt（联网最慢的一步）；需要重装时删除 stamp 文件
APT_STAMP="/var/lib/wpe-hostabi-apt.stamp"
if [ ! -f "$APT_STAMP" ]; then
  apt-get update
  apt-get install -y \
    build-essential gcc-11 g++-11 cmake ninja-build meson pkg-config \
    curl ca-certificates xz-utils gzip tar patch patchelf file git \
    python3 python3-distutils ruby bison flex gperf gettext-base perl \
    libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libsoup-3.0-dev libcairo2-dev libfreetype6-dev libharfbuzz-dev \
    libatk1.0-dev libseccomp-dev libepoxy-dev libsystemd-dev \
    libxml2-dev libsqlite3-dev libxslt1-dev liblcms2-dev libwoff-dev \
    libjpeg-dev libpng-dev libopenjp2-7-dev libwebp-dev libtasn1-6-dev \
    libfontconfig1-dev libgcrypt20-dev libegl1-mesa-dev libgles2-mesa-dev \
    libgbm-dev libdrm-dev libwayland-dev libxkbcommon-dev
  touch "$APT_STAMP"
fi

mkdir -p "$WORK/src" "$WORK/build" "$PREFIX"
cd "$WORK/src"

download() {
  local url="$1"
  local file="${url##*/}"
  if [ ! -f "$file" ]; then
    curl -fL --retry 5 --retry-delay 2 -o "$file" "$url"
  fi
}

download "https://github.com/unicode-org/icu/releases/download/release-72-1/icu4c-72_1-src.tgz"
download "https://wpewebkit.org/releases/libwpe-1.14.0.tar.xz"
download "https://wpewebkit.org/releases/wpebackend-fdo-1.14.2.tar.xz"
download "https://wpewebkit.org/releases/wpewebkit-2.38.6.tar.xz"

export CC=gcc-11
export CXX=g++-11
export CFLAGS="-O2 -pipe"
export CXXFLAGS="-O2 -pipe"
export LDFLAGS="-L$LINK_LIBSTDCXX -Wl,-rpath-link,$LINK_LIBSTDCXX -Wl,--enable-new-dtags -Wl,-rpath,'\$ORIGIN'"
export LIBRARY_PATH="$LINK_LIBSTDCXX${LIBRARY_PATH:+:$LIBRARY_PATH}"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export CMAKE_PREFIX_PATH="$PREFIX"
export LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}"

bad_existing=""
if [ -d "$PREFIX/lib" ]; then
  bad_existing="$(
    find "$PREFIX/lib" -type f -name "*.so*" -print0 |
      xargs -0 -r readelf --version-info 2>/dev/null |
      grep -E 'GLIBCXX_3\.4\.(3[0-9]|[4-9][0-9])|GLIBC_2\.(3[7-9]|[4-9][0-9])' || true
  )"
fi
if [ -n "$bad_existing" ]; then
  echo "Existing private prefix exceeds host ABI; rebuilding install prefix."
  rm -rf "$PREFIX"
  mkdir -p "$PREFIX"
fi

if [ ! -f "$PREFIX/lib/libicuuc.so.72" ]; then
  rm -rf icu
  tar -xzf icu4c-72_1-src.tgz
  cd icu/source
  ./configure \
    --prefix="$PREFIX" \
    --libdir="$PREFIX/lib" \
    --disable-static \
    --enable-shared \
    --disable-samples \
    --disable-tests
  make -j"$JOBS"
  make install
  cd "$WORK/src"
fi

if [ ! -f "$PREFIX/lib/libwpe-1.0.so.1" ]; then
  rm -rf libwpe-1.14.0 "$WORK/build/libwpe"
  tar -xJf libwpe-1.14.0.tar.xz
  meson setup "$WORK/build/libwpe" libwpe-1.14.0 \
    --prefix="$PREFIX" \
    --libdir=lib \
    --buildtype=release
  ninja -C "$WORK/build/libwpe" -j"$JOBS"
  ninja -C "$WORK/build/libwpe" install
fi

if [ ! -f "$PREFIX/lib/libWPEBackend-fdo-1.0.so.1" ]; then
  rm -rf wpebackend-fdo-1.14.2 "$WORK/build/wpebackend-fdo"
  tar -xJf wpebackend-fdo-1.14.2.tar.xz
  meson setup "$WORK/build/wpebackend-fdo" wpebackend-fdo-1.14.2 \
    --prefix="$PREFIX" \
    --libdir=lib \
    --buildtype=release
  ninja -C "$WORK/build/wpebackend-fdo" -j"$JOBS"
  ninja -C "$WORK/build/wpebackend-fdo" install
fi

if [ ! -f "$PREFIX/lib/libWPEWebKit-1.1.so.0" ]; then
  if [ ! -d wpewebkit-2.38.6 ]; then
    tar -xJf wpewebkit-2.38.6.tar.xz
  fi
  cmake -S wpewebkit-2.38.6 -B "$WORK/build/wpewebkit" -GNinja \
    -DPORT=WPE \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_INSTALL_RPATH="\$ORIGIN" \
    -DCMAKE_BUILD_RPATH="$PREFIX/lib" \
    -DCMAKE_C_COMPILER=gcc-11 \
    -DCMAKE_CXX_COMPILER=g++-11 \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DCMAKE_EXE_LINKER_FLAGS="-L$LINK_LIBSTDCXX -Wl,-rpath-link,$LINK_LIBSTDCXX" \
    -DCMAKE_SHARED_LINKER_FLAGS="-L$LINK_LIBSTDCXX -Wl,-rpath-link,$LINK_LIBSTDCXX" \
    -DCMAKE_MODULE_LINKER_FLAGS="-L$LINK_LIBSTDCXX -Wl,-rpath-link,$LINK_LIBSTDCXX" \
    -DENABLE_ACCESSIBILITY=OFF \
    -DENABLE_DOCUMENTATION=OFF \
    -DENABLE_MINIBROWSER=OFF \
    -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
    -DENABLE_INTROSPECTION=OFF \
    -DENABLE_JIT=OFF \
    -DENABLE_DFG_JIT=OFF \
    -DENABLE_FTL_JIT=OFF \
    -DENABLE_WEBASSEMBLY=OFF \
    -DENABLE_WEBASSEMBLY_B3JIT=OFF \
    -DENABLE_GAMEPAD=OFF \
    -DENABLE_VIDEO=OFF \
    -DENABLE_VIDEO_PRESENTATION_MODE=OFF \
    -DENABLE_VIDEO_USES_ELEMENT_FULLSCREEN=OFF \
    -DENABLE_MEDIA_CONTROLS_SCRIPT=OFF \
    -DENABLE_MEDIA_RECORDER=OFF \
    -DENABLE_MEDIA_SESSION=OFF \
    -DENABLE_MEDIA_SOURCE=OFF \
    -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_ENCRYPTED_MEDIA=OFF \
    -DENABLE_LEGACY_ENCRYPTED_MEDIA=OFF \
    -DENABLE_WEBDRIVER=OFF \
    -DENABLE_WEB_AUDIO=OFF \
    -DENABLE_WEBGL=OFF \
    -DENABLE_WEBGL2=OFF \
    -DENABLE_WEBXR=OFF \
    -DENABLE_WEB_RTC=OFF \
    -DENABLE_PDFJS=OFF \
    -DUSE_LCMS=OFF \
    -DUSE_OPENJPEG=OFF \
    -DUSE_WOFF2=OFF
  ninja -C "$WORK/build/wpewebkit" -j"$JOBS"
  ninja -C "$WORK/build/wpewebkit" install
fi

copy_runtime_dependency_closure() {
  local root_lib="/usr/lib/aarch64-linux-gnu"
  local lib_path
  local lib_name

  LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
    ldd "$PREFIX/lib/libWPEWebKit-1.1.so.0" \
        "$PREFIX/lib/libWPEBackend-fdo-1.0.so.1" \
        "$PREFIX/lib/libwpe-1.0.so.1" |
    awk '/=> \/(usr\/)?lib\// { print $3 }' |
    sort -u |
    while IFS= read -r lib_path; do
      lib_name="$(basename "$lib_path")"
      case "$lib_name" in
        libstdc++*|libc.so*|libm.so*|libgcc_s*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|ld-linux*|linux-vdso*)
          continue
          ;;
      esac

      # 只拷贝该库本体及其版本后缀变体，避免同前缀的无关库被通配带入
      for src in "$root_lib/$lib_name" "$root_lib/$lib_name".* "$lib_path" "$lib_path".*; do
        [ -e "$src" ] && cp -a "$src" "$PREFIX/lib"/
      done
    done
}

copy_runtime_dependency_closure

find "$PREFIX/lib" -type f -name "*.so*" -print0 | while IFS= read -r -d '' lib; do
  if file "$lib" | grep -q "ELF"; then
    patchelf --set-rpath '$ORIGIN' "$lib" || true
    strip --strip-unneeded "$lib" || true
  fi
done

patch_private_gio_tls() {
  local module="$PREFIX/lib/gio/modules/libgiognutls.so"
  local gnutls_real
  local private_name="libwpehostabi-gnutls.so.30"
  local private_path="$PREFIX/lib/$private_name"

  [ -f "$module" ] || return 0
  gnutls_real="$(readlink -f "$PREFIX/lib/libgnutls.so.30")"
  [ -f "$gnutls_real" ] || return 0

  cp -a "$gnutls_real" "$private_path"
  strip --strip-unneeded "$private_path" || true
  patchelf --set-soname "$private_name" "$private_path"
  patchelf --replace-needed libgnutls.so.30 "$private_name" "$module"
  patchelf --set-rpath '$ORIGIN/../..:$ORIGIN' "$module"
}

patch_private_gio_tls

if find "$PREFIX" -name 'libstdc++.so*' | grep -q .; then
  echo "ERROR: private runtime must not contain libstdc++.so" >&2
  find "$PREFIX" -name 'libstdc++.so*' >&2
  exit 2
fi

bad="$(
  find "$PREFIX/lib" -type f -name "*.so*" -print0 |
    xargs -0 -r readelf --version-info 2>/dev/null |
    grep -E 'GLIBCXX_3\.4\.(3[0-9]|[4-9][0-9])|GLIBC_2\.(3[7-9]|[4-9][0-9])' || true
)"
if [ -n "$bad" ]; then
  echo "ERROR: private runtime exceeds host ABI:" >&2
  echo "$bad" >&2
  exit 3
fi

find "$PREFIX" -type f -o -type l | sort > "$PREFIX/manifest.txt"
tar -C /opt -czf /tmp/wpe-hostabi-runtime.tar.gz wpe-hostabi
echo "runtime ready: /tmp/wpe-hostabi-runtime.tar.gz"
INNER

run_sudo chmod +x "$ROOT/tmp/build-wpe-hostabi-inner.sh"
run_sudo chroot "$ROOT" /usr/bin/env WORK="$WORK" JOBS="$JOBS" /bin/bash /tmp/build-wpe-hostabi-inner.sh
run_sudo cp "$ROOT/tmp/wpe-hostabi-runtime.tar.gz" "$TARBALL"
run_sudo chown "$(id -u):$(id -g)" "$TARBALL"
echo "copied runtime tarball to $TARBALL"
