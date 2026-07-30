#!/bin/sh
umask 077
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
GST_SOURCE="bundled"
# 默认只使用包内 GStreamer 插件，保证 AMR 自包含。需要调试系统 VPU/音频插件时，
# 显式设置 WPE_USE_SYSTEM_GST=1，并可用 WPE_SYSTEM_GST_PLUGIN_DIR 覆盖系统目录。
if [ "${WPE_USE_SYSTEM_GST:-0}" = "1" ]; then
    GST_SYSTEM_PLUGIN_DIR="${WPE_SYSTEM_GST_PLUGIN_DIR:-${WPE_VPU_PLUGIN_DIR:-/usr/lib/gstreamer-1.0}}"
else
    GST_SYSTEM_PLUGIN_DIR=""
fi
if [ -n "$GST_SYSTEM_PLUGIN_DIR" ] && [ -d "$GST_SYSTEM_PLUGIN_DIR" ]; then
    export GST_PLUGIN_PATH="$GST_SYSTEM_PLUGIN_DIR:$GST_PRIVATE_PLUGIN_DIR"
    export GST_PLUGIN_SYSTEM_PATH="$GST_SYSTEM_PLUGIN_DIR:$GST_PRIVATE_PLUGIN_DIR"
    GST_SOURCE="system"
else
    export GST_PLUGIN_PATH="$GST_PRIVATE_PLUGIN_DIR"
    export GST_PLUGIN_SYSTEM_PATH="$GST_PRIVATE_PLUGIN_DIR"
fi

# 浏览器音频走系统 ALSA（RK817 codec，/etc/asound.conf 路由到 speaker）。
# 启动前尽力解除静音，失败不阻塞。
if command -v amixer >/dev/null 2>&1; then
    env -u LD_LIBRARY_PATH amixer sset Master unmute >/dev/null 2>&1 || true
    env -u LD_LIBRARY_PATH amixer sset Playback unmute >/dev/null 2>&1 || true
fi
export GIO_MODULE_DIR="$DIR/lib/gio/modules"
export GIO_EXTRA_MODULES="$DIR/lib/gio/modules"
export G_TLS_CA_FILE="${G_TLS_CA_FILE:-$DIR/etc/ssl/certs/ca-certificates.crt}"
export SSL_CERT_FILE="${SSL_CERT_FILE:-$DIR/etc/ssl/certs/ca-certificates.crt}"
export WEBKIT_TLS_CAFILE_PEM="${WEBKIT_TLS_CAFILE_PEM:-$G_TLS_CA_FILE}"
export OPENSSL_MODULES="${OPENSSL_MODULES:-$DIR/lib/ossl-modules}"
if [ ! -f "$G_TLS_CA_FILE" ]; then
    echo "WPE fatal: CA certificates missing: $G_TLS_CA_FILE"
    exit 85
fi
export TZDIR="${TZDIR:-$DIR/share/zoneinfo}"
export TZ="${TZ:-Asia/Shanghai}"
export GST_PLUGIN_SCANNER="$DIR/libexec/gstreamer-1.0/gst-plugin-scanner"
export GST_REGISTRY="${GST_REGISTRY:-$VAR_DIR/gst-registry.bin}"
GST_PLUGIN_SET_FILE="$DIR/share/gstreamer-plugin-set.sha256"
GST_PLUGIN_SET_ACTIVE="$VAR_DIR/gstreamer-plugin-set.sha256"
if [ -s "$GST_PLUGIN_SET_FILE" ]; then
    if ! cmp -s "$GST_PLUGIN_SET_FILE" "$GST_PLUGIN_SET_ACTIVE" 2>/dev/null; then
        rm -f "$GST_REGISTRY"
        cp "$GST_PLUGIN_SET_FILE" "$GST_PLUGIN_SET_ACTIVE"
        chmod 600 "$GST_PLUGIN_SET_ACTIVE" 2>/dev/null || true
        echo "WPE GStreamer: plugin set changed; registry reset"
    fi
fi
export WEBKIT_CLOUD_FORCE_ICE_CONTROLLER="${WEBKIT_CLOUD_FORCE_ICE_CONTROLLER:-1}"
export WPE_CLOUD_DIAGNOSTICS="${WPE_CLOUD_DIAGNOSTICS:-0}"
export WPE_START_URL_OVERRIDE="${WPE_START_URL_OVERRIDE:-0}"
if [ "$WPE_CLOUD_DIAGNOSTICS" = "1" ]; then
    export WEBKIT_CLOUD_DATACHANNEL_DIAGNOSTICS=1
    export WEBKIT_CLOUD_WEBSOCKET_DIAGNOSTICS=1
    export WEBKIT_CLOUD_SDP_DIAGNOSTICS=1
    export GST_DEBUG="${GST_DEBUG:-2,decodebin:4,uridecodebin:4,webkit*:3,webrtcbin:4,webrtcdatachannel:6,sctp*:6,dtlsconnection:4,dtlssrtp*:4,webrtctransport*:5}"
else
    export GST_DEBUG="${GST_DEBUG:-1,webkit*:2}"
fi
if [ "${WPE_GST_DOT:-0}" = "1" ]; then
    GST_DOT_DIR="${WPE_GST_DOT_DIR:-$VAR_DIR/gst-dot}"
    mkdir -p "$GST_DOT_DIR"
    chmod 700 "$GST_DOT_DIR" 2>/dev/null || true
    export GST_DEBUG_DUMP_DOT_DIR="$GST_DOT_DIR"
    echo "WPE GStreamer: gst_dot=enabled dir=$GST_DOT_DIR"
else
    unset GST_DEBUG_DUMP_DOT_DIR
fi
export WEBKIT_GST_DISABLE_GL_SINK="${WEBKIT_GST_DISABLE_GL_SINK:-1}"
export WEBKIT_WEBGL_DISABLE_GBM="${WEBKIT_WEBGL_DISABLE_GBM:-1}"
export WEBKIT_DISABLE_DMABUF_ATLAS="${WEBKIT_DISABLE_DMABUF_ATLAS:-1}"
export WEBKIT_FORCE_VBLANK_TIMER="${WEBKIT_FORCE_VBLANK_TIMER:-1}"
# 低配设备（1GB/4xA53/无GPU）调优：
# - 恢复系统级内存压力监控（关闭会同时使 tile 预取恒 2x）
# - Skia 走原生 CPU 光栅化，不再经 softpipe 软件 GL 模拟的 GPU 路径（收益最大）
# - 刷新节流 30 fps（须为刷新率 60 的因子）：视频是核心场景，20 会卡 25/30fps 视频
# - JSC：FTL JIT 关（最耗内存层）、看门狗 8s 防死循环；
#   forceRAMSize 让 JSC 按 512MB 预算做比例式堆增长（gcMaxHeapSize 非硬上限已弃用）；
#   GC marker 降 2、mutator 时间片上调、JIT warmup 阈值上调减少编译抖动
# - MSE_MAX_BUFFER_SIZE：MSE 每 SourceBuffer 默认 304MB，抖音多 video 同时
#   buffer 会吃掉几百 MB，收紧到 视频40M/音频8M
export WEBKIT_SKIA_ENABLE_CPU_RENDERING="${WEBKIT_SKIA_ENABLE_CPU_RENDERING:-1}"
export WEBKIT_SKIA_CPU_PAINTING_THREADS="${WEBKIT_SKIA_CPU_PAINTING_THREADS:-3}"
export WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS="${WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS:-30}"
export JSC_useFTLJIT="${JSC_useFTLJIT:-false}"
export JSC_watchdog="${JSC_watchdog:-8000}"
export JSC_forceRAMSize="${JSC_forceRAMSize:-536870912}"
export JSC_numberOfGCMarkers="${JSC_numberOfGCMarkers:-2}"
export JSC_maximumMutatorUtilization="${JSC_maximumMutatorUtilization:-0.85}"
export JSC_thresholdForJITAfterWarmUp="${JSC_thresholdForJITAfterWarmUp:-2000}"
export JSC_thresholdForOptimizeAfterWarmUp="${JSC_thresholdForOptimizeAfterWarmUp:-6000}"
export MSE_MAX_BUFFER_SIZE="${MSE_MAX_BUFFER_SIZE:-V:40M,A:8M}"
export WPE_TOUCH_HORIZONTAL_SCROLL="${WPE_TOUCH_HORIZONTAL_SCROLL:-1}"
export WPE_DRM_MAX_FPS="${WPE_DRM_MAX_FPS:-0}"
export WPE_DRM_FENCE_TIMEOUT_MS="${WPE_DRM_FENCE_TIMEOUT_MS:-2000}"
export WPE_DRM_PARTIAL_COPY="${WPE_DRM_PARTIAL_COPY:-0}"
export WPE_WEBRTC="${WPE_WEBRTC:-1}"
export WPE_WEBRTC_CAPTURE="${WPE_WEBRTC_CAPTURE:-deny}"
# 云原神的信令 WSS 当前在 *.mhystatic.com:5443 返回已过期证书。
# WebKit 内核只对该域名后缀、该端口且错误仅为 EXPIRED 时放行；
# 未知 CA、域名不匹配及其他 TLS 错误继续拒绝。设为 0 可完全关闭例外。
export WPE_CLOUDGAME_ALLOW_EXPIRED_TLS="${WPE_CLOUDGAME_ALLOW_EXPIRED_TLS:-1}"

# 视频 KMS overlay 直出（hole-punch）：mppvideodec 解码 NV12 dmabuf 经 RGA
# 预旋转后直接放 DRM overlay plane，跳过软件颜色转换与 Skia 合成。
# WPE_VIDEO_OVERLAY=0 一键回退纯软件视频路径。
if [ "${WPE_VIDEO_OVERLAY:-1}" = "1" ]; then
    export WEBKIT_GST_HOLE_PUNCH_QUIRK="${WEBKIT_GST_HOLE_PUNCH_QUIRK:-rockchip}"
    export WEBKIT_GST_HOLE_PUNCH_MEDIASTREAM_EARLY="${WEBKIT_GST_HOLE_PUNCH_MEDIASTREAM_EARLY:-1}"
    export WPE_VIDEO_OVERLAY_SOCKET="${WPE_VIDEO_OVERLAY_SOCKET:-$VAR_DIR/wpe-video-overlay.sock}"
    export WPE_VIDEO_OVERLAY_ROTATION="${WPE_VIDEO_OVERLAY_ROTATION:-${WPE_DRM_ROTATION:-${WPE_PANEL_ROTATION:-0}}}"
    export WPE_VIDEO_OVERLAY_FIT="${WPE_VIDEO_OVERLAY_FIT:-contain}"
fi

# MiniApp 是 WPE 的生命周期宿主，OOM 时必须优先保住 MiniApp。WPE 退出后可由
# watchdog 返回启动页或重新启动；MiniApp 被杀会让整套框架重启。
WPE_OOM_SCORE_ADJ="${WPE_OOM_SCORE_ADJ:-200}"
case "$WPE_OOM_SCORE_ADJ" in
    -[0-9]*|[0-9]*) echo "$WPE_OOM_SCORE_ADJ" > /proc/self/oom_score_adj 2>/dev/null || true ;;
    *) echo "WPE warn: invalid WPE_OOM_SCORE_ADJ=$WPE_OOM_SCORE_ADJ; keeping system default" ;;
esac

# 全局 VM 调优默认关闭。高 swappiness 在 512MB swap 已满的设备上会放大交互
# 延迟；drop_caches 也会让浏览器首屏重新产生大量 I/O。仅保留显式诊断开关。
if [ "${WPE_DROP_CACHES:-0}" = "1" ]; then
    sync 2>/dev/null || true
    echo 1 > /proc/sys/vm/drop_caches 2>/dev/null || true
fi
WPE_SAVED_SWAPPINESS=""
if [ -n "${WPE_SWAPPINESS:-}" ]; then
    WPE_SAVED_SWAPPINESS="$(cat /proc/sys/vm/swappiness 2>/dev/null)"
    if [ -n "$WPE_SAVED_SWAPPINESS" ]; then
        echo "$WPE_SWAPPINESS" > /proc/sys/vm/swappiness 2>/dev/null || WPE_SAVED_SWAPPINESS=""
    fi
fi
restore_swappiness() {
    if [ -n "$WPE_SAVED_SWAPPINESS" ]; then
        echo "$WPE_SAVED_SWAPPINESS" > /proc/sys/vm/swappiness 2>/dev/null || true
    fi
}
export WPE_CHROME_ENABLED="${WPE_CHROME_ENABLED:-1}"
export WPE_CHROME_HEIGHT="${WPE_CHROME_HEIGHT:-44}"
export WPE_CHROME_HIT_SLOP="${WPE_CHROME_HIT_SLOP:-6}"
export WPE_CHROME_REVEAL_HEIGHT="${WPE_CHROME_REVEAL_HEIGHT:-22}"
export WPE_CHROME_TAP_MAX_MOVE="${WPE_CHROME_TAP_MAX_MOVE:-32}"
export WPE_CHROME_ANIMATION_MS="${WPE_CHROME_ANIMATION_MS:-160}"
export WPE_CHROME_HIDE_DOWN_PX="${WPE_CHROME_HIDE_DOWN_PX:-96}"
export WPE_CHROME_SHOW_UP_PX="${WPE_CHROME_SHOW_UP_PX:-180}"
export WPE_CHROME_SHOW_UP_MIN_VELOCITY="${WPE_CHROME_SHOW_UP_MIN_VELOCITY:-700}"
export WPE_CHROME_LAYOUT="${WPE_CHROME_LAYOUT:-inset}"
export WPE_CHROME_STATE="${WPE_CHROME_STATE:-$VAR_DIR/browser-state.ini}"
export WPE_CHROME_RENDER_STATE="${WPE_CHROME_RENDER_STATE:-$VAR_DIR/chrome-render-state.ini}"
export WPE_BROWSER_DB="${WPE_BROWSER_DB:-$VAR_DIR/browser.sqlite3}"
export WPE_PROFILES_DIR="${WPE_PROFILES_DIR:-$VAR_DIR/profiles}"
export WPE_PROFILE_SWITCH_FILE="${WPE_PROFILE_SWITCH_FILE:-$VAR_DIR/profile-switch.request}"
export WPE_CHROME_FONT="${WPE_CHROME_FONT:-$DIR/assets/fonts/miniapp/HarmonyOS_Sans_SC_Regular.ttf}"
export WPE_CHROME_FONT_MEDIUM="${WPE_CHROME_FONT_MEDIUM:-$DIR/assets/fonts/miniapp/HarmonyOS_Sans_SC_Medium.ttf}"
export WPE_CHROME_FONT_BOLD="${WPE_CHROME_FONT_BOLD:-$DIR/assets/fonts/miniapp/HarmonyOS_Sans_SC_Bold.ttf}"
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
mkdir -p "$FONTCONFIG_CACHE_DIR"
# 字体目录扫描与 fonts.conf 生成结果缓存：runtime 字体集不变时直接复用，
# 避免每次启动都全量 find 字体目录并重写配置。
REBUILD_FONTCONFIG=1
if [ -s "$FONT_DIRS_FILE" ] && [ -s "$FONTCONFIG_RUNTIME_FILE" ] \
    && grep -q "$BUNDLED_FONT_ROOT" "$FONTCONFIG_RUNTIME_FILE" 2>/dev/null \
    && [ -z "$(find "$BUNDLED_FONT_ROOT" -newer "$FONTCONFIG_RUNTIME_FILE" -print 2>/dev/null | head -n 1)" ]; then
    REBUILD_FONTCONFIG=0
fi
if [ "$REBUILD_FONTCONFIG" = 1 ]; then
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
fi
export FONTCONFIG_PATH="$(dirname "$FONTCONFIG_RUNTIME_FILE")"
export FONTCONFIG_FILE="$FONTCONFIG_RUNTIME_FILE"
unset FONTCONFIG_SYSROOT
export GSETTINGS_SCHEMA_DIR="$DIR/share/glib-2.0/schemas"
export XKB_CONFIG_ROOT="$DIR/share/X11/xkb"
export LIBINPUT_QUIRKS_DIR="$DIR/share/libinput"
export WEBKIT_EXEC_PATH="$DIR/libexec/wpe-webkit-2.0"
export WEBKIT_INJECTED_BUNDLE_PATH="$DIR/lib/wpe-webkit-2.0/injected-bundle"
# Cloud gaming uses the early H.264/Mpp path so encoded access units reach the
# hardware decoder without blocking the final MediaStream player. Audio is
# attached only after video preroll; disabling it remains an explicit rollback.
export WEBKIT_GST_WEBRTC_FORCE_EARLY_VIDEO_DECODING="${WEBKIT_GST_WEBRTC_FORCE_EARLY_VIDEO_DECODING:-1}"
export WEBKIT_GST_WEBRTC_DISABLE_PLAYER_AUDIO="${WEBKIT_GST_WEBRTC_DISABLE_PLAYER_AUDIO:-0}"
export WEBKIT_GST_WEBRTC_DIRECT_REMOTE_AUDIO="${WEBKIT_GST_WEBRTC_DIRECT_REMOTE_AUDIO:-1}"
export WPE_WEB_PROCESS_MEMORY_LIMIT_MB="${WPE_WEB_PROCESS_MEMORY_LIMIT_MB:-448}"
export WEBKIT_GST_WEBRTC_DEFER_PLAYER_AUDIO="${WEBKIT_GST_WEBRTC_DEFER_PLAYER_AUDIO:-0}"
export WEBKIT_GST_WEBRTC_ALLOW_EARLY_AUDIO="${WEBKIT_GST_WEBRTC_ALLOW_EARLY_AUDIO:-1}"
export WEBKIT_GST_MEDIASTREAM_DIRECT_AUDIO_SINK="${WEBKIT_GST_MEDIASTREAM_DIRECT_AUDIO_SINK:-1}"
export WEBKIT_GST_WEBRTC_FORCE_EARLY_AUDIO_DECODING="${WEBKIT_GST_WEBRTC_FORCE_EARLY_AUDIO_DECODING:-1}"
export WEBKIT_GST_WEBRTC_DISABLE_INCOMING_SYNC="${WEBKIT_GST_WEBRTC_DISABLE_INCOMING_SYNC:-1}"
export WEBKIT_GST_WEBRTC_LEAKY_RAW_VIDEO="${WEBKIT_GST_WEBRTC_LEAKY_RAW_VIDEO:-1}"
export WEBKIT_GST_WEBRTC_COALESCE_INCOMING_VIDEO="${WEBKIT_GST_WEBRTC_COALESCE_INCOMING_VIDEO:-1}"
export WEBKIT_GST_WEBRTC_RETIMESTAMP_INCOMING="${WEBKIT_GST_WEBRTC_RETIMESTAMP_INCOMING:-1}"
export WEBKIT_GST_WEBRTC_COPY_DECODED_VIDEO="${WEBKIT_GST_WEBRTC_COPY_DECODED_VIDEO:-0}"
export WEBKIT_GST_WEBRTC_REPARSE_H264="${WEBKIT_GST_WEBRTC_REPARSE_H264:-1}"
export WEBKIT_GST_MPP_REQUIRE_H264_AU="${WEBKIT_GST_MPP_REQUIRE_H264_AU:-1}"
# The device has no usable GPU, but dma-heap direct scanout is still much
# faster than SHM dumb-buffer copies. Set WPE_DRM_FORCE_SHM=1 only for fallback
# diagnostics on kernels without a working dma-heap scanout path.
export WPE_DRM_FORCE_SHM="${WPE_DRM_FORCE_SHM:-0}"
export WPE_DRM_BUFFER_PATH="${WPE_DRM_BUFFER_PATH:-dma_heap}"
export WPE_DRM_RUNTIME_DIR="${WPE_DRM_RUNTIME_DIR:-$VAR_DIR/runtime-tmp}"
export WPE_KEYBOARD_DIR="${WPE_KEYBOARD_DIR:-$VAR_DIR/keyboard}"
export WPE_RAW_TOUCH="${WPE_RAW_TOUCH:-1}"
export WPE_TOUCH_DEVICE="${WPE_TOUCH_DEVICE:-/dev/input/by-path/hyn_ts}"
export WPE_TOUCH_OFFSET_X="${WPE_TOUCH_OFFSET_X:-0}"
export WPE_TOUCH_OFFSET_Y="${WPE_TOUCH_OFFSET_Y:-0}"
export WPE_SEND_TOUCH_EVENTS="${WPE_SEND_TOUCH_EVENTS:-1}"
export WPE_SYNTHESIZE_POINTER_TAP="${WPE_SYNTHESIZE_POINTER_TAP:-1}"
export WPE_GAME_POINTER_TAP_FALLBACK="${WPE_GAME_POINTER_TAP_FALLBACK:-0}"
export WPE_GAME_MEDIA_IMMERSIVE="${WPE_GAME_MEDIA_IMMERSIVE:-1}"
export WPE_TOUCH_SCROLL_FALLBACK="${WPE_TOUCH_SCROLL_FALLBACK:-0}"
export WPE_TOUCH_NATIVE_SCROLL="${WPE_TOUCH_NATIVE_SCROLL:-1}"
export WPE_TOUCH_JS_SCROLL="${WPE_TOUCH_JS_SCROLL:-0}"
export WPE_TOUCH_SCROLL_INVERT_Y="${WPE_TOUCH_SCROLL_INVERT_Y:-0}"
export WPE_TOUCH_SCROLL_SCALE="${WPE_TOUCH_SCROLL_SCALE:-1.0}"
export WPE_TOUCH_SCROLL_MAX_STEP="${WPE_TOUCH_SCROLL_MAX_STEP:-32}"
export WPE_TOUCH_SCROLL_PENDING_LIMIT="${WPE_TOUCH_SCROLL_PENDING_LIMIT:-64}"
export WPE_TOUCH_TAP_MAX_MOVE="${WPE_TOUCH_TAP_MAX_MOVE:-32}"
export WPE_TOUCH_SCROLL_INTERVAL_MS="${WPE_TOUCH_SCROLL_INTERVAL_MS:-16}"
export WPE_TOUCH_SCROLL_STOP_DELAY_MS="${WPE_TOUCH_SCROLL_STOP_DELAY_MS:-80}"
export WPE_INPUT_PROFILE="${WPE_INPUT_PROFILE:-auto}"
export WPE_GAME_HOSTS="${WPE_GAME_HOSTS:-ys.mihoyo.com,cloudgame.mihoyo.com}"
export WPE_CLOUD_AUTOSTART="${WPE_CLOUD_AUTOSTART:-1}"
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
# 2026-04 起的新内核可能没有 dma_heap 节点（实测 5.10.160 #6 黑屏根因：
# WPE_DRM_BUFFER_PATH=dma_heap 强制路径无回退，WebProcess 产不出帧）。
# 先在同目录里找可用 heap，全都没有就回退 SHM 路径。
if [ ! -e "$WPE_DRM_DMA_HEAP" ]; then
    for heap in /dev/dma_heap/system /dev/dma_heap/system-uncached /dev/dma_heap/*; do
        if [ -e "$heap" ]; then
            WPE_DRM_DMA_HEAP="$heap"
            break
        fi
    done
fi
if [ ! -e "$WPE_DRM_DMA_HEAP" ]; then
    echo "WPE warn: no dma_heap device available, falling back to SHM buffer path"
    export WPE_DRM_FORCE_SHM=1
    export WPE_DRM_BUFFER_PATH=shm
fi
export WPE_DRM_DMA_HEAP
CPU_BUFFER_PATH="$WPE_DRM_BUFFER_PATH"
CPU_FORCE_SHM="$WPE_DRM_FORCE_SHM"
CPU_DMA_HEAP="$WPE_DRM_DMA_HEAP"
CPU_COMPOSITOR="${WEBKIT_SKIA_CPU_COMPOSITOR:-1}"

MALI_LIB="$DIR/gpu/mali/lib"
GPU_MODULE_DIR="$DIR/gpu/modules/5.10.160"
GPU_PROBE="$DIR/libexec/wpe-gpu-probe"
GPU_SHIM="$MALI_LIB/libwpe-mali-gbm-compat.so"
GPU_MODE="${WPE_GPU_MODE:-auto}"
GPU_OWN_HELPER=0
GPU_OWN_KBASE=0
GPU_READY_FILE="$VAR_DIR/gpu-first-frame.ready"
BROWSER_PID=
KEEP_PID=
TERMINATING=0
PROFILE_SWITCH_COUNT=0
rm -f "$WPE_PROFILE_SWITCH_FILE"

case "$GPU_MODE" in
    auto|off|required) ;;
    *)
        echo "WPE warning: invalid WPE_GPU_MODE=$GPU_MODE, using auto"
        GPU_MODE=auto
        ;;
esac
export WPE_GPU_MODE="$GPU_MODE"

module_loaded() {
    grep -q "^$1 " /proc/modules 2>/dev/null
}

cleanup_gpu_modules() {
    if [ "$GPU_OWN_KBASE" = 1 ]; then
        GPU_UNLOAD_WAIT=0
        while ! rmmod bifrost_kbase >/dev/null 2>&1; do
            GPU_UNLOAD_WAIT=$((GPU_UNLOAD_WAIT + 1))
            [ "$GPU_UNLOAD_WAIT" -lt 5 ] || break
            sleep 1
        done
        if module_loaded bifrost_kbase; then
            echo "WPE GPU warning: failed to unload bifrost_kbase; keeping helper loaded"
            return
        fi
        echo "WPE GPU: unloaded browser-owned module bifrost_kbase wait=${GPU_UNLOAD_WAIT}s"
        GPU_OWN_KBASE=0
    fi
    if [ "$GPU_OWN_HELPER" = 1 ]; then
        if rmmod x7_gpu_dt_enable >/dev/null 2>&1; then
            echo "WPE GPU: unloaded browser-owned module x7_gpu_dt_enable"
            GPU_OWN_HELPER=0
        else
            echo "WPE GPU warning: failed to unload x7_gpu_dt_enable"
        fi
    fi
}

module_vermagic_matches() {
    [ -f "$1" ] || return 1
    if command -v modinfo >/dev/null 2>&1; then
        modinfo -F vermagic "$1" 2>/dev/null | grep -q '^5.10.160.*aarch64'
        return $?
    fi
    if command -v strings >/dev/null 2>&1; then
        strings "$1" 2>/dev/null | grep -q 'vermagic=5.10.160.*aarch64'
        return $?
    fi
    grep -a -q 'vermagic=5.10.160.*aarch64' "$1" 2>/dev/null
}

load_gpu_modules() {
    if [ -c /dev/mali0 ]; then
        echo "WPE GPU: existing /dev/mali0 detected; no module load"
        return 0
    fi
    if [ "$(uname -m 2>/dev/null)" != aarch64 ] || [ "$(uname -r 2>/dev/null)" != 5.10.160 ]; then
        echo "WPE GPU unavailable: module load requires aarch64 kernel 5.10.160"
        return 1
    fi
    if [ "$(id -u 2>/dev/null)" != 0 ] || ! command -v insmod >/dev/null 2>&1; then
        echo "WPE GPU unavailable: root/insmod permission missing"
        return 1
    fi
    if [ "$(cat /proc/sys/kernel/modules_disabled 2>/dev/null)" = 1 ]; then
        echo "WPE GPU unavailable: kernel module loading is disabled"
        return 1
    fi
    if ! module_vermagic_matches "$GPU_MODULE_DIR/x7_gpu_dt_enable.ko" || \
       ! module_vermagic_matches "$GPU_MODULE_DIR/bifrost_kbase.ko"; then
        echo "WPE GPU unavailable: KO missing or vermagic mismatch"
        return 1
    fi
    if module_loaded bifrost_kbase; then
        echo "WPE GPU unavailable: bifrost_kbase is already loaded but /dev/mali0 is absent"
        return 1
    fi

    if ! module_loaded x7_gpu_dt_enable; then
        OF_UPDATE_PROPERTY_ADDR=$(awk \
            '$3 == "of_update_property" && $2 ~ /^[Tt]$/ && $1 !~ /^0+$/ { print "0x" $1; exit }' \
            /proc/kallsyms 2>/dev/null)
        if [ -n "$OF_UPDATE_PROPERTY_ADDR" ]; then
            insmod "$GPU_MODULE_DIR/x7_gpu_dt_enable.ko" \
                of_update_property_addr="$OF_UPDATE_PROPERTY_ADDR" >/dev/null 2>&1 || return 1
        else
            insmod "$GPU_MODULE_DIR/x7_gpu_dt_enable.ko" >/dev/null 2>&1 || return 1
        fi
        GPU_OWN_HELPER=1
        echo "WPE GPU: loaded browser-owned module x7_gpu_dt_enable"
    fi

    if ! insmod "$GPU_MODULE_DIR/bifrost_kbase.ko" >/dev/null 2>&1; then
        echo "WPE GPU unavailable: bifrost_kbase insmod failed"
        cleanup_gpu_modules
        return 1
    fi
    GPU_OWN_KBASE=1
    echo "WPE GPU: loaded browser-owned module bifrost_kbase"

    GPU_WAIT=0
    while [ "$GPU_WAIT" -lt 5 ] && [ ! -c /dev/mali0 ]; do
        sleep 1
        GPU_WAIT=$((GPU_WAIT + 1))
    done
    if [ ! -c /dev/mali0 ]; then
        echo "WPE GPU unavailable: /dev/mali0 was not created"
        cleanup_gpu_modules
        return 1
    fi
    return 0
}

configure_cpu_profile() {
    export WPE_RENDER_PROFILE=cpu
    export LD_LIBRARY_PATH="$MESA/lib:$DIR/lib"
    unset LD_PRELOAD
    export LIBGL_DRIVERS_PATH="$MESA/lib/dri"
    export GBM_BACKENDS_PATH="$MESA/lib/gbm"
    export GBM_BACKEND=drm
    export GALLIUM_DRIVER=softpipe
    export MESA_LOADER_DRIVER_OVERRIDE=kms_swrast
    export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
    export WEBKIT_SKIA_CPU_COMPOSITOR="$CPU_COMPOSITOR"
    export WEBKIT_SKIA_CPU_PAINTING_THREADS="${WPE_CPU_PAINTING_THREADS:-3}"
    unset WEBKIT_SKIA_GPU_PAINTING_THREADS
    export WEBKIT_WEBGL_DISABLE_GBM=1
    export WEBKIT_DISABLE_DMABUF_ATLAS=1
    export WPE_DRM_BUFFER_PATH="$CPU_BUFFER_PATH"
    export WPE_DRM_FORCE_SHM="$CPU_FORCE_SHM"
    export WPE_DRM_DMA_HEAP="$CPU_DMA_HEAP"
    unset WPE_DMABUF_BUFFER_FORMAT
    unset WEBKIT_SKIA_USE_LINEAR_TILE_TEXTURES
}

configure_gpu_profile() {
    export WPE_RENDER_PROFILE=gpu
    export LD_LIBRARY_PATH="$MALI_LIB:$DIR/lib"
    export LD_PRELOAD="$GPU_SHIM"
    unset LIBGL_DRIVERS_PATH
    unset GBM_BACKENDS_PATH
    unset GBM_BACKEND
    unset GALLIUM_DRIVER
    unset MESA_LOADER_DRIVER_OVERRIDE
    export WEBKIT_SKIA_ENABLE_CPU_RENDERING=0
    export WEBKIT_SKIA_CPU_COMPOSITOR=0
    export WEBKIT_SKIA_GPU_PAINTING_THREADS=1
    unset WEBKIT_SKIA_CPU_PAINTING_THREADS
    export WEBKIT_WEBGL_DISABLE_GBM=0
    export WEBKIT_DISABLE_DMABUF_ATLAS=0
    export WPE_DRM_BUFFER_PATH=auto
    export WPE_DRM_FORCE_SHM=0
    # The Rockchip video overlay sits below the WPE primary plane. Keep the
    # primary ARGB-capable so WebKit's hole-punch pixels remain transparent.
    export WPE_DMABUF_BUFFER_FORMAT=AR24:0:scanout
    export WEBKIT_SKIA_USE_LINEAR_TILE_TEXTURES=1
}

gpu_assets_ready() {
    [ -x "$GPU_PROBE" ] && [ -f "$GPU_SHIM" ] && \
    [ -f "$MALI_LIB/libmali.so.1" ] && [ -f "$MALI_LIB/libmali_hook.so.1" ] && \
    [ -f "$MALI_LIB/libEGL.so.1" ] && [ -f "$MALI_LIB/libGLESv2.so.2" ] && \
    [ -f "$MALI_LIB/libgbm.so.1" ]
}

probe_gpu() {
    if ! gpu_assets_ready; then
        echo "WPE GPU unavailable: local Mali runtime/probe is not packaged"
        return 1
    fi
    load_gpu_modules || return 1
    configure_gpu_profile
    "$GPU_PROBE" "$DRM"
    GPU_PROBE_STATUS=$?
    if [ "$GPU_PROBE_STATUS" -eq 0 ]; then
        echo "WPE GPU probe: result=ok"
        return 0
    fi
    echo "WPE GPU probe: result=failed status=$GPU_PROBE_STATUS"
    cleanup_gpu_modules
    return 1
}

cleanup_runtime() {
    if [ -n "$KEEP_PID" ]; then
        kill "$KEEP_PID" 2>/dev/null || true
        KEEP_PID=
    fi
    cleanup_gpu_modules
    restore_swappiness
}

on_terminate() {
    TERMINATING=1
    if [ -n "$BROWSER_PID" ]; then
        kill "$BROWSER_PID" 2>/dev/null || true
    fi
}

run_browser_profile() {
    rm -f "$GPU_READY_FILE"
    export WPE_GPU_READY_FILE="$GPU_READY_FILE"
    "$DIR/wpe-drm-minimal" "$URL" "$DRM" "$VIEWPORT" "$ROTATION" &
    BROWSER_PID=$!
    wait "$BROWSER_PID"
    BROWSER_STATUS=$?
    BROWSER_PID=
    return "$BROWSER_STATUS"
}

run_profile_switch_loop() {
    while :; do
        run_browser_profile
        BROWSER_STATUS=$?
        if [ "$BROWSER_STATUS" -ne 75 ] || [ "$TERMINATING" != 0 ]; then
            return "$BROWSER_STATUS"
        fi
        PROFILE_SWITCH_COUNT=$((PROFILE_SWITCH_COUNT + 1))
        if [ "$PROFILE_SWITCH_COUNT" -gt 32 ]; then
            echo "WPE profile switch aborted: restart limit exceeded"
            return 76
        fi
        if [ ! -f "$WPE_PROFILE_SWITCH_FILE" ]; then
            echo "WPE profile switch failed: request file missing"
            return 76
        fi
        PROFILE_REQUEST=$(sed -n '1p' "$WPE_PROFILE_SWITCH_FILE" 2>/dev/null || true)
        rm -f "$WPE_PROFILE_SWITCH_FILE"
        case "$PROFILE_REQUEST" in
            guest) ;;
            ''|*[!0-9]*)
                echo "WPE profile switch failed: invalid request=$PROFILE_REQUEST"
                return 76
                ;;
        esac
        export WPE_PROFILE_OVERRIDE="$PROFILE_REQUEST"
        echo "WPE profile restart: request=$PROFILE_REQUEST count=$PROFILE_SWITCH_COUNT profile=$SELECTED_PROFILE"
    done
}

trap cleanup_runtime EXIT
trap on_terminate INT TERM
printf '%s\n' "$ROTATION" >"$WPE_DRM_ROTATION_FILE" 2>/dev/null || true

SELECTED_PROFILE=cpu
if [ "$GPU_MODE" != off ] && probe_gpu; then
    SELECTED_PROFILE=gpu
else
    cleanup_gpu_modules
    if [ "$GPU_MODE" = required ]; then
        echo "WPE fatal: required Mali GPU profile is unavailable"
        exit 90
    fi
    configure_cpu_profile
fi

echo "WPE launch: profile=$SELECTED_PROFILE gpu_mode=$GPU_MODE compositor=$WEBKIT_SKIA_CPU_COMPOSITOR url=$URL drm=$DRM panel=$WPE_PANEL_SIZE drm_mode=$WPE_DRM_MODE viewport=$VIEWPORT rotation=$ROTATION panel_rotation=$WPE_PANEL_ROTATION touch_rotation=$WPE_TOUCH_ROTATION touch_device=$WPE_TOUCH_DEVICE touch_offset=$WPE_TOUCH_OFFSET_X,$WPE_TOUCH_OFFSET_Y browser_mode=${WPE_BROWSER_MODE:-unknown} display_source=${WPE_DISPLAY_SOURCE:-unknown} fit=$WPE_DRM_FIT video_fit=${WPE_VIDEO_OVERLAY_FIT:-disabled} video_rotation=${WPE_VIDEO_OVERLAY_ROTATION:-disabled} chrome_layout=$WPE_CHROME_LAYOUT gst_source=$GST_SOURCE panel_crtc_x=$WPE_PANEL_CRTC_X rotated_x=${WPE_DRM_ROTATED_X:-auto} touch_active_x=$WPE_TOUCH_ACTIVE_X fps=$WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS max_fps=$WPE_DRM_MAX_FPS web_mem_mb=$WPE_WEB_PROCESS_MEMORY_LIMIT_MB mem_pressure_monitor=$WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR heap=$WPE_DRM_DMA_HEAP buffer_path=$WPE_DRM_BUFFER_PATH keyboard_dir=$WPE_KEYBOARD_DIR browser_db=$WPE_BROWSER_DB profiles_dir=$WPE_PROFILES_DIR profile_override=${WPE_PROFILE_OVERRIDE:-last}"

if command -v hal-screen >/dev/null 2>&1; then
    env -u LD_LIBRARY_PATH -u LD_PRELOAD hal-screen on >/dev/null 2>&1 || true
    (
        while :; do
            env -u LD_LIBRARY_PATH -u LD_PRELOAD hal-screen keep >/dev/null 2>&1 || true
            sleep 20
        done
    ) &
    KEEP_PID=$!
fi

run_profile_switch_loop
STATUS=$?
if [ "$STATUS" -eq 74 ]; then
    echo "WPE user shutdown completed"
    exit 0
fi
if [ "$SELECTED_PROFILE" = gpu ] && [ "$TERMINATING" = 0 ] && \
   { [ "$STATUS" -eq 91 ] || [ ! -f "$GPU_READY_FILE" ]; }; then
    if [ "$GPU_MODE" = required ]; then
        echo "WPE fatal: required GPU launch failed status=$STATUS ready=0"
        exit "$STATUS"
    fi
    echo "WPE GPU runtime failed before first frame status=$STATUS; restarting once with CPU profile"
    cleanup_gpu_modules
    configure_cpu_profile
    SELECTED_PROFILE=cpu-fallback
    run_profile_switch_loop
    STATUS=$?
fi
if [ "$STATUS" -eq 74 ]; then
    echo "WPE user shutdown completed"
    STATUS=0
fi
exit "$STATUS"
