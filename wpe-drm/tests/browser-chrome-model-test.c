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
    g_assert_cmpuint(chrome_panel_default_line_count(
                         CHROME_PANEL_APPEARANCE), ==, 6);
    g_assert_cmpint(chrome_panel_parent(CHROME_PANEL_ROTATION), ==,
                    CHROME_PANEL_MENU);
    g_assert_cmpuint(chrome_panel_default_line_count(
                         CHROME_PANEL_ROTATION), ==, 4);
    g_assert_false(chrome_panel_is_overflow_stack(CHROME_PANEL_TABS));
    g_assert_true(chrome_panel_is_internal_list(CHROME_PANEL_HISTORY));
}

static void test_toolbar_geometry(void)
{
    BrowserChromeToolbarGeometry landscape =
        browser_chrome_toolbar_geometry(936, 280, 44, 80, 5);
    g_assert_false(landscape.stacked);
    g_assert_cmpint(landscape.height, ==, 44);
    g_assert_cmpint(landscape.address_y, ==, landscape.controls_y);
    g_assert_cmpint(landscape.controls_x + landscape.controls_width, <=, 936);

    BrowserChromeToolbarGeometry portrait =
        browser_chrome_toolbar_geometry(280, 936, 44, 80, 5);
    g_assert_true(portrait.stacked);
    g_assert_cmpint(portrait.height, ==, 80);
    g_assert_cmpint(portrait.address_x, >=, 0);
    g_assert_cmpint(portrait.address_x + portrait.address_width, <=, 280);
    g_assert_cmpint(portrait.controls_x, >=, 0);
    g_assert_cmpint(portrait.controls_x + portrait.controls_width, <=, 280);
    g_assert_cmpint(portrait.address_y + portrait.address_height,
                    <, portrait.controls_y);
    for (int index = 0; index < 5; ++index) {
        double center_x = portrait.controls_x
            + portrait.controls_width * (index + 0.5) / 5.0;
        double center_y = portrait.controls_y
            + portrait.controls_height / 2.0;
        g_assert_cmpint(browser_chrome_toolbar_button_at(
                            &portrait, center_x, center_y), ==, index);
    }
    g_assert_cmpint(browser_chrome_toolbar_button_at(
                        &portrait, 140, portrait.address_y + 10), ==, -1);
}

static void test_menu_grid_geometry(void)
{
    BrowserChromeMenuGridGeometry landscape =
        browser_chrome_menu_grid_geometry(960, 266, 7);
    g_assert_false(landscape.portrait);
    g_assert_cmpint(landscape.columns, ==, 3);
    g_assert_cmpint(landscape.rows, ==, 2);
    g_assert_cmpint(landscape.items_per_page, ==, 6);
    g_assert_cmpint(landscape.page_count, ==, 2);

    const int portrait_sizes[][2] = {
        { 266, 960 },
        { 568, 1210 },
    };
    for (guint index = 0; index < G_N_ELEMENTS(portrait_sizes); ++index) {
        BrowserChromeMenuGridGeometry portrait =
            browser_chrome_menu_grid_geometry(portrait_sizes[index][0],
                                               portrait_sizes[index][1], 7);
        g_assert_true(portrait.portrait);
        g_assert_cmpint(portrait.columns, ==, 2);
        g_assert_cmpint(portrait.rows, ==, 3);
        g_assert_cmpint(portrait.items_per_page, ==, 6);
        g_assert_cmpint(portrait.page_count, ==, 2);
    }

    BrowserChromeMenuGridGeometry one_page =
        browser_chrome_menu_grid_geometry(280, 936, 6);
    g_assert_cmpint(one_page.page_count, ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/browser/chrome/panel-definitions", test_panel_definitions);
    g_test_add_func("/browser/chrome/toolbar-geometry", test_toolbar_geometry);
    g_test_add_func("/browser/chrome/menu-grid-geometry", test_menu_grid_geometry);
    return g_test_run();
}
