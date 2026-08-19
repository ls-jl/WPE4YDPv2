#pragma once

#include <glib.h>
#include <stdint.h>

typedef enum {
    BROWSER_MEMORY_PRESSURE_NORMAL = 0,
    BROWSER_MEMORY_PRESSURE_WARNING = 1,
    BROWSER_MEMORY_PRESSURE_CRITICAL = 2,
} BrowserMemoryPressureLevel;

typedef struct {
    uint64_t total_kb;
    uint64_t available_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
} BrowserMemorySnapshot;

typedef struct {
    unsigned warning_percent;
    unsigned critical_percent;
    unsigned warning_available_mb;
    unsigned critical_available_mb;
    unsigned recovery_seconds;
} BrowserMemoryPolicy;

typedef struct {
    BrowserMemoryPressureLevel level;
    int64_t recovery_since_us;
} BrowserMemoryGovernor;

BrowserMemoryPressureLevel browser_memory_pressure_classify(
    const BrowserMemorySnapshot *snapshot, const BrowserMemoryPolicy *policy);
gboolean browser_memory_governor_update(BrowserMemoryGovernor *governor,
    const BrowserMemorySnapshot *snapshot, const BrowserMemoryPolicy *policy,
    int64_t now_us);
const char *browser_memory_pressure_name(BrowserMemoryPressureLevel level);
