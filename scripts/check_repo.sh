#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

fail() {
    echo "check_repo: $*" >&2
    exit 1
}

require_file() {
    [ -f "$1" ] || fail "required file missing: $1"
}

reject_lfs_pointer() {
    if head -n 1 "$1" | grep -q 'git-lfs.github.com/spec'; then
        fail "LFS object is not materialized: $1"
    fi
}

dynamic_info() {
    if command -v readelf >/dev/null 2>&1; then
        readelf -d "$1"
    elif command -v objdump >/dev/null 2>&1; then
        objdump -p "$1"
    else
        fail 'readelf or objdump is required for ELF dependency checks'
    fi
}

echo '[1/8] JavaScript fixtures'
node scripts/test_keyboard_event.mjs
node scripts/test_keyboard_trigger.mjs
node scripts/test_display_resolver.mjs
node scripts/test_browser_lifecycle.mjs
node scripts/test_navigation.mjs
c++ -std=c++11 -Wall -Wextra -Werror \
    -Ijsapi/src/jsapi_browser \
    jsapi/src/jsapi_browser/BrowserFilesystem.cpp \
    jsapi/tests/browser-filesystem-test.cpp \
    -o /tmp/wpe-browser-filesystem-test
/tmp/wpe-browser-filesystem-test
c++ -std=c++11 -Wall -Wextra -Werror \
    -Ijsapi/src/jsapi_browser \
    jsapi/src/jsapi_browser/BrowserProcessIdentity.cpp \
    jsapi/tests/browser-process-identity-test.cpp \
    -o /tmp/wpe-browser-process-identity-test
/tmp/wpe-browser-process-identity-test
c++ -std=c++11 -Wall -Wextra -Werror \
    -Iwpe-drm wpe-drm/tests/video-release-queue-test.cpp \
    -o /tmp/wpe-video-release-queue-test
/tmp/wpe-video-release-queue-test
c++ -std=c++11 -Wall -Wextra -Werror \
    -Iwpe-drm wpe-drm/tests/gpu-frame-validity-test.cpp \
    -o /tmp/wpe-gpu-frame-validity-test
/tmp/wpe-gpu-frame-validity-test

echo '[2/8] Shell syntax'
for script in wpe-drm/run.sh wpe-drm/runtime/*.sh scripts/*.sh; do
    case "$(head -n 1 "$script")" in
        *bash*) bash -n "$script" ;;
        *) sh -n "$script" ;;
    esac
done
sh scripts/test_memory_runtime.sh
while IFS= read -r patch; do
    case "$patch" in ''|'#'*) continue ;; esac
    require_file "wpe-drm/webkit-patches/$patch"
    git apply --numstat "wpe-drm/webkit-patches/$patch" >/dev/null \
        || fail "invalid WebKit patch syntax: $patch"
done < wpe-drm/webkit-patches/series

echo '[3/8] Chrome model test'
if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists glib-2.0; then
    fail 'glib-2.0 development metadata is required for chrome model tests'
fi
cc -std=c11 -Wall -Wextra -Werror \
    $(pkg-config --cflags glib-2.0) \
    wpe-drm/browser-chrome-model.c wpe-drm/tests/browser-chrome-model-test.c \
    $(pkg-config --libs glib-2.0) \
    -o /tmp/wpe-browser-chrome-model-test
/tmp/wpe-browser-chrome-model-test
cc -std=c11 -Wall -Wextra -Werror \
    $(pkg-config --cflags glib-2.0) \
    wpe-drm/browser-i18n.c wpe-drm/tests/browser-i18n-test.c \
    $(pkg-config --libs glib-2.0) \
    -o /tmp/wpe-browser-i18n-test
/tmp/wpe-browser-i18n-test
cc -std=c11 -Wall -Wextra -Werror \
    $(pkg-config --cflags glib-2.0) \
    wpe-drm/browser-navigation.c wpe-drm/tests/browser-navigation-test.c \
    $(pkg-config --libs glib-2.0) \
    -o /tmp/wpe-browser-navigation-test
/tmp/wpe-browser-navigation-test
cc -std=c11 -Wall -Wextra -Werror \
    $(pkg-config --cflags glib-2.0) \
    wpe-drm/browser-touch-gesture.c wpe-drm/tests/browser-touch-gesture-test.c \
    $(pkg-config --libs glib-2.0) -lm \
    -o /tmp/wpe-browser-touch-gesture-test
/tmp/wpe-browser-touch-gesture-test
cc -std=c11 -Wall -Wextra -Werror \
    $(pkg-config --cflags glib-2.0) \
    wpe-drm/browser-memory-policy.c wpe-drm/tests/browser-memory-policy-test.c \
    $(pkg-config --libs glib-2.0) \
    -o /tmp/wpe-browser-memory-policy-test
/tmp/wpe-browser-memory-policy-test

echo '[4/8] Profile database test'
if command -v pkg-config >/dev/null 2>&1 \
    && pkg-config --exists glib-2.0 sqlite3; then
    cc -std=c11 -Wall -Wextra -Werror \
        $(pkg-config --cflags glib-2.0 sqlite3) \
        wpe-drm/browser-profile-store.c wpe-drm/tests/browser-profile-store-test.c \
        $(pkg-config --libs glib-2.0 sqlite3) \
        -o /tmp/wpe-browser-profile-store-test
    /tmp/wpe-browser-profile-store-test
else
    fail 'glib-2.0/sqlite3 development metadata is required for profile tests'
fi

echo '[5/8] Runtime completeness'
for path in \
    assets/wpe-runtime/wpe-drm-minimal \
    assets/wpe-runtime/build-manifest.json \
    assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1 \
    assets/wpe-runtime/lib/librga.so.2 \
    assets/wpe-runtime/lib/libavif.so.16 \
    assets/wpe-runtime/lib/libdav1d.so.7 \
    assets/wpe-runtime/lib/gstreamer-1.0/libgstinterleave.so \
    assets/wpe-runtime/lib/gstreamer-1.0/libgstdeinterlace.so \
    assets/wpe-runtime/lib/gstreamer-1.0/libgstalsa.so \
    assets/wpe-runtime/lib/gstreamer-1.0/libgstvolume.so \
    assets/wpe-runtime/lib/libasound.so.2 \
    assets/wpe-runtime/etc/ssl/certs/ca-certificates.crt \
    assets/wpe-runtime/libexec/wpe-webkit-2.0/WPEWebProcess \
    assets/wpe-runtime/libexec/wpe-webkit-2.0/WPENetworkProcess \
    assets/wpe-runtime/assets/fonts/miniapp/NotoSansSC-Regular.otf \
    libs/arm64-orange/libjsapi_browser.so; do
    require_file "$path"
    reject_lfs_pointer "$path"
done
dynamic_info assets/wpe-runtime/lib/libavif.so.16 \
    | grep -q 'NEEDED.*libdav1d.so.7' \
    || fail 'packaged AVIF decoder is not linked to the bundled dav1d runtime'
sh scripts/validate_gpu_runtime.sh optional
webkit_entries=$(find assets/wpe-runtime/lib -maxdepth 1 \
    -name 'libWPEWebKit-2.0.so*' -print)
[ "$webkit_entries" = "assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1" ] \
    && [ ! -L assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1 ] \
    || fail 'runtime must contain exactly one regular libWPEWebKit-2.0.so.1'
dynamic_info assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1 \
    | grep -q 'SONAME.*libWPEWebKit-2.0.so.1' \
    || fail 'WebKit ELF SONAME does not match the canonical package name'
[ ! -L assets/wpe-runtime/lib/librga.so.2 ] \
    || fail 'librga.so.2 must be the single regular packaged RGA library'
dynamic_info assets/wpe-runtime/lib/librga.so.2 \
    | grep -q 'SONAME.*librga.so.2' \
    || fail 'RGA ELF SONAME does not match the canonical package name'
for consumer in \
    assets/wpe-runtime/wpe-drm-minimal \
    assets/wpe-runtime/libexec/wpe-webkit-2.0/WPEWebProcess \
    assets/wpe-runtime/libexec/wpe-webkit-2.0/WPENetworkProcess; do
    dynamic_info "$consumer" | grep -q 'NEEDED.*libWPEWebKit-2.0.so.1' \
        || fail "$consumer does not depend on libWPEWebKit-2.0.so.1"
done
if [ -f 8001779591038449.1_0_0.amr ]; then
    sh scripts/verify_amr_runtime.sh
fi
node scripts/verify_build_manifests.mjs
c++ -std=c++11 -Wall -Wextra -Werror -c \
    jsapi/src/jsapi_browser/BrowserDisplayConfig.cpp \
    -o /tmp/wpe-browser-display-config.o

echo '[6/8] Generated-source policy'
[ ! -e assets/wpe-runtime/run.sh ] \
    || cmp -s wpe-drm/run.sh assets/wpe-runtime/run.sh \
    || fail 'assets/wpe-runtime/run.sh differs from wpe-drm/run.sh'
if [ -d assets/wpe-runtime/runtime ]; then
    for module in wpe-drm/runtime/*.sh; do
        generated="assets/wpe-runtime/runtime/$(basename "$module")"
        cmp -s "$module" "$generated" \
            || fail "generated runtime module differs from source: $module"
    done
fi

echo '[7/8] External fallback policy'
if rg -n '/userdisk/(wpe-drm2|wpe-cairo|mesa)' \
    src jsapi wpe-drm scripts --glob '!**/*.md'; then
    fail 'production source contains an external /userdisk runtime fallback'
fi
EVENT5_PATH='/dev/input/event''5'
if rg -n "$EVENT5_PATH" src jsapi wpe-drm scripts --glob '!**/*.md'; then
    fail 'production source contains the removed event5 touch fallback'
fi
if rg -n '/dev/input/by-path/hyn_ts' wpe-drm/run.sh; then
    fail 'production supervisor contains a hard-coded touch device fallback'
fi
if rg -n 'webkit_network_session_allow_tls_certificate_for_host' wpe-drm/wpe-drm-minimal.c; then
    fail 'launcher must not bypass TLS validation when the bundled CA exists'
fi
if ! rg -q 'Web process memory recovery stopped:.*auto_reload=0' \
    wpe-drm/wpe-drm-minimal.c; then
    fail 'memory-limit WebProcess termination must not auto-reload the failed page'
fi
if rg -n 'reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT.*schedule_web_process_reload' \
    wpe-drm/wpe-drm-minimal.c; then
    fail 'memory-limit recovery must require an explicit Retry/Home action'
fi
if rg -n '\.detach\(\)' jsapi/src/jsapi_browser --glob '*.{cpp,h}'; then
    fail 'JSAPI must not leave detached threads running across native module teardown'
fi
if rg -n '/home/parallels|qemu-x86_64|10\.211\.55\.4' scripts \
    --glob '*.sh' --glob '!check_repo.sh'; then
    fail 'build scripts contain a deprecated Parallels builder path'
fi

echo '[8/8] Patch series metadata'
require_file wpe-drm/webkit-patches/series
require_file wpe-drm/webkit-patches/revision
require_file wpe-drm/webkit-patches/inputs
while IFS= read -r patch; do
    case "$patch" in ''|'#'*) continue ;; esac
    require_file "wpe-drm/webkit-patches/$patch"
done < wpe-drm/webkit-patches/series
while IFS= read -r input; do
    case "$input" in ''|'#'*) continue ;; esac
    require_file "$input"
done < wpe-drm/webkit-patches/inputs

echo 'check_repo: all checks passed'
