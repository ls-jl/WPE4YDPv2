#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
directory="$WPE_BUILD_ROOT/sanitizers"
mkdir -p "$directory"
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:handle_segv=0}"
# Use non-PIE test executables after observing unstable PIE sanitizer startup.
flags=(-O1 -g -fno-pie -no-pie -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Wextra -Werror)
for name in browser-memory-policy browser-touch-gesture browser-chrome-model browser-chrome-publisher browser-i18n browser-navigation browser-profile-store; do
  packages='glib-2.0'
  if [ "$name" = browser-chrome-publisher ]; then packages='glib-2.0 gobject-2.0'; fi
  if [ "$name" = browser-profile-store ]; then packages="$packages sqlite3"; fi
  cc -std=c11 "${flags[@]}" -I"$PROJECT_ROOT/wpe-drm" $(pkg-config --cflags $packages) \
    "$PROJECT_ROOT/wpe-drm/$name.c" "$PROJECT_ROOT/wpe-drm/tests/$name-test.c" \
    $(pkg-config --libs $packages) -lm -o "$directory/$name"
  (ulimit -f 8192; timeout -k 2 60 "$directory/$name") > "$directory/$name.log" 2>&1 || {
    tail -60 "$directory/$name.log"
    exit 1
  }
  echo "ASan/UBSan passed: $name"
done
cd "$PROJECT_ROOT"
WPE_TEST_SANITIZERS=1 node scripts/test_gpu_tile_protocol.mjs
WPE_TEST_SANITIZERS=1 node scripts/test_evdev_protocol.mjs
for name in video-release-queue video-wire gpu-frame-validity rendering-fence native-scroll-state drm-ownership tile-prefetch-budget; do
  c++ -std=c++11 "${flags[@]}" -I"$PROJECT_ROOT/wpe-drm" \
    $(pkg-config --cflags glib-2.0) "$PROJECT_ROOT/wpe-drm/tests/$name-test.cpp" \
    $(pkg-config --libs glib-2.0) -o "$directory/$name"
  (ulimit -f 8192; timeout -k 2 60 "$directory/$name") > "$directory/$name.log" 2>&1 || {
    tail -60 "$directory/$name.log"
    exit 1
  }
  echo "ASan/UBSan passed: $name"
done
