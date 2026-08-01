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
node scripts/test_display_resolver.mjs

echo '[2/8] Shell syntax'
for script in wpe-drm/run.sh wpe-drm/runtime/*.sh scripts/*.sh; do
    case "$(head -n 1 "$script")" in
        *bash*) bash -n "$script" ;;
        *) sh -n "$script" ;;
    esac
done

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

echo '[4/8] Profile database test'
if command -v pkg-config >/dev/null 2>&1 \
    && pkg-config --exists glib-2.0 sqlite3; then
    cc -std=c11 -Wall -Wextra \
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
    assets/wpe-runtime/etc/ssl/certs/ca-certificates.crt \
    assets/wpe-runtime/libexec/wpe-webkit-2.0/WPEWebProcess \
    assets/wpe-runtime/libexec/wpe-webkit-2.0/WPENetworkProcess \
    assets/wpe-runtime/assets/fonts/miniapp/NotoSansSC-Regular.otf \
    libs/arm64-orange/libjsapi_browser.so; do
    require_file "$path"
    reject_lfs_pointer "$path"
done
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
[ ! -d assets/wpe-runtime/runtime ] \
    || cmp -s wpe-drm/runtime/gpu-runtime.sh assets/wpe-runtime/runtime/gpu-runtime.sh \
    || fail 'generated GPU runtime module differs from source'

echo '[7/8] External fallback policy'
if rg -n '/userdisk/(wpe-drm2|wpe-cairo|mesa)' \
    src jsapi wpe-drm scripts --glob '!**/*.md'; then
    fail 'production source contains an external /userdisk runtime fallback'
fi
EVENT5_PATH='/dev/input/event''5'
if rg -n "$EVENT5_PATH" src jsapi wpe-drm scripts --glob '!**/*.md'; then
    fail 'production source contains the removed event5 touch fallback'
fi

echo '[8/8] Patch series metadata'
require_file wpe-drm/webkit-patches/series
while IFS= read -r patch; do
    case "$patch" in ''|'#'*) continue ;; esac
    require_file "wpe-drm/webkit-patches/$patch"
done < wpe-drm/webkit-patches/series

echo 'check_repo: all checks passed'
