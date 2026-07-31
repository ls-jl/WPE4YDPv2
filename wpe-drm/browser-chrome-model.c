#include "browser-chrome-model.h"

static const ChromePanelDefinition panel_definitions[CHROME_PANEL_COUNT] = {
    [CHROME_PANEL_NONE] = { "none", CHROME_PANEL_NONE, 0, FALSE, FALSE },
    [CHROME_PANEL_TABS] = { "tabs", CHROME_PANEL_NONE, 0, FALSE, FALSE },
    [CHROME_PANEL_MENU] = { "menu", CHROME_PANEL_NONE, 0, TRUE, FALSE },
    [CHROME_PANEL_SETTINGS] = { "settings", CHROME_PANEL_MENU, 5, TRUE, TRUE },
    [CHROME_PANEL_APPEARANCE] = { "appearance", CHROME_PANEL_SETTINGS, 4, TRUE, TRUE },
    [CHROME_PANEL_WEB] = { "web", CHROME_PANEL_SETTINGS, 5, TRUE, TRUE },
    [CHROME_PANEL_STARTUP] = { "startup & search", CHROME_PANEL_SETTINGS, 4, TRUE, TRUE },
    [CHROME_PANEL_SETTINGS_PRIVACY] = { "privacy & data", CHROME_PANEL_SETTINGS, 4, TRUE, TRUE },
    [CHROME_PANEL_ABOUT] = { "about", CHROME_PANEL_SETTINGS, 5, TRUE, TRUE },
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
