#!/usr/bin/env bash
set -euo pipefail

# Run this script on the PVE build host. It reuses the existing Buildroot
# GStreamer dependency prefix and the current WPE 2.53 DRM build.
BASE="${BASE:-/home/pve/wpe_lite}"
WPE_ROOT="${WPE_ROOT:-/home/pve/wpe-lite2/stripped}"
WEBKIT_BASE="${WEBKIT_BASE:-$WPE_ROOT/WebKit}"
WEBKIT_WORKTREE="${WEBKIT_WORKTREE:-$WPE_ROOT/worktrees/webkit-production}"
WEBKIT_SOURCE="$WEBKIT_WORKTREE"
WEBKIT_BUILD="${WEBKIT_BUILD:-$WPE_ROOT/build-drm-aarch64-production}"
FULL_PREFIX="${FULL_PREFIX:-$BASE/deps-aarch64-fullfat}"
PREFIX="${WEBRTC_PREFIX:-$BASE/deps-aarch64-webrtc}"
WORK="${WEBRTC_WORK:-$BASE/build/webrtc-runtime}"
DOWNLOADS="${WEBRTC_DOWNLOADS:-$BASE/sources/webrtc-downloads}"
STAGE="${WEBRTC_STAGE:-$WPE_ROOT/webrtc-stage}"
JOBS="${JOBS:-72}"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"

BR_TOOLCHAIN="$BASE/toolchains/aarch64--glibc--stable-2018.11-1"
BR_SYSROOT="$BR_TOOLCHAIN/aarch64-buildroot-linux-gnu/sysroot"
DEB_SYSROOT="/home/pve/toolchains/debian12-arm64-sysroot-min"
DEVICE_CXX="/home/pve/toolchains/device-libstdcxx29/lib"
CROSS_FILE="$BASE/build/clang-aarch64-buildroot-fullfat-stage.ini"

mkdir -p "$PREFIX" "$WORK/bin" "$DOWNLOADS" "$STAGE"

PROJECT_ROOT="$PROJECT_ROOT" WEBKIT_BASE="$WEBKIT_BASE" \
  WEBKIT_WORKTREE="$WEBKIT_WORKTREE" \
  "$PROJECT_ROOT/scripts/prepare_webkit_worktree.sh"

cat >"$WORK/bin/aarch64-cc" <<EOF
#!/bin/sh
exec clang --target=aarch64-buildroot-linux-gnu --gcc-toolchain="$BR_TOOLCHAIN" \
  -B"$BR_TOOLCHAIN/bin" -B"$BR_TOOLCHAIN/aarch64-buildroot-linux-gnu/bin" \
  --sysroot="$BR_SYSROOT" "\$@"
EOF
cat >"$WORK/bin/aarch64-cxx" <<EOF
#!/bin/sh
exec clang++ --target=aarch64-buildroot-linux-gnu --gcc-toolchain="$BR_TOOLCHAIN" \
  -B"$BR_TOOLCHAIN/bin" -B"$BR_TOOLCHAIN/aarch64-buildroot-linux-gnu/bin" \
  --sysroot="$BR_SYSROOT" "\$@"
EOF
cat >"$WORK/bin/aarch64-deb12-cc" <<EOF
#!/bin/sh
exec clang --target=aarch64-buildroot-linux-gnu --gcc-toolchain="$BR_TOOLCHAIN" \
  -B"$BR_TOOLCHAIN/bin" -B"$BR_TOOLCHAIN/aarch64-buildroot-linux-gnu/bin" \
  --sysroot="$DEB_SYSROOT" "\$@"
EOF
chmod 755 "$WORK/bin/aarch64-cc" "$WORK/bin/aarch64-cxx" "$WORK/bin/aarch64-deb12-cc"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$FULL_PREFIX/lib/pkgconfig:$FULL_PREFIX/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH:$BR_SYSROOT/usr/lib/pkgconfig:$BR_SYSROOT/usr/share/pkgconfig"
export PATH="$WORK/bin:$PATH"

download() {
  local url="$1" output="$2"
  if [ ! -s "$output" ]; then
    curl -fL --retry 5 --retry-delay 2 "$url" -o "$output"
  fi
}

download "https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz" \
  "$DOWNLOADS/openssl-3.5.7.tar.gz"
download "https://github.com/cisco/libsrtp/archive/refs/tags/v2.8.0.tar.gz" \
  "$DOWNLOADS/libsrtp-2.8.0.tar.gz"

GST_BAD_SOURCE="$BASE/sources/gst-plugins-bad-1.22.12"
GST_WEBRTC_PATCH="$PROJECT_ROOT/wpe-drm/gstreamer-patches/0001-webrtcbin-update-remote-offer-transceivers.patch"
GST_ICE_ROLE_PATCH="$PROJECT_ROOT/wpe-drm/gstreamer-patches/0002-webrtcbin-preserve-ice-controller.patch"
GST_DTLS_MTU_PATCH="$PROJECT_ROOT/wpe-drm/gstreamer-patches/0003-dtls-bio-query-mtu-1200.patch"

apply_patch_once() {
  local source="$1" patch_file="$2"
  if patch -d "$source" -p1 -R --dry-run <"$patch_file" >/dev/null 2>&1; then
    return 0
  fi
  if patch -d "$source" -p1 --dry-run <"$patch_file" >/dev/null 2>&1; then
    patch -d "$source" -p1 <"$patch_file"
    return 0
  fi
  echo "Source does not match clean or fully patched state: $patch_file" >&2
  return 1
}
if [ ! -f "$PREFIX/lib/libcrypto.so.3" ]; then
  rm -rf "$WORK/openssl-3.5.7"
  tar -xzf "$DOWNLOADS/openssl-3.5.7.tar.gz" -C "$WORK"
  pushd "$WORK/openssl-3.5.7" >/dev/null
  CC="$WORK/bin/aarch64-cc" \
  AR="$BR_TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-ar" \
  RANLIB="$BR_TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-ranlib" \
    ./Configure linux-aarch64 --prefix="$PREFIX" --libdir=lib shared \
      no-tests no-docs no-apps
  make -j"$JOBS"
  make install_sw
  popd >/dev/null
fi

if [ ! -f "$PREFIX/lib/libsrtp2.so" ]; then
  rm -rf "$WORK/libsrtp-2.8.0" "$WORK/libsrtp-build"
  tar -xzf "$DOWNLOADS/libsrtp-2.8.0.tar.gz" -C "$WORK"
  cmake -S "$WORK/libsrtp-2.8.0" -B "$WORK/libsrtp-build" -GNinja \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$WORK/bin/aarch64-cc" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DENABLE_OPENSSL=ON \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DTEST_APPS=OFF
  ninja -C "$WORK/libsrtp-build" -j"$JOBS" install
fi
cat >"$PREFIX/lib/pkgconfig/libsrtp2.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libsrtp2
Description: Secure RTP library
Version: 2.8.0
Requires.private: openssl
Libs: -L\${libdir} -lsrtp2
Cflags: -I\${includedir}
EOF

meson_build() {
  local name="$1" source="$2"
  shift 2
  local build="$WORK/$name"
  rm -rf "$build"
  PKG_CONFIG_PATH="$PKG_CONFIG_PATH" meson setup "$build" "$source" \
    --cross-file "$CROSS_FILE" --prefix="$PREFIX" --libdir=lib \
    --buildtype=release -Dauto_features=disabled "$@"
  PKG_CONFIG_PATH="$PKG_CONFIG_PATH" ninja -C "$build" -j"$JOBS" install
}

if [ ! -f "$PREFIX/lib/gstreamer-1.0/libgstnice.so" ]; then
  meson_build libnice "$BASE/sources/libnice-0.1.22" \
    -Dgstreamer=enabled -Dcrypto-library=openssl -Dgupnp=disabled \
    -Dexamples=disabled -Dtests=disabled -Dgtk_doc=disabled -Dintrospection=disabled
fi

if [ ! -f "$PREFIX/lib/gstreamer-1.0/libgstrtp.so" ] || \
  [ ! -f "$PREFIX/lib/gstreamer-1.0/libgstrtpmanager.so" ]; then
  meson_build gst-good "$BASE/sources/gst-plugins-good-1.22.12" \
    -Drtp=enabled -Drtpmanager=enabled -Dexamples=disabled -Dtests=disabled
fi

if [ ! -f "$PREFIX/lib/gstreamer-1.0/libgstdtls.so" ] || \
  [ ! -f "$PREFIX/lib/gstreamer-1.0/libgstsctp.so" ] || \
  [ ! -f "$PREFIX/lib/gstreamer-1.0/libgstsrtp.so" ] || \
  [ ! -f "$PREFIX/lib/gstreamer-1.0/libgstwebrtc.so" ] || \
  [ "${FORCE_REBUILD_GST_BAD:-0}" = 1 ]; then
  apply_patch_once "$GST_BAD_SOURCE" "$GST_WEBRTC_PATCH"
  apply_patch_once "$GST_BAD_SOURCE" "$GST_ICE_ROLE_PATCH"
  apply_patch_once "$GST_BAD_SOURCE" "$GST_DTLS_MTU_PATCH"
  meson_build gst-bad "$GST_BAD_SOURCE" \
    -Ddtls=enabled -Dsctp=enabled -Dsrtp=enabled -Dwebrtc=enabled \
    -Dsctp-internal-usrsctp=enabled -Dexamples=disabled -Dtests=disabled
fi

existing_cache_flag() {
  sed -n "s/^$1:[^=]*=//p" "$WEBKIT_BUILD/CMakeCache.txt" | head -n 1
}
EXE_FLAGS="$(existing_cache_flag CMAKE_EXE_LINKER_FLAGS)"
SHARED_FLAGS="$(existing_cache_flag CMAKE_SHARED_LINKER_FLAGS)"
C_FLAGS="$(existing_cache_flag CMAKE_C_FLAGS)"
CXX_FLAGS="$(existing_cache_flag CMAKE_CXX_FLAGS)"
case "$EXE_FLAGS" in *"$PREFIX/lib"*) ;; *) EXE_FLAGS="$EXE_FLAGS -L$PREFIX/lib -Wl,-rpath-link,$PREFIX/lib" ;; esac
case "$SHARED_FLAGS" in *"$PREFIX/lib"*) ;; *) SHARED_FLAGS="$SHARED_FLAGS -L$PREFIX/lib -Wl,-rpath-link,$PREFIX/lib" ;; esac
case "$C_FLAGS" in *"-I$PREFIX/include"*) ;; *) C_FLAGS="-I$PREFIX/include $C_FLAGS" ;; esac
case "$CXX_FLAGS" in *"-I$PREFIX/include"*) ;; *) CXX_FLAGS="-I$PREFIX/include $CXX_FLAGS" ;; esac

if [ "${SKIP_WEBKIT_BUILD:-0}" != 1 ]; then
  PKG_CONFIG_PATH="$PKG_CONFIG_PATH" cmake -S "$WEBKIT_SOURCE" -B "$WEBKIT_BUILD" \
    -DENABLE_WEB_RTC=ON \
    -DUSE_GSTREAMER_WEBRTC=ON \
    -DENABLE_MEDIA_STREAM=ON \
    -DENABLE_VIDEO=ON \
    -DUSE_LIBRICE=OFF \
    -DENABLE_GPU_PROCESS=OFF \
    -DENABLE_WEBGL=ON \
    -DUSE_GBM=ON \
    -DUSE_SKIA=ON \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.so" \
    -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.so" \
    -DOPENSSL_USE_STATIC_LIBS=FALSE \
    -DCMAKE_PREFIX_PATH="$PREFIX;$FULL_PREFIX" \
    -DCMAKE_C_FLAGS="$C_FLAGS" \
    -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$EXE_FLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$SHARED_FLAGS"

  PKG_CONFIG_PATH="$PKG_CONFIG_PATH" ninja -C "$WEBKIT_BUILD" -j"$JOBS" \
    libWPEWebKit-2.0.so WPEWebProcess WPENetworkProcess
fi

COMMON_CFLAGS="$(PKG_CONFIG_PATH="$PKG_CONFIG_PATH" pkg-config --cflags glib-2.0 gobject-2.0 gio-2.0 libsoup-3.0 sqlite3)"
COMMON_LIBS="$(PKG_CONFIG_PATH="$PKG_CONFIG_PATH" pkg-config --libs glib-2.0 gobject-2.0 gio-2.0 libsoup-3.0 sqlite3)"
"$WORK/bin/aarch64-deb12-cc" -O2 -DNDEBUG \
  -I"$WEBKIT_BUILD/DerivedSources/WebKit" \
  -I"$WEBKIT_BUILD/DerivedSources/WPEPlatform" \
  -I"$WEBKIT_BUILD/JavaScriptCoreGLib/DerivedSources" \
  -I"$WEBKIT_BUILD/JavaScriptCoreGLib/Headers" \
  -I"$WEBKIT_SOURCE/Source/WebKit/UIProcess/API" \
  -I"$WEBKIT_SOURCE/Source/WebKit/WPEPlatform" \
  $COMMON_CFLAGS \
  "$WPE_ROOT/wpe-drm-minimal.c" "$WPE_ROOT/browser-chrome-model.c" \
  "$WPE_ROOT/browser-navigation.c" \
  "$WPE_ROOT/browser-profile-store.c" \
  -o "$WPE_ROOT/wpe-drm-minimal" \
  -L"$WEBKIT_BUILD/lib" -L"$FULL_PREFIX/lib" -L"$PREFIX/lib" \
  -L"$DEVICE_CXX" -Wl,-rpath-link,"$WEBKIT_BUILD/lib" \
  -Wl,-rpath-link,"$FULL_PREFIX/lib" -Wl,-rpath-link,"$PREFIX/lib" \
  -lWPEWebKit-2.0 $COMMON_LIBS -lm -ldl -lpthread -latomic -fuse-ld=lld

rm -rf "$STAGE"
mkdir -p "$STAGE/lib/gstreamer-1.0" "$STAGE/lib/ossl-modules" \
  "$STAGE/libexec/wpe-webkit-2.0"

copy_family() {
  local pattern="$1"
  compgen -G "$PREFIX/lib/$pattern" >/dev/null || return 0
  cp -a $PREFIX/lib/$pattern "$STAGE/lib/"
}
copy_family 'libcrypto.so*'
copy_family 'libssl.so*'
copy_family 'libsrtp2.so*'
copy_family 'libnice.so*'
copy_family 'libgstwebrtc-1.0.so*'
copy_family 'libgstsdp-1.0.so*'
copy_family 'libgstsctp-1.0.so*'
copy_family 'libgstwebrtcnice-1.0.so*'

if [ -d "$PREFIX/lib/ossl-modules" ]; then
  cp -a "$PREFIX/lib/ossl-modules/." "$STAGE/lib/ossl-modules/"
fi
for plugin in libgstnice.so libgstrtp.so libgstrtpmanager.so libgstsrtp.so \
  libgstdtls.so libgstsctp.so libgstwebrtc.so; do
  test -f "$PREFIX/lib/gstreamer-1.0/$plugin"
  cp -a "$PREFIX/lib/gstreamer-1.0/$plugin" "$STAGE/lib/gstreamer-1.0/"
done

cp -a "$WEBKIT_BUILD/lib/libWPEWebKit-2.0.so.1.10.2" "$STAGE/lib/"
cp -a "$WEBKIT_BUILD/bin/WPEWebProcess" "$STAGE/libexec/wpe-webkit-2.0/"
cp -a "$WEBKIT_BUILD/bin/WPENetworkProcess" "$STAGE/libexec/wpe-webkit-2.0/"
cp -a "$WPE_ROOT/wpe-drm-minimal" "$STAGE/"

PROJECT_ROOT="$PROJECT_ROOT" WEBRTC_STAGE="$STAGE" \
  "$PROJECT_ROOT/scripts/generate_build_manifest.sh"

find "$STAGE" -type f -exec chmod go-w {} +
file "$STAGE/lib/libWPEWebKit-2.0.so.1.10.2" "$STAGE/wpe-drm-minimal"
echo "WebRTC runtime stage: $STAGE"
