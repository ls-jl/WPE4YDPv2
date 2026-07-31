#!/bin/sh

# Mali probing and render-profile selection. The supervisor owns process
# restart policy; this module only owns GPU modules loaded by this browser.

gpu_runtime_init() {
    MALI_LIB="$DIR/gpu/mali/lib"
    GPU_MODULE_DIR="$DIR/gpu/modules/5.10.160"
    GPU_PROBE="$DIR/libexec/wpe-gpu-probe"
    GPU_SHIM="$MALI_LIB/libwpe-mali-gbm-compat.so"
    GPU_MODE="${WPE_GPU_MODE:-auto}"
    GPU_MODE_FILE="${WPE_GPU_MODE_FILE:-$VAR_DIR/gpu-mode}"
    GPU_OWN_HELPER=0
    GPU_OWN_KBASE=0
    GPU_READY_FILE="$VAR_DIR/gpu-first-frame.ready"
    load_gpu_mode_preference
}

load_gpu_mode_preference() {
    if [ -r "$GPU_MODE_FILE" ]; then
        GPU_MODE_PREFERENCE=$(sed -n '1p' "$GPU_MODE_FILE" 2>/dev/null || true)
        case "$GPU_MODE_PREFERENCE" in
            auto|off|required) GPU_MODE="$GPU_MODE_PREFERENCE" ;;
            *) echo "WPE warning: invalid GPU mode preference in $GPU_MODE_FILE" ;;
        esac
    fi
    case "$GPU_MODE" in
        auto|off|required) ;;
        *)
            echo "WPE warning: invalid WPE_GPU_MODE=$GPU_MODE, using auto"
            GPU_MODE=auto
            ;;
    esac
    export WPE_GPU_MODE="$GPU_MODE"
    export WPE_GPU_MODE_FILE="$GPU_MODE_FILE"
}

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
    unset LIBGL_DRIVERS_PATH GBM_BACKENDS_PATH GBM_BACKEND
    unset GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
    export WEBKIT_SKIA_ENABLE_CPU_RENDERING=0
    export WEBKIT_SKIA_CPU_COMPOSITOR=0
    export WEBKIT_SKIA_GPU_PAINTING_THREADS=1
    unset WEBKIT_SKIA_CPU_PAINTING_THREADS
    export WEBKIT_WEBGL_DISABLE_GBM=0
    export WEBKIT_DISABLE_DMABUF_ATLAS=0
    export WPE_DRM_BUFFER_PATH=auto
    export WPE_DRM_FORCE_SHM=0
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

select_render_profile() {
    SELECTED_PROFILE=cpu
    if [ "$GPU_MODE" != off ] && probe_gpu; then
        SELECTED_PROFILE=gpu
        export WPE_GPU_STATUS=active
        return 0
    fi
    cleanup_gpu_modules
    if [ "$GPU_MODE" = required ]; then
        echo "WPE fatal: required Mali GPU profile is unavailable"
        return 90
    fi
    configure_cpu_profile
    if [ "$GPU_MODE" = off ]; then
        export WPE_GPU_STATUS=off
    else
        export WPE_GPU_STATUS=unavailable
    fi
    return 0
}
