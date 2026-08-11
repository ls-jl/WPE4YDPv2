#include "browser-memory-policy.h"

static unsigned used_percent(const BrowserMemorySnapshot *snapshot)
{
    if (!snapshot || !snapshot->total_kb)
        return 0;
    uint64_t available = MIN(snapshot->available_kb, snapshot->total_kb);
    return (unsigned)((snapshot->total_kb - available) * 100 / snapshot->total_kb);
}

BrowserMemoryPressureLevel browser_memory_pressure_classify(
    const BrowserMemorySnapshot *snapshot, const BrowserMemoryPolicy *policy)
{
    g_return_val_if_fail(snapshot, BROWSER_MEMORY_PRESSURE_NORMAL);
    g_return_val_if_fail(policy, BROWSER_MEMORY_PRESSURE_NORMAL);
    unsigned used = used_percent(snapshot);
    if (used >= policy->critical_percent
        || snapshot->available_kb <= (uint64_t)policy->critical_available_mb * 1024
        || (snapshot->swap_total_kb && snapshot->swap_free_kb <= 64 * 1024))
        return BROWSER_MEMORY_PRESSURE_CRITICAL;
    if (used >= policy->warning_percent
        || snapshot->available_kb <= (uint64_t)policy->warning_available_mb * 1024
        || (snapshot->swap_total_kb
            && snapshot->swap_free_kb * 10 <= snapshot->swap_total_kb * 3))
        return BROWSER_MEMORY_PRESSURE_WARNING;
    return BROWSER_MEMORY_PRESSURE_NORMAL;
}

gboolean browser_memory_governor_update(BrowserMemoryGovernor *governor,
    const BrowserMemorySnapshot *snapshot, const BrowserMemoryPolicy *policy,
    int64_t now_us)
{
    g_return_val_if_fail(governor, FALSE);
    BrowserMemoryPressureLevel desired = browser_memory_pressure_classify(snapshot, policy);
    BrowserMemoryPressureLevel previous = governor->level;
    if (desired > governor->level) {
        governor->level = desired;
        governor->recovery_since_us = 0;
    } else if (desired < governor->level) {
        gboolean safely_recovered = used_percent(snapshot) + 4 < policy->warning_percent
            && snapshot->available_kb > (uint64_t)(policy->warning_available_mb + 64) * 1024
            && (!snapshot->swap_total_kb
                || snapshot->swap_free_kb * 10 > snapshot->swap_total_kb * 4);
        if (!safely_recovered)
            governor->recovery_since_us = 0;
        else if (!governor->recovery_since_us)
            governor->recovery_since_us = now_us;
        else if (now_us - governor->recovery_since_us
                 >= (int64_t)policy->recovery_seconds * G_USEC_PER_SEC) {
            governor->level = desired;
            governor->recovery_since_us = 0;
        }
    } else
        governor->recovery_since_us = 0;
    return previous != governor->level;
}

const char *browser_memory_pressure_name(BrowserMemoryPressureLevel level)
{
    switch (level) {
    case BROWSER_MEMORY_PRESSURE_WARNING: return "warning";
    case BROWSER_MEMORY_PRESSURE_CRITICAL: return "critical";
    default: return "normal";
    }
}
