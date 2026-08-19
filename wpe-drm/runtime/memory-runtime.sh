#!/bin/sh

# Select one coherent memory profile for WebKit, JSC, MSE and the system
# pressure monitor. Tests may point WPE_MEMORY_MEMINFO at a fixture.

memory_value_kb() {
    key="$1"
    file="${WPE_MEMORY_MEMINFO:-/proc/meminfo}"
    awk -v key="$key" '$1 == key ":" { print $2; exit }' "$file" 2>/dev/null
}

memory_clamp_uint() {
    value="$1"
    minimum="$2"
    maximum="$3"
    fallback="$4"
    case "$value" in
        ''|*[!0-9]*) value="$fallback" ;;
    esac
    [ "$value" -ge "$minimum" ] 2>/dev/null || value="$minimum"
    [ "$value" -le "$maximum" ] 2>/dev/null || value="$maximum"
    printf '%s\n' "$value"
}

memory_runtime_init() {
    WPE_MEMORY_TOTAL_KB="$(memory_value_kb MemTotal)"
    WPE_MEMORY_AVAILABLE_KB="$(memory_value_kb MemAvailable)"
    WPE_MEMORY_SWAP_TOTAL_KB="$(memory_value_kb SwapTotal)"
    WPE_MEMORY_SWAP_FREE_KB="$(memory_value_kb SwapFree)"
    WPE_MEMORY_TOTAL_KB="${WPE_MEMORY_TOTAL_KB:-0}"
    WPE_MEMORY_AVAILABLE_KB="${WPE_MEMORY_AVAILABLE_KB:-0}"
    WPE_MEMORY_SWAP_TOTAL_KB="${WPE_MEMORY_SWAP_TOTAL_KB:-0}"
    WPE_MEMORY_SWAP_FREE_KB="${WPE_MEMORY_SWAP_FREE_KB:-0}"

    if [ -n "${WPE_WEB_PROCESS_MEMORY_LIMIT_MB+x}" ]; then
        WPE_WEB_PROCESS_MEMORY_LIMIT_MB="$(memory_clamp_uint \
            "$WPE_WEB_PROCESS_MEMORY_LIMIT_MB" 192 2048 448)"
        if [ -z "${WPE_MEMORY_PROFILE:-}" ]; then
            if [ "$WPE_WEB_PROCESS_MEMORY_LIMIT_MB" -ge 600 ]; then
                WPE_MEMORY_PROFILE=large
            elif [ "$WPE_WEB_PROCESS_MEMORY_LIMIT_MB" -le 448 ]; then
                WPE_MEMORY_PROFILE=conservative
            else
                WPE_MEMORY_PROFILE=balanced
            fi
        fi
        WPE_WEB_PROCESS_MEMORY_LIMIT_SOURCE="explicit"
    elif [ "$WPE_MEMORY_TOTAL_KB" -ge 1572864 ] \
        && [ "$WPE_MEMORY_AVAILABLE_KB" -ge 393216 ]; then
        WPE_MEMORY_PROFILE=large
        WPE_WEB_PROCESS_MEMORY_LIMIT_MB=640
        WPE_WEB_PROCESS_MEMORY_LIMIT_SOURCE=adaptive-large
    elif [ "$WPE_MEMORY_TOTAL_KB" -ge 921600 ] \
        && [ "$WPE_MEMORY_AVAILABLE_KB" -ge 196608 ] \
        && [ "$WPE_MEMORY_SWAP_TOTAL_KB" -ge 524288 ] \
        && [ "$WPE_MEMORY_SWAP_FREE_KB" -ge 262144 ]; then
        WPE_MEMORY_PROFILE=balanced
        WPE_WEB_PROCESS_MEMORY_LIMIT_MB=512
        WPE_WEB_PROCESS_MEMORY_LIMIT_SOURCE=adaptive-balanced
    else
        WPE_MEMORY_PROFILE=conservative
        WPE_WEB_PROCESS_MEMORY_LIMIT_MB=448
        WPE_WEB_PROCESS_MEMORY_LIMIT_SOURCE=adaptive-conservative
    fi

    case "$WPE_MEMORY_PROFILE" in
        large)
            profile_conservative=45
            profile_strict=65
            profile_kill=90
            profile_poll=5
            profile_warning=85
            profile_critical=93
            profile_warning_available=256
            profile_critical_available=128
            profile_mse='V:40M,A:8M'
            ;;
        balanced)
            profile_conservative=40
            profile_strict=58
            # Let the standard/strict pressure handlers reclaim first. The
            # previous 82% value killed game pages around 420MB while the 1GB
            # device still had ample swap and more than 170MB available.
            profile_kill=96
            profile_poll=2
            profile_warning=82
            profile_critical=90
            profile_warning_available=192
            profile_critical_available=96
            profile_mse='V:32M,A:6M'
            ;;
        *)
            WPE_MEMORY_PROFILE=conservative
            profile_conservative=38
            profile_strict=55
            profile_kill=80
            profile_poll=2
            profile_warning=80
            profile_critical=88
            profile_warning_available=192
            profile_critical_available=96
            profile_mse='V:24M,A:4M'
            ;;
    esac

    export WPE_MEMORY_PROFILE
    export WPE_MEMORY_TOTAL_KB WPE_MEMORY_AVAILABLE_KB
    export WPE_MEMORY_SWAP_TOTAL_KB WPE_MEMORY_SWAP_FREE_KB
    export WPE_WEB_PROCESS_MEMORY_LIMIT_MB WPE_WEB_PROCESS_MEMORY_LIMIT_SOURCE
    export WPE_WEB_PROCESS_MEMORY_CONSERVATIVE_PERCENT="${WPE_WEB_PROCESS_MEMORY_CONSERVATIVE_PERCENT:-$profile_conservative}"
    export WPE_WEB_PROCESS_MEMORY_STRICT_PERCENT="${WPE_WEB_PROCESS_MEMORY_STRICT_PERCENT:-$profile_strict}"
    export WPE_WEB_PROCESS_MEMORY_KILL_PERCENT="${WPE_WEB_PROCESS_MEMORY_KILL_PERCENT:-$profile_kill}"
    export WPE_WEB_PROCESS_MEMORY_POLL_SECONDS="${WPE_WEB_PROCESS_MEMORY_POLL_SECONDS:-$profile_poll}"
    export WEBKIT_SYSTEM_MEMORY_PRESSURE_PERCENT="${WEBKIT_SYSTEM_MEMORY_PRESSURE_PERCENT:-$profile_warning}"
    export WEBKIT_SYSTEM_MEMORY_PRESSURE_CRITICAL_PERCENT="${WEBKIT_SYSTEM_MEMORY_PRESSURE_CRITICAL_PERCENT:-$profile_critical}"
    export WEBKIT_SYSTEM_MEMORY_PRESSURE_AVAILABLE_MB="${WEBKIT_SYSTEM_MEMORY_PRESSURE_AVAILABLE_MB:-$profile_warning_available}"
    export WEBKIT_SYSTEM_MEMORY_PRESSURE_CRITICAL_AVAILABLE_MB="${WEBKIT_SYSTEM_MEMORY_PRESSURE_CRITICAL_AVAILABLE_MB:-$profile_critical_available}"
    export WEBKIT_SYSTEM_MEMORY_PRESSURE_RECOVERY_SECONDS="${WEBKIT_SYSTEM_MEMORY_PRESSURE_RECOVERY_SECONDS:-10}"
    export JSC_forceRAMSize="${JSC_forceRAMSize:-$((WPE_WEB_PROCESS_MEMORY_LIMIT_MB * 1024 * 1024))}"
    export MSE_MAX_BUFFER_SIZE="${MSE_MAX_BUFFER_SIZE:-$profile_mse}"

    echo "WPE memory profile: profile=$WPE_MEMORY_PROFILE total_kb=$WPE_MEMORY_TOTAL_KB available_kb=$WPE_MEMORY_AVAILABLE_KB swap_total_kb=$WPE_MEMORY_SWAP_TOTAL_KB swap_free_kb=$WPE_MEMORY_SWAP_FREE_KB web_limit_mb=$WPE_WEB_PROCESS_MEMORY_LIMIT_MB source=$WPE_WEB_PROCESS_MEMORY_LIMIT_SOURCE process_thresholds=${WPE_WEB_PROCESS_MEMORY_CONSERVATIVE_PERCENT}/${WPE_WEB_PROCESS_MEMORY_STRICT_PERCENT}/${WPE_WEB_PROCESS_MEMORY_KILL_PERCENT} poll_s=$WPE_WEB_PROCESS_MEMORY_POLL_SECONDS system_thresholds=${WEBKIT_SYSTEM_MEMORY_PRESSURE_PERCENT}/${WEBKIT_SYSTEM_MEMORY_PRESSURE_CRITICAL_PERCENT} available_mb=${WEBKIT_SYSTEM_MEMORY_PRESSURE_AVAILABLE_MB}/${WEBKIT_SYSTEM_MEMORY_PRESSURE_CRITICAL_AVAILABLE_MB} jsc_ram=$JSC_forceRAMSize mse=$MSE_MAX_BUFFER_SIZE"
}
