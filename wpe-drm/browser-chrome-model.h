#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    CHROME_PANEL_NONE = 0,
    CHROME_PANEL_TABS,
    CHROME_PANEL_MENU,
    CHROME_PANEL_SETTINGS,
    CHROME_PANEL_LANGUAGE,
    CHROME_PANEL_APPEARANCE,
    CHROME_PANEL_WEB,
    CHROME_PANEL_STARTUP,
    CHROME_PANEL_SETTINGS_PRIVACY,
    CHROME_PANEL_ABOUT,
    CHROME_PANEL_PRIVACY,
    CHROME_PANEL_CLEAR_SITE_DATA,
    CHROME_PANEL_PROFILES,
    CHROME_PANEL_PROFILE_MANAGE,
    CHROME_PANEL_PROFILE_DELETE,
    CHROME_PANEL_HISTORY,
    CHROME_PANEL_BOOKMARKS,
    CHROME_PANEL_CLEAR_HISTORY,
    CHROME_PANEL_COUNT
} ChromePanel;

typedef struct {
    const char *name;
    ChromePanel parent;
    guint default_line_count;
    gboolean overflow_stack;
    gboolean internal_list;
} ChromePanelDefinition;

typedef struct {
    gboolean stacked;
    int height;
    int address_x;
    int address_y;
    int address_width;
    int address_height;
    int controls_x;
    int controls_y;
    int controls_width;
    int controls_height;
    int button_count;
} BrowserChromeToolbarGeometry;

const ChromePanelDefinition *chrome_panel_definition(ChromePanel panel);
const char *chrome_panel_name(ChromePanel panel);
ChromePanel chrome_panel_from_name(const char *name);
ChromePanel chrome_panel_parent(ChromePanel panel);
gboolean chrome_panel_is_overflow_stack(ChromePanel panel);
gboolean chrome_panel_is_internal_list(ChromePanel panel);
guint chrome_panel_default_line_count(ChromePanel panel);
BrowserChromeToolbarGeometry browser_chrome_toolbar_geometry(
    int panel_width, int panel_height, int landscape_height,
    int portrait_height, int button_count);
int browser_chrome_toolbar_button_at(
    const BrowserChromeToolbarGeometry *geometry, double x, double y);

G_END_DECLS
