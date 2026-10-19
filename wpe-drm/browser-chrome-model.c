#include "browser-chrome-model.h"

ChromeTabsGeometry browser_chrome_tabs_geometry(int width, int height,
    int toolbar_height, int tab_count, int row_height)
{
    int panel_width = MAX(1, width / 2);
    ChromeTabsGeometry geometry = {
        .panel_x = width - panel_width, .panel_y = toolbar_height,
        .panel_width = panel_width, .panel_height = MAX(0, height - toolbar_height),
        .list_top = toolbar_height, .list_height = MAX(0, height - toolbar_height),
        .footer_top = height, .row_height = row_height,
        .max_scroll = MAX(0, tab_count * row_height - MAX(0, height - toolbar_height)),
    };
    return geometry;
}

ChromeMenuGeometry browser_chrome_menu_geometry(int width, int height,
    int toolbar_height, int item_count)
{
    BrowserChromeMenuGridGeometry grid = browser_chrome_menu_grid_geometry(width, height, item_count);
    ChromeMenuGeometry geometry = {
        .panel_x = width / 3, .panel_y = toolbar_height,
        .panel_width = width - width / 3, .panel_height = MAX(0, height - toolbar_height),
        .columns = grid.columns, .rows = grid.rows, .items_per_page = grid.items_per_page,
        .item_count = item_count, .page_count = grid.page_count,
        .max_scroll = MAX(0, grid.page_count - 1) * (width - width / 3),
    };
    return geometry;
}

ChromeListGeometry browser_chrome_list_geometry(int width, int height,
    int toolbar_height, int line_count, int row_height, int header_height)
{
    ChromeListGeometry geometry = {
        .panel_x = 0, .panel_y = toolbar_height, .panel_width = width,
        .panel_height = MAX(0, height - toolbar_height),
        .header_height = header_height, .row_height = row_height,
        .list_top = toolbar_height + header_height,
        .list_height = MAX(0, height - toolbar_height - header_height),
        .line_count = line_count,
        .max_scroll = MAX(0, line_count * row_height - MAX(0, height - toolbar_height - header_height)),
    };
    return geometry;
}

static const ChromePanelDefinition panel_definitions[CHROME_PANEL_COUNT] = {
    [CHROME_PANEL_NONE] = { "none", CHROME_PANEL_NONE, 0, FALSE, FALSE },
    [CHROME_PANEL_TABS] = { "tabs", CHROME_PANEL_NONE, 0, FALSE, FALSE },
    [CHROME_PANEL_MENU] = { "menu", CHROME_PANEL_NONE, 0, TRUE, FALSE },
    [CHROME_PANEL_SETTINGS] = { "settings", CHROME_PANEL_MENU, 6, TRUE, TRUE },
    [CHROME_PANEL_LANGUAGE] = { "language", CHROME_PANEL_SETTINGS, 2, TRUE, TRUE },
    [CHROME_PANEL_ROTATION] = { "rotation", CHROME_PANEL_MENU, 4, TRUE, TRUE },
    [CHROME_PANEL_APPEARANCE] = { "appearance", CHROME_PANEL_SETTINGS, 6, TRUE, TRUE },
    [CHROME_PANEL_WEB] = { "web", CHROME_PANEL_SETTINGS, 5, TRUE, TRUE },
    [CHROME_PANEL_STARTUP] = { "startup & search", CHROME_PANEL_SETTINGS, 4, TRUE, TRUE },
    [CHROME_PANEL_SETTINGS_PRIVACY] = { "privacy & data", CHROME_PANEL_SETTINGS, 4, TRUE, TRUE },
    [CHROME_PANEL_ABOUT] = { "about", CHROME_PANEL_SETTINGS, 6, TRUE, TRUE },
    [CHROME_PANEL_PRIVACY] = { "privacy & cookies", CHROME_PANEL_MENU, 2, TRUE, TRUE },
    [CHROME_PANEL_CLEAR_SITE_DATA] = { "clear site data?", CHROME_PANEL_PRIVACY, 2, TRUE, TRUE },
    [CHROME_PANEL_PROFILES] = { "profiles", CHROME_PANEL_MENU, 6, TRUE, TRUE },
    [CHROME_PANEL_PROFILE_MANAGE] = { "profile", CHROME_PANEL_PROFILES, 2, TRUE, TRUE },
    [CHROME_PANEL_PROFILE_DELETE] = { "delete profile?", CHROME_PANEL_PROFILE_MANAGE, 2, TRUE, TRUE },
    [CHROME_PANEL_HISTORY] = { "history", CHROME_PANEL_MENU, 8, TRUE, TRUE },
    [CHROME_PANEL_BOOKMARKS] = { "bookmarks", CHROME_PANEL_MENU, 8, TRUE, TRUE },
    [CHROME_PANEL_CLEAR_HISTORY] = { "clear history?", CHROME_PANEL_HISTORY, 2, TRUE, TRUE },
};

const ChromePanelDefinition *chrome_panel_definition(ChromePanel panel)
{
    if (panel < CHROME_PANEL_NONE || panel >= CHROME_PANEL_COUNT)
        return &panel_definitions[CHROME_PANEL_NONE];
    return &panel_definitions[panel];
}

const char *chrome_panel_name(ChromePanel panel)
{
    return chrome_panel_definition(panel)->name;
}

ChromePanel chrome_panel_from_name(const char *name)
{
    if (!name)
        return CHROME_PANEL_NONE;
    for (int panel = CHROME_PANEL_NONE; panel < CHROME_PANEL_COUNT; ++panel) {
        if (!g_ascii_strcasecmp(name, panel_definitions[panel].name))
            return (ChromePanel)panel;
    }
    return CHROME_PANEL_NONE;
}

ChromePanel chrome_panel_parent(ChromePanel panel)
{
    return chrome_panel_definition(panel)->parent;
}

gboolean chrome_panel_is_overflow_stack(ChromePanel panel)
{
    return chrome_panel_definition(panel)->overflow_stack;
}

gboolean chrome_panel_is_internal_list(ChromePanel panel)
{
    return chrome_panel_definition(panel)->internal_list;
}

guint chrome_panel_default_line_count(ChromePanel panel)
{
    return chrome_panel_definition(panel)->default_line_count;
}

BrowserChromeToolbarGeometry browser_chrome_toolbar_geometry(
    int panel_width, int panel_height, int landscape_height,
    int portrait_height, int button_count)
{
    panel_width = MAX(1, panel_width);
    panel_height = MAX(1, panel_height);
    landscape_height = CLAMP(landscape_height, 24, 80);
    portrait_height = CLAMP(portrait_height, 56, 80);
    button_count = MAX(1, button_count);

    BrowserChromeToolbarGeometry geometry = {
        .stacked = panel_width < panel_height,
        .height = panel_width < panel_height
            ? portrait_height : landscape_height,
        .button_count = button_count,
    };

    if (geometry.stacked) {
        const int margin = 8;
        const int row_gap = 4;
        const int available_height = MAX(40, geometry.height - 12 - row_gap);
        const int address_height = available_height / 2;
        geometry.address_x = margin;
        geometry.address_y = 6;
        geometry.address_width = MAX(1, panel_width - margin * 2);
        geometry.address_height = address_height;
        geometry.controls_x = margin;
        geometry.controls_y = geometry.address_y + address_height + row_gap;
        geometry.controls_width = MAX(1, panel_width - margin * 2);
        geometry.controls_height = MAX(1, geometry.height - 6 - geometry.controls_y);
        return geometry;
    }

    const int legacy_button_x = panel_width / 2 + 8;
    const int button_width = MAX(36,
        (panel_width - legacy_button_x - 8) / 6);
    geometry.controls_x = legacy_button_x + button_width;
    geometry.controls_y = 6;
    geometry.controls_width = button_width * button_count;
    geometry.controls_height = MAX(1, geometry.height - 12);
    geometry.address_x = 8;
    geometry.address_y = 6;
    geometry.address_width = MAX(1, geometry.controls_x - 16);
    geometry.address_height = geometry.controls_height;
    return geometry;
}

int browser_chrome_toolbar_button_at(
    const BrowserChromeToolbarGeometry *geometry, double x, double y)
{
    if (!geometry || geometry->controls_width <= 0
        || geometry->controls_height <= 0 || geometry->button_count <= 0
        || x < geometry->controls_x
        || x >= geometry->controls_x + geometry->controls_width
        || y < geometry->controls_y
        || y >= geometry->controls_y + geometry->controls_height)
        return -1;
    int index = (int)((x - geometry->controls_x)
        * geometry->button_count / geometry->controls_width);
    return CLAMP(index, 0, geometry->button_count - 1);
}

BrowserChromeMenuGridGeometry browser_chrome_menu_grid_geometry(
    int panel_width, int panel_height, int item_count)
{
    panel_width = MAX(1, panel_width);
    panel_height = MAX(1, panel_height);
    item_count = MAX(0, item_count);

    BrowserChromeMenuGridGeometry geometry = {
        .portrait = panel_width < panel_height,
        .columns = panel_width < panel_height ? 2 : 3,
        .rows = panel_width < panel_height ? 3 : 2,
    };
    geometry.items_per_page = geometry.columns * geometry.rows;
    geometry.page_count = MAX(1,
        (item_count + geometry.items_per_page - 1) / geometry.items_per_page);
    return geometry;
}
