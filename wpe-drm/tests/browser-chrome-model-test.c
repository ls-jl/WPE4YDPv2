#include "../browser-chrome-model.h"

static void test_panel_definitions(void)
{
    for (int value = CHROME_PANEL_NONE; value < CHROME_PANEL_COUNT; ++value) {
        ChromePanel panel = (ChromePanel)value;
        const ChromePanelDefinition *definition = chrome_panel_definition(panel);
        g_assert_nonnull(definition);
        g_assert_nonnull(definition->name);
        g_assert_cmpint(chrome_panel_from_name(definition->name), ==, panel);
        g_assert_cmpint(chrome_panel_parent(panel), ==, definition->parent);
        g_assert_cmpint(chrome_panel_is_overflow_stack(panel), ==,
                        definition->overflow_stack);
        g_assert_cmpint(chrome_panel_is_internal_list(panel), ==,
                        definition->internal_list);
        g_assert_cmpuint(chrome_panel_default_line_count(panel), ==,
                         definition->default_line_count);
    }

    g_assert_cmpint(chrome_panel_from_name("unknown"), ==, CHROME_PANEL_NONE);
    g_assert_cmpint(chrome_panel_parent(CHROME_PANEL_APPEARANCE), ==,
                    CHROME_PANEL_SETTINGS);
    g_assert_false(chrome_panel_is_overflow_stack(CHROME_PANEL_TABS));
    g_assert_true(chrome_panel_is_internal_list(CHROME_PANEL_HISTORY));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/browser/chrome/panel-definitions", test_panel_definitions);
    return g_test_run();
}
