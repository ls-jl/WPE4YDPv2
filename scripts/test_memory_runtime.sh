#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODULE="$ROOT/wpe-drm/runtime/memory-runtime.sh"
TMP="${TMPDIR:-/tmp}/wpe-memory-runtime-test.$$"
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP"

write_meminfo() {
    cat >"$TMP/meminfo" <<EOF
MemTotal:       $1 kB
MemAvailable:   $2 kB
SwapTotal:      $3 kB
SwapFree:       $4 kB
EOF
}

run_case() {
    expected_profile="$1"
    expected_limit="$2"
    expected_mse="$3"
    expected_kill="$4"
    shift 4
    result=$(env -i PATH="$PATH" WPE_MEMORY_MEMINFO="$TMP/meminfo" "$@" sh -c '
        . "$1"
        memory_runtime_init >/dev/null
        printf "%s|%s|%s|%s|%s|%s\n" "$WPE_MEMORY_PROFILE" \
            "$WPE_WEB_PROCESS_MEMORY_LIMIT_MB" "$MSE_MAX_BUFFER_SIZE" \
            "$JSC_forceRAMSize" "$WEBKIT_SYSTEM_MEMORY_PRESSURE_PERCENT" \
            "$WPE_WEB_PROCESS_MEMORY_KILL_PERCENT"
    ' sh "$MODULE")
    expected_jsc=$((expected_limit * 1024 * 1024))
    case "$result" in
        "$expected_profile|$expected_limit|$expected_mse|$expected_jsc|"*"|$expected_kill") ;;
        *) echo "memory runtime fixture failed: expected=$expected_profile/$expected_limit/$expected_mse result=$result" >&2; exit 1 ;;
    esac
}

write_meminfo 2097152 786432 524288 524288
run_case large 640 'V:40M,A:8M' 90

write_meminfo 1024000 307200 524288 393216
run_case balanced 512 'V:32M,A:6M' 96

write_meminfo 1024000 307200 524284 393216
run_case balanced 512 'V:32M,A:6M' 96

write_meminfo 1024000 131072 524288 65536
run_case conservative 448 'V:24M,A:4M' 80

write_meminfo 1024000 454888 524284 254460
run_case balanced 512 'V:32M,A:6M' 96

write_meminfo 1024000 307200 524284 254460
run_case conservative 448 'V:24M,A:4M' 80

write_meminfo 1024000 307200 524288 393216
run_case conservative 400 'V:24M,A:4M' 80 WPE_WEB_PROCESS_MEMORY_LIMIT_MB=400

echo 'memory runtime fixtures passed: 7'

for name in WPE_CLOUD_AUTOSTART WPE_CLOUDGAME_ALLOW_EXPIRED_TLS; do
    assignment=$(awk -v name="$name" '$0 ~ "^export " name "=" { print; exit }' "$ROOT/wpe-drm/run.sh")
    [ -n "$assignment" ] || exit 1
    actual=$(env -i PATH="$PATH" sh -c "$assignment; printenv $name")
    [ "$actual" = 0 ] || { echo "unsafe default: $name=$actual" >&2; exit 1; }
done
if grep -Eq 'amixer.*unmute' "$ROOT/wpe-drm/run.sh"; then
    echo 'browser must not override system mute' >&2
    exit 1
fi
if grep -Eq '/proc/sys/vm/(drop_caches|swappiness)' "$ROOT/wpe-drm/run.sh"; then
    echo 'production supervisor must not change system VM settings' >&2
    exit 1
fi
echo 'runtime safety default fixtures passed'
