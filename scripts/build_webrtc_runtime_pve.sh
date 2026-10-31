#!/usr/bin/env bash
set -euo pipefail

# Compatibility filename: builds locally; no SSH or old-host paths are used.
source "$(dirname "$0")/build-env.sh"
bash "$PROJECT_ROOT/scripts/check_build_env.sh" --all
BASE="$WPE_WORK_ROOT"
WPE_ROOT="$WPE_BUILD_ROOT"
WEBKIT_SOURCE="$WEBKIT_WORKTREE"
WORK="$WEBRTC_WORK"
DOWNLOADS="$WPE_DOWNLOADS_ROOT/media"
STAGE="$WEBRTC_STAGE"

if [ -e "$STAGE" ]; then
  echo "Refusing to overwrite an existing stage: $STAGE; choose WEBRTC_STAGE" >&2
  exit 1
fi
mkdir -p "$PREFIX" "$WORK/bin" "$DOWNLOADS"
mkdir -p "$STAGE"
node "$PROJECT_ROOT/scripts/native_source_inputs.mjs" record "$STAGE/.source-inputs.json"
if [ -f "$WPE_SDK_ROOT/media-build-receipt.json" ]; then
  node "$PROJECT_ROOT/scripts/sdk_receipt.mjs" check
elif [ -f "$PREFIX/lib/libcrypto.so.3" ]; then
  echo 'Refusing unverified pre-existing media binaries; choose a fresh SDK prefix and rebuild' >&2
  exit 1
fi

PROJECT_ROOT="$PROJECT_ROOT" WEBKIT_BASE="$WEBKIT_BASE" \
  WEBKIT_WORKTREE="$WEBKIT_WORKTREE" \
  "$PROJECT_ROOT/scripts/prepare_webkit_worktree.sh"

source "$PROJECT_ROOT/scripts/build_media_runtime.sh"

existing_cache_flag() {
  local cache="$REFERENCE_CACHE"
  [ -f "$cache" ] || {
    echo "Missing validated WebKit CMake cache: $cache" >&2
    return 1
  }
  sed -n "s/^$1:[^=]*=//p" "$cache" | head -n 1
}
EXE_FLAGS="-L$DEVICE_CXX -L$PREFIX/lib -L$FULL_PREFIX/lib -L$FULL_PREFIX/lib/aarch64-linux-gnu -Wl,-rpath-link,$DEVICE_CXX -Wl,-rpath-link,$PREFIX/lib -Wl,-rpath-link,$FULL_PREFIX/lib -Wl,-rpath-link,$FULL_PREFIX/lib/aarch64-linux-gnu -latomic -lm -lpthread -fuse-ld=lld-18 -flto=thin -Wl,--thinlto-jobs=$THINLTO_JOBS"
ABI_OVERLAY="$WPE_SDK_ROOT/abi-overlay"
EXE_FLAGS="-L$ABI_OVERLAY/lib -Wl,-rpath-link,$ABI_OVERLAY/lib $EXE_FLAGS"
SHARED_FLAGS="$EXE_FLAGS"
C_FLAGS="--gcc-toolchain=$BR_TOOLCHAIN -B$BR_TOOLCHAIN/bin -B$BR_TOOLCHAIN/aarch64-buildroot-linux-gnu/bin -mcpu=cortex-a53 -fomit-frame-pointer -flto=thin"
CXX_FLAGS="$C_FLAGS -nostdinc++ -D__cpp_concepts=202002 -Wno-macro-redefined -Wno-builtin-macro-redefined -isystem $FULL_PREFIX/include/c++/12 -isystem $FULL_PREFIX/include/aarch64-linux-gnu/c++/12 -isystem $FULL_PREFIX/include/c++/12/backward -DHAVE_MISSING_STD_FILESYSTEM_PATH_CONSTRUCTOR=1"
case "$EXE_FLAGS" in *"$PREFIX/lib"*) ;; *) EXE_FLAGS="$EXE_FLAGS -L$PREFIX/lib -Wl,-rpath-link,$PREFIX/lib" ;; esac
case "$SHARED_FLAGS" in *"$PREFIX/lib"*) ;; *) SHARED_FLAGS="$SHARED_FLAGS -L$PREFIX/lib -Wl,-rpath-link,$PREFIX/lib" ;; esac
case "$C_FLAGS" in *"-I$PREFIX/include"*) ;; *) C_FLAGS="-I$PREFIX/include $C_FLAGS" ;; esac
case "$CXX_FLAGS" in *"-I$PREFIX/include"*) ;; *) CXX_FLAGS="-I$PREFIX/include $CXX_FLAGS" ;; esac

CMAKE_FEATURE_ARGS=()
while IFS= read -r cache_entry; do
  name="${cache_entry%%:*}"
  value="${cache_entry#*=}"
  CMAKE_FEATURE_ARGS+=("-D$name=$value")
done < <(grep -E '^(ENABLE_|USE_)[A-Za-z0-9_]*:(BOOL|STRING)=' "$REFERENCE_CACHE")

if [ "${SKIP_WEBKIT_BUILD:-0}" != 1 ]; then
  llvm-profdata-18 show "$PGO_PROFILE_PATH" >/dev/null
  PKG_CONFIG_PATH="$PKG_CONFIG_PATH" cmake -S "$WEBKIT_SOURCE" -B "$WEBKIT_BUILD" -GNinja \
    -DPORT=WPE \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$(command -v "$WPE_CLANG")" \
    -DCMAKE_CXX_COMPILER="$(command -v "$WPE_CLANGXX")" \
    -DCMAKE_C_COMPILER_TARGET=aarch64-buildroot-linux-gnu \
    -DCMAKE_CXX_COMPILER_TARGET=aarch64-buildroot-linux-gnu \
    -DCMAKE_SYSROOT="$DEB_SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX;$ABI_OVERLAY;$FULL_PREFIX;$DEB_SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DPERL_EXECUTABLE="$(command -v perl)" \
    -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" \
    -DPython3_EXECUTABLE="$(command -v python3)" \
    -DRuby_EXECUTABLE="$(command -v ruby)" \
    -DICU_I18N_LIBRARY_RELEASE="$ABI_OVERLAY/lib/libicui18n.so.72" \
    -DICU_UC_LIBRARY_RELEASE="$ABI_OVERLAY/lib/libicuuc.so.72" \
    -DICU_DATA_LIBRARY_RELEASE="$ABI_OVERLAY/lib/libicudata.so.72" \
    -DJPEG_LIBRARY_RELEASE="$ABI_OVERLAY/lib/libjpeg.so" \
    -DJPEG_INCLUDE_DIR="$ABI_OVERLAY/include" \
    -DGLIB_COMPILE_RESOURCES_EXECUTABLE=/usr/bin/glib-compile-resources \
    "${CMAKE_FEATURE_ARGS[@]}" \
    -DPGO_PROFILE_PATH="$PGO_PROFILE_PATH" \
    -DCMAKE_JOB_POOLS="link_pool=$LINK_JOBS" \
    -DCMAKE_JOB_POOL_LINK=link_pool \
    -DENABLE_WEB_RTC=ON \
    -DUSE_GSTREAMER_WEBRTC=ON \
    -DENABLE_MEDIA_STREAM=ON \
    -DENABLE_VIDEO=ON \
    -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DUSE_LIBRICE=OFF \
    -DENABLE_GPU_PROCESS=OFF \
    -DENABLE_WEBGL=ON \
    -DUSE_AVIF=ON \
    -DUSE_JPEGXL=OFF \
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
  "$PROJECT_ROOT/wpe-drm/wpe-drm-minimal.c" "$PROJECT_ROOT/wpe-drm/browser-chrome-model.c" \
  "$PROJECT_ROOT/wpe-drm/browser-chrome-publisher.c" \
  "$PROJECT_ROOT/wpe-drm/browser-i18n.c" \
  "$PROJECT_ROOT/wpe-drm/browser-memory-policy.c" \
  "$PROJECT_ROOT/wpe-drm/browser-navigation.c" \
  "$PROJECT_ROOT/wpe-drm/browser-touch-gesture.c" \
  "$PROJECT_ROOT/wpe-drm/browser-profile-store.c" \
  -o "$WPE_ROOT/wpe-drm-minimal" \
  -L"$WEBKIT_BUILD/lib" -L"$FULL_PREFIX/lib" -L"$PREFIX/lib" \
  -L"$DEVICE_CXX" -Wl,-rpath-link,"$WEBKIT_BUILD/lib" \
  -Wl,-rpath-link,"$FULL_PREFIX/lib" -Wl,-rpath-link,"$PREFIX/lib" \
  -lWPEWebKit-2.0 $COMMON_LIBS -lm -ldl -lpthread -latomic -fuse-ld=lld-18

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
copy_family 'libdav1d.so*'
copy_family 'libavif.so*'

if [ -d "$PREFIX/lib/ossl-modules" ]; then
  cp -a "$PREFIX/lib/ossl-modules/." "$STAGE/lib/ossl-modules/"
fi
for plugin in libgstnice.so libgstrtp.so libgstrtpmanager.so libgstinterleave.so \
  libgstdeinterlace.so libgstsrtp.so \
  libgstdtls.so libgstsctp.so libgstwebrtc.so; do
  test -f "$PREFIX/lib/gstreamer-1.0/$plugin"
  cp -a "$PREFIX/lib/gstreamer-1.0/$plugin" "$STAGE/lib/gstreamer-1.0/"
done

cp -a "$WEBKIT_BUILD/lib/libWPEWebKit-2.0.so.1.10.2" \
  "$STAGE/lib/libWPEWebKit-2.0.so.1"
cp -a "$WEBKIT_BUILD/bin/WPEWebProcess" "$STAGE/libexec/wpe-webkit-2.0/"
cp -a "$WEBKIT_BUILD/bin/WPENetworkProcess" "$STAGE/libexec/wpe-webkit-2.0/"
cp -a "$WPE_ROOT/wpe-drm-minimal" "$STAGE/"

PROJECT_ROOT="$PROJECT_ROOT" WEBRTC_STAGE="$STAGE" \
  "$PROJECT_ROOT/scripts/generate_build_manifest.sh"

find "$STAGE" -type f -exec chmod go-w {} +
file "$STAGE/lib/libWPEWebKit-2.0.so.1" "$STAGE/wpe-drm-minimal"
echo "WebRTC runtime stage: $STAGE"
