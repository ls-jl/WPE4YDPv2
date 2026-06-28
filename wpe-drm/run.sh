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
export WPE_CHROME_SHOW_UP_PX="${WPE_CHROME_SHOW_UP_PX:-72}"
export WPE_CHROME_STATE="${WPE_CHROME_STATE:-$VAR_DIR/browser-state.ini}"
export WPE_CHROME_RENDER_STATE="${WPE_CHROME_RENDER_STATE:-$VAR_DIR/chrome-render-state.ini}"
export WPE_PANEL_SIZE="${WPE_PANEL_SIZE:-960x266}"
export WPE_VIEWPORT="${WPE_VIEWPORT:-$WPE_PANEL_SIZE}"
export WPE_DRM_MODE="${WPE_DRM_MODE:-480x960}"
export WPE_DRM_FIT="${WPE_DRM_FIT:-panel-native}"
export WPE_DRM_ROTATED_X="${WPE_DRM_ROTATED_X:-111}"
export WPE_TOUCH_ACTIVE_X="${WPE_TOUCH_ACTIVE_X:-$WPE_DRM_ROTATED_X..$((WPE_DRM_ROTATED_X + 266))}"
FONTCONFIG_CACHE_DIR="${FONTCONFIG_CACHE_DIR:-$VAR_DIR/fontconfig-cache}"
FONTCONFIG_RUNTIME_FILE="${FONTCONFIG_RUNTIME_FILE:-$VAR_DIR/fonts.conf}"
BUNDLED_FONT_DIR="${WPE_BUNDLED_FONT_DIR:-$DIR/assets/fonts/dejavu}"
if ! find "$BUNDLED_FONT_DIR" -maxdepth 1 \( -name '*.ttf' -o -name '*.otf' -o -name '*.ttc' \) 2>/dev/null | grep -q .; then
    echo "WPE fatal: bundled fonts missing dir=$BUNDLED_FONT_DIR"
    exit 86
fi
mkdir -p "$FONTCONFIG_CACHE_DIR"
{
    echo '<?xml version="1.0"?>'
    echo '<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">'
    echo '<fontconfig>'
    echo "  <dir>$BUNDLED_FONT_DIR</dir>"
    if [ -d "$DIR/assets/fonts/noto" ]; then
        echo "  <dir>$DIR/assets/fonts/noto</dir>"
    fi
    echo "  <cachedir>$FONTCONFIG_CACHE_DIR</cachedir>"
    echo '  <match target="pattern">'
    echo '    <test qual="any" name="family"><string>serif</string></test>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Serif</string></edit>'
    echo '  </match>'
    echo '  <match target="pattern">'
    echo '    <test qual="any" name="family"><string>sans-serif</string></test>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans</string></edit>'
    echo '  </match>'
    echo '  <match target="pattern">'
    echo '    <test qual="any" name="family"><string>monospace</string></test>'
    echo '    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans Mono</string></edit>'
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
export WPE_RAW_TOUCH="${WPE_RAW_TOUCH:-1}"
export WPE_TOUCH_DEVICE="${WPE_TOUCH_DEVICE:-/dev/input/by-path/hyn_ts}"
export WPE_SEND_TOUCH_EVENTS="${WPE_SEND_TOUCH_EVENTS:-0}"
export WPE_SYNTHESIZE_POINTER_TAP="${WPE_SYNTHESIZE_POINTER_TAP:-1}"
export WPE_TOUCH_SCROLL_FALLBACK="${WPE_TOUCH_SCROLL_FALLBACK:-1}"
export WPE_TOUCH_NATIVE_SCROLL="${WPE_TOUCH_NATIVE_SCROLL:-0}"
export WPE_TOUCH_JS_SCROLL="${WPE_TOUCH_JS_SCROLL:-1}"
export WPE_TOUCH_SCROLL_SCALE="${WPE_TOUCH_SCROLL_SCALE:-0.85}"
export WPE_TOUCH_SCROLL_MAX_STEP="${WPE_TOUCH_SCROLL_MAX_STEP:-18}"
export WPE_TOUCH_SCROLL_PENDING_LIMIT="${WPE_TOUCH_SCROLL_PENDING_LIMIT:-24}"
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
export WPE_DRM_ROTATION_FILE="${WPE_DRM_ROTATION_FILE:-$VAR_DIR/rotation}"
if [ -z "$WPE_DRM_DMA_HEAP" ]; then
    case "$ROTATION" in
        0|360|-0) WPE_DRM_DMA_HEAP=/dev/dma_heap/system-uncached ;;
        *) WPE_DRM_DMA_HEAP=/dev/dma_heap/system ;;
    esac
fi
export WPE_DRM_DMA_HEAP
printf '%s\n' "$ROTATION" >"$WPE_DRM_ROTATION_FILE" 2>/dev/null || true
echo "WPE launch: url=$URL drm=$DRM panel=$WPE_PANEL_SIZE drm_mode=$WPE_DRM_MODE viewport=$VIEWPORT rotation=$ROTATION panel_rotation=$WPE_PANEL_ROTATION fit=$WPE_DRM_FIT fps=$WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS mem_pressure_monitor=$WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR heap=$WPE_DRM_DMA_HEAP"

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
