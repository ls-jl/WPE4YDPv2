#!/bin/sh
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VAR_DIR="${WPE_VAR_DIR:-$DIR/var}"
mkdir -p "$VAR_DIR"
MESA="${WPE_MESA_DIR:-$DIR}"
if [ ! -d "$MESA/lib" ]; then
    echo "WPE fatal: Mesa/runtime lib dir missing: $MESA/lib"
    exit 84
fi
unset WPE_BACKEND_LIBRARY
export LD_LIBRARY_PATH="$MESA/lib:$DIR/lib"
export LIBGL_DRIVERS_PATH="$MESA/lib/dri"
export GBM_BACKENDS_PATH="$MESA/lib/gbm"
export GBM_BACKEND="${GBM_BACKEND:-drm}"
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-softpipe}"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-kms_swrast}"
GST_PRIVATE_PLUGIN_DIR="$DIR/lib/gstreamer-1.0"
GST_VPU_PLUGIN_DIR="${WPE_VPU_PLUGIN_DIR:-/usr/lib/gstreamer-1.0}"
if [ -f "$GST_VPU_PLUGIN_DIR/libgstrockchipmpp.so" ]; then
    export GST_PLUGIN_PATH="$GST_VPU_PLUGIN_DIR:$GST_PRIVATE_PLUGIN_DIR"
    export GST_PLUGIN_SYSTEM_PATH="$GST_VPU_PLUGIN_DIR:$GST_PRIVATE_PLUGIN_DIR"
else
    export GST_PLUGIN_PATH="$GST_PRIVATE_PLUGIN_DIR"
    export GST_PLUGIN_SYSTEM_PATH="$GST_PRIVATE_PLUGIN_DIR"
fi
export GIO_MODULE_DIR="$DIR/lib/gio/modules"
export GIO_EXTRA_MODULES="$DIR/lib/gio/modules"
export G_TLS_CA_FILE="${G_TLS_CA_FILE:-$DIR/etc/ssl/certs/ca-certificates.crt}"
export SSL_CERT_FILE="${SSL_CERT_FILE:-$DIR/etc/ssl/certs/ca-certificates.crt}"
export WEBKIT_TLS_CAFILE_PEM="${WEBKIT_TLS_CAFILE_PEM:-$G_TLS_CA_FILE}"
if [ ! -f "$G_TLS_CA_FILE" ]; then
    echo "WPE fatal: CA certificates missing: $G_TLS_CA_FILE"
    exit 85
fi
export TZDIR="${TZDIR:-$DIR/share/zoneinfo}"
export TZ="${TZ:-Asia/Shanghai}"
export GST_PLUGIN_SCANNER="$DIR/libexec/gstreamer-1.0/gst-plugin-scanner"
export GST_REGISTRY="${GST_REGISTRY:-$VAR_DIR/gst-registry.bin}"
export GST_DEBUG="${GST_DEBUG:-2,decodebin:4,uridecodebin:4,webkit*:3}"
export WEBKIT_GST_DISABLE_GL_SINK="${WEBKIT_GST_DISABLE_GL_SINK:-1}"
export WEBKIT_WEBGL_DISABLE_GBM="${WEBKIT_WEBGL_DISABLE_GBM:-1}"
export WEBKIT_DISABLE_DMABUF_ATLAS="${WEBKIT_DISABLE_DMABUF_ATLAS:-1}"
export WEBKIT_FORCE_VBLANK_TIMER="${WEBKIT_FORCE_VBLANK_TIMER:-1}"
export WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR="${WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR:-1}"
export WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS="${WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS:-30}"
export WPE_DRM_MAX_FPS="${WPE_DRM_MAX_FPS:-0}"
export WPE_CHROME_ENABLED="${WPE_CHROME_ENABLED:-1}"
export WPE_CHROME_HEIGHT="${WPE_CHROME_HEIGHT:-44}"
export WPE_CHROME_TOUCH_HEIGHT="${WPE_CHROME_TOUCH_HEIGHT:-52}"
export WPE_CHROME_HIT_SLOP="${WPE_CHROME_HIT_SLOP:-6}"
export WPE_CHROME_REVEAL_HEIGHT="${WPE_CHROME_REVEAL_HEIGHT:-22}"
export WPE_CHROME_TAP_MAX_MOVE="${WPE_CHROME_TAP_MAX_MOVE:-32}"
export WPE_CHROME_ANIMATION_MS="${WPE_CHROME_ANIMATION_MS:-160}"
export WPE_CHROME_HIDE_DOWN_PX="${WPE_CHROME_HIDE_DOWN_PX:-96}"
export WPE_CHROME_SHOW_UP_PX="${WPE_CHROME_SHOW_UP_PX:-180}"
export WPE_CHROME_SHOW_UP_MIN_VELOCITY="${WPE_CHROME_SHOW_UP_MIN_VELOCITY:-700}"
export WPE_CHROME_LAYOUT="${WPE_CHROME_LAYOUT:-resize}"
export WPE_CHROME_STATE="${WPE_CHROME_STATE:-$VAR_DIR/browser-state.ini}"
export WPE_CHROME_RENDER_STATE="${WPE_CHROME_RENDER_STATE:-$VAR_DIR/chrome-render-state.ini}"
export WPE_PANEL_SIZE="${WPE_PANEL_SIZE:-${WPE_VIEWPORT:-960x266}}"
export WPE_VIEWPORT="${WPE_VIEWPORT:-$WPE_PANEL_SIZE}"
export WPE_DRM_MODE="${WPE_DRM_MODE:-$WPE_PANEL_SIZE}"
export WPE_DRM_FIT="${WPE_DRM_FIT:-panel-native}"
REQUESTED_ROTATION="${4:-${WPE_DRM_ROTATION:-${WPE_PANEL_ROTATION:-0}}}"
export WPE_PANEL_ROTATION="${WPE_PANEL_ROTATION:-$REQUESTED_ROTATION}"
export WPE_TOUCH_ROTATION="${WPE_TOUCH_ROTATION:-$WPE_PANEL_ROTATION}"
PANEL_W="${WPE_PANEL_SIZE%x*}"
PANEL_H="${WPE_PANEL_SIZE#*x}"
DRM_W="${WPE_DRM_MODE%x*}"
case "$WPE_PANEL_ROTATION" in
    90|270) ROTATED_PANEL_W="$PANEL_H" ;;
    *) ROTATED_PANEL_W="$PANEL_W" ;;
esac
case "$DRM_W:$ROTATED_PANEL_W" in
    *[!0-9:]*|:|*:)
        WPE_PANEL_CRTC_X="${WPE_DRM_ROTATED_X:-0}"
        ;;
    *)
        if [ -n "$WPE_DRM_ROTATED_X" ]; then
            WPE_PANEL_CRTC_X="$WPE_DRM_ROTATED_X"
        else
            WPE_PANEL_CRTC_X=$(((DRM_W - ROTATED_PANEL_W) / 2))
            [ "$WPE_PANEL_CRTC_X" -lt 0 ] && WPE_PANEL_CRTC_X=0
        fi
        ;;
esac
export WPE_PANEL_CRTC_X
export WPE_TOUCH_ACTIVE_X="${WPE_TOUCH_ACTIVE_X:-$WPE_PANEL_CRTC_X..$((WPE_PANEL_CRTC_X + ROTATED_PANEL_W))}"
FONTCONFIG_CACHE_DIR="${FONTCONFIG_CACHE_DIR:-$VAR_DIR/fontconfig-cache}"
FONTCONFIG_RUNTIME_FILE="${FONTCONFIG_RUNTIME_FILE:-$VAR_DIR/fonts.conf}"
BUNDLED_FONT_ROOT="${WPE_BUNDLED_FONT_ROOT:-$DIR/assets/fonts}"
FONT_DIRS_FILE="$VAR_DIR/font-dirs.txt"
FONT_DIRS_TMP="$VAR_DIR/font-dirs.tmp"
MINIAPP_FONT_DIR="$BUNDLED_FONT_ROOT/miniapp"
: >"$FONT_DIRS_TMP"
if [ -d "$MINIAPP_FONT_DIR" ]; then
    find "$MINIAPP_FONT_DIR" -type f \( -name '*.ttf' -o -name '*.otf' -o -name '*.ttc' -o -name '*.pcf' -o -name '*.pcf.gz' -o -name '*.woff' -o -name '*.woff2' \) -exec dirname {} \; 2>/dev/null | sort -u >>"$FONT_DIRS_TMP"
fi
find "$BUNDLED_FONT_ROOT" -type f \( -name '*.ttf' -o -name '*.otf' -o -name '*.ttc' -o -name '*.pcf' -o -name '*.pcf.gz' -o -name '*.woff' -o -name '*.woff2' \) -exec dirname {} \; 2>/dev/null | sort -u >>"$FONT_DIRS_TMP"
awk '!seen[$0]++' "$FONT_DIRS_TMP" >"$FONT_DIRS_FILE"
rm -f "$FONT_DIRS_TMP"
if [ ! -s "$FONT_DIRS_FILE" ]; then
    echo "WPE fatal: bundled fonts missing dir=$BUNDLED_FONT_ROOT"
    exit 86
fi
CJK_FONT="$MINIAPP_FONT_DIR/NotoSansSC-Regular.otf"
if [ -f "$CJK_FONT" ]; then
    echo "WPE fonts: root=$BUNDLED_FONT_ROOT cjk=$CJK_FONT"
else
    echo "WPE warning: CJK font missing: $CJK_FONT"
fi
mkdir -p "$FONTCONFIG_CACHE_DIR"
{
    echo '<?xml version="1.0"?>'
    echo '<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">'
    echo '<fontconfig>'
    while IFS= read -r font_dir; do
        [ -n "$font_dir" ] && echo "  <dir>$font_dir</dir>"
    done <"$FONT_DIRS_FILE"
    echo "  <cachedir>$FONTCONFIG_CACHE_DIR</cachedir>"
    echo '  <match target="pattern">'
    echo '    <test qual="any" name="family"><string>serif</string></test>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Serif</string></edit>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>Noto Sans SC</string></edit>'
    echo '  </match>'
    echo '  <match target="pattern">'
    echo '    <test qual="any" name="family"><string>sans-serif</string></test>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans</string></edit>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>HarmonyOS Sans SC</string></edit>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>Noto Sans SC</string></edit>'
    echo '  </match>'
    echo '  <match target="pattern">'
    echo '    <test qual="any" name="family"><string>monospace</string></test>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans Mono</string></edit>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>Noto Sans SC</string></edit>'
    echo '  </match>'
    echo '</fontconfig>'
} >"$FONTCONFIG_RUNTIME_FILE"
export FONTCONFIG_PATH="$(dirname "$FONTCONFIG_RUNTIME_FILE")"
export FONTCONFIG_FILE="$FONTCONFIG_RUNTIME_FILE"
unset FONTCONFIG_SYSROOT
export GSETTINGS_SCHEMA_DIR="$DIR/share/glib-2.0/schemas"
export XKB_CONFIG_ROOT="$DIR/share/X11/xkb"
export LIBINPUT_QUIRKS_DIR="$DIR/share/libinput"
export WEBKIT_EXEC_PATH="$DIR/libexec/wpe-webkit-2.0"
export WEBKIT_INJECTED_BUNDLE_PATH="$DIR/lib/wpe-webkit-2.0/injected-bundle"
# The device has no usable GPU, but dma-heap direct scanout is still much
# faster than SHM dumb-buffer copies. Set WPE_DRM_FORCE_SHM=1 only for fallback
# diagnostics on kernels without a working dma-heap scanout path.
export WPE_DRM_FORCE_SHM="${WPE_DRM_FORCE_SHM:-0}"
export WPE_DRM_BUFFER_PATH="${WPE_DRM_BUFFER_PATH:-dma_heap}"
export WPE_DRM_RUNTIME_DIR="${WPE_DRM_RUNTIME_DIR:-$VAR_DIR/runtime}"
export WPE_KEYBOARD_DIR="${WPE_KEYBOARD_DIR:-$VAR_DIR/keyboard}"
export WPE_RAW_TOUCH="${WPE_RAW_TOUCH:-1}"
export WPE_TOUCH_DEVICE="${WPE_TOUCH_DEVICE:-/dev/input/by-path/hyn_ts}"
export WPE_TOUCH_OFFSET_X="${WPE_TOUCH_OFFSET_X:-0}"
export WPE_TOUCH_OFFSET_Y="${WPE_TOUCH_OFFSET_Y:-0}"
export WPE_SEND_TOUCH_EVENTS="${WPE_SEND_TOUCH_EVENTS:-0}"
export WPE_SYNTHESIZE_POINTER_TAP="${WPE_SYNTHESIZE_POINTER_TAP:-1}"
export WPE_TOUCH_SCROLL_FALLBACK="${WPE_TOUCH_SCROLL_FALLBACK:-1}"
export WPE_TOUCH_NATIVE_SCROLL="${WPE_TOUCH_NATIVE_SCROLL:-1}"
export WPE_TOUCH_JS_SCROLL="${WPE_TOUCH_JS_SCROLL:-0}"
export WPE_TOUCH_SCROLL_INVERT_Y="${WPE_TOUCH_SCROLL_INVERT_Y:-0}"
export WPE_TOUCH_SCROLL_SCALE="${WPE_TOUCH_SCROLL_SCALE:-1.0}"
export WPE_TOUCH_SCROLL_MAX_STEP="${WPE_TOUCH_SCROLL_MAX_STEP:-32}"
export WPE_TOUCH_SCROLL_PENDING_LIMIT="${WPE_TOUCH_SCROLL_PENDING_LIMIT:-64}"
export WPE_TOUCH_TAP_MAX_MOVE="${WPE_TOUCH_TAP_MAX_MOVE:-32}"
export WPE_TOUCH_SCROLL_INTERVAL_MS="${WPE_TOUCH_SCROLL_INTERVAL_MS:-16}"
export WPE_TOUCH_SCROLL_STOP_DELAY_MS="${WPE_TOUCH_SCROLL_STOP_DELAY_MS:-80}"
export WPE_DEFAULT_URL="${WPE_DEFAULT_URL:-https://m.baidu.com/}"
URL="${1:-$WPE_DEFAULT_URL}"
DRM="${2:-/dev/dri/card0}"
VIEWPORT="${3:-$WPE_VIEWPORT}"
ROTATION="${4:-${WPE_DRM_ROTATION:-${WPE_PANEL_ROTATION:-0}}}"
export WPE_VIEWPORT="$VIEWPORT"
export WPE_DRM_VIEWPORT="$VIEWPORT"
export WPE_DRM_ROTATION="$ROTATION"
export WPE_PANEL_ROTATION="${WPE_PANEL_ROTATION:-$ROTATION}"
export WPE_TOUCH_ROTATION="${WPE_TOUCH_ROTATION:-$WPE_PANEL_ROTATION}"
export WPE_DRM_ROTATION_FILE="${WPE_DRM_ROTATION_FILE:-$VAR_DIR/rotation}"
if [ -z "$WPE_DRM_DMA_HEAP" ]; then
    case "$ROTATION" in
        0|360|-0) WPE_DRM_DMA_HEAP=/dev/dma_heap/system-uncached ;;
        *) WPE_DRM_DMA_HEAP=/dev/dma_heap/system ;;
    esac
fi
export WPE_DRM_DMA_HEAP
printf '%s\n' "$ROTATION" >"$WPE_DRM_ROTATION_FILE" 2>/dev/null || true
echo "WPE launch: url=$URL drm=$DRM panel=$WPE_PANEL_SIZE drm_mode=$WPE_DRM_MODE viewport=$VIEWPORT rotation=$ROTATION panel_rotation=$WPE_PANEL_ROTATION touch_rotation=$WPE_TOUCH_ROTATION touch_device=$WPE_TOUCH_DEVICE touch_offset=$WPE_TOUCH_OFFSET_X,$WPE_TOUCH_OFFSET_Y browser_mode=${WPE_BROWSER_MODE:-unknown} display_source=${WPE_DISPLAY_SOURCE:-unknown} fit=$WPE_DRM_FIT panel_crtc_x=$WPE_PANEL_CRTC_X rotated_x=${WPE_DRM_ROTATED_X:-auto} touch_active_x=$WPE_TOUCH_ACTIVE_X fps=$WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS max_fps=$WPE_DRM_MAX_FPS mem_pressure_monitor=$WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR heap=$WPE_DRM_DMA_HEAP keyboard_dir=$WPE_KEYBOARD_DIR"

KEEP_PID=
if command -v hal-screen >/dev/null 2>&1; then
    env -u LD_LIBRARY_PATH hal-screen on >/dev/null 2>&1 || true
    (
        while :; do
            env -u LD_LIBRARY_PATH hal-screen keep >/dev/null 2>&1 || true
            sleep 20
        done
    ) &
    KEEP_PID=$!
fi

"$DIR/wpe-drm-minimal" "$URL" "$DRM" "$VIEWPORT" "$ROTATION"
STATUS=$?
if [ -n "$KEEP_PID" ]; then
    kill "$KEEP_PID" 2>/dev/null || true
fi
exit "$STATUS"
