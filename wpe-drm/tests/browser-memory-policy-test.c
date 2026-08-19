#include "../browser-memory-policy.h"

static const BrowserMemoryPolicy policy = {
    .warning_percent = 82,
    .critical_percent = 90,
    .warning_available_mb = 192,
    .critical_available_mb = 96,
    .recovery_seconds = 10,
};

static BrowserMemorySnapshot snapshot(uint64_t available_kb,
                                      uint64_t swap_free_kb)
{
    return (BrowserMemorySnapshot) {
        .total_kb = 1024 * 1024,
        .available_kb = available_kb,
        .swap_total_kb = 512 * 1024,
        .swap_free_kb = swap_free_kb,
    };
}

static void test_classification(void)
{
    BrowserMemorySnapshot normal = snapshot(320 * 1024, 400 * 1024);
    BrowserMemorySnapshot warning = snapshot(180 * 1024, 300 * 1024);
    BrowserMemorySnapshot critical = snapshot(90 * 1024, 200 * 1024);
    BrowserMemorySnapshot swap_critical = snapshot(300 * 1024, 60 * 1024);
    g_assert_cmpint(browser_memory_pressure_classify(&normal, &policy), ==,
                    BROWSER_MEMORY_PRESSURE_NORMAL);
    g_assert_cmpint(browser_memory_pressure_classify(&warning, &policy), ==,
                    BROWSER_MEMORY_PRESSURE_WARNING);
    g_assert_cmpint(browser_memory_pressure_classify(&critical, &policy), ==,
                    BROWSER_MEMORY_PRESSURE_CRITICAL);
    g_assert_cmpint(browser_memory_pressure_classify(&swap_critical, &policy), ==,
                    BROWSER_MEMORY_PRESSURE_CRITICAL);
}

static void test_hysteresis(void)
{
    BrowserMemoryGovernor governor = { 0 };
    BrowserMemorySnapshot warning = snapshot(180 * 1024, 300 * 1024);
    BrowserMemorySnapshot recovered = snapshot(320 * 1024, 400 * 1024);
    g_assert_true(browser_memory_governor_update(&governor, &warning, &policy, 1));
    g_assert_cmpint(governor.level, ==, BROWSER_MEMORY_PRESSURE_WARNING);
    g_assert_false(browser_memory_governor_update(&governor, &recovered, &policy, 2 * G_USEC_PER_SEC));
    g_assert_false(browser_memory_governor_update(&governor, &recovered, &policy, 11 * G_USEC_PER_SEC));
    g_assert_true(browser_memory_governor_update(&governor, &recovered, &policy, 12 * G_USEC_PER_SEC));
    g_assert_cmpint(governor.level, ==, BROWSER_MEMORY_PRESSURE_NORMAL);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/browser/memory/classification", test_classification);
    g_test_add_func("/browser/memory/hysteresis", test_hysteresis);
    return g_test_run();
}
