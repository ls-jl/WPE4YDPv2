/*
 * wpe-drm-minimal.c — Minimal WPE DRM launcher for dictionary pen
 *
 * Architecture:
 *   HTML → WebKitWebView → WPEView → WPEViewDRM (built-in DRM scanout) → screen
 *
 * No Cog. No WPEBackend-fdo. No Wayland. No libwpe.
 * WPEPlatform API handles buffer delivery and DRM page-flip internally.
 *
 * Build:
 *   gcc -O2 -o wpe-drm-minimal wpe-drm-minimal.c \
 *       $(pkg-config --cflags --libs wpe-webkit-2.0)
 */

#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/drm/wpe-drm.h>
#include "browser-profile-store.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <linux/input.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static GMainLoop *main_loop = NULL;
static guint64 frame_count = 0;
static gint64 frame_stats_window_start_us = 0;
static guint64 frame_stats_window_start_count = 0;
static GHashTable *tls_exception_hosts = NULL;
static gboolean gpu_render_profile = FALSE;
static int runtime_exit_code = 0;
static guint gpu_first_frame_timeout_source_id = 0;

typedef struct {
    BrowserProfileStore *store;
    BrowserProfile profile;
    gboolean guest;
    char *root;
    char *runtime;
    char *data;
    char *cache;
    char *cookie_jar;
} BrowserProfileRuntime;

static BrowserProfileRuntime profile_runtime;

static void load_uri_preserving_local_html(WebKitWebView *web_view, const char *uri);

#define MAX_TOUCH_SLOTS 10
#define MAX_BROWSER_TABS 8
#define MAX_BROWSER_HISTORY 16
#define BROWSER_URL_MAX 512
#define BROWSER_TITLE_MAX 160
#define CHROME_TOOLBAR_MARGIN 8
#define CHROME_TOOLBAR_BUTTON_COUNT 5
#define CHROME_TOOLBAR_LEGACY_BUTTON_COUNT 6
#define CHROME_TABS_HEADER_HEIGHT 28
#define CHROME_TABS_FOOTER_HEIGHT 40
#define CHROME_TABS_ROW_HEIGHT 34
#define CHROME_TABS_CLOSE_HIT_WIDTH 42

typedef enum {
    CHROME_PANEL_NONE = 0,
    CHROME_PANEL_TABS,
    CHROME_PANEL_MENU,
    CHROME_PANEL_SETTINGS,
    CHROME_PANEL_PRIVACY,
    CHROME_PANEL_CLEAR_SITE_DATA,
    CHROME_PANEL_PROFILES,
    CHROME_PANEL_PROFILE_MANAGE,
    CHROME_PANEL_PROFILE_DELETE,
    CHROME_PANEL_HISTORY,
    CHROME_PANEL_BOOKMARKS,
    CHROME_PANEL_CLEAR_HISTORY
} ChromePanel;

typedef struct {
    gboolean active;
    gboolean just_down;
    gboolean just_up;
    int tracking_id;
    int raw_x;
    int raw_y;
    double x;
    double y;
    double last_x;
    double last_y;
    double down_x;
    double down_y;
    double max_move_sq;
    gboolean scrolling;
    gboolean chrome_consumed;
    gboolean tabs_scroll_candidate;
} TouchSlot;

typedef struct {
    guint id;
    char url[BROWSER_URL_MAX];
    char title[BROWSER_TITLE_MAX];
    char *back[MAX_BROWSER_HISTORY];
    int back_count;
    char *forward[MAX_BROWSER_HISTORY];
    int forward_count;
} BrowserTab;

typedef struct {
    gboolean enabled;
    gboolean visible;
    gboolean loading;
    double load_progress;
    gboolean touch_debug;
    int height;
    double hide_down_px;
    double show_up_px;
    double show_up_min_velocity_px_s;
    gint64 transition_us;
    char *render_state_path;
    char *home_url;
    BrowserTab tabs[MAX_BROWSER_TABS];
    int tab_count;
    int active;
    guint next_tab_id;
    ChromePanel panel;
    double scroll_accum;
    double scroll_velocity_peak;
    guint32 scroll_last_time_ms;
    int scroll_direction;
    gboolean suppress_history;
    guint layout_timer;
    char site_profile[16];
    BrowserCookiePolicy cookie_policy;
    guint panel_page;
    gboolean panel_delete_mode;
    gboolean site_data_clearing;
    char site_data_status[64];
    GPtrArray *panel_profiles;
    GPtrArray *panel_pages;
    int64_t current_visit_id;
    double tabs_scroll_offset;
    gint64 tabs_scroll_last_publish_us;
} BrowserChrome;

typedef struct {
    WPEView *view;
    WPEToplevel *toplevel;
    WebKitWebView *web_view;
    WebKitNetworkSession *network_session;
    int viewport_width;
    int viewport_height;
    int panel_width;
    int panel_height;
    int panel_rotation;
    int touch_rotation;
    int touch_fd;
    guint touch_source_id;
    guint64 touch_event_count;
    int current_slot;
    int abs_x_code;
    int abs_y_code;
    int abs_x_min;
    int abs_x_max;
    int abs_y_min;
    int abs_y_max;
    gboolean touch_active_x_enabled;
    int touch_active_x_min;
    int touch_active_x_max;
    gboolean touch_active_y_enabled;
    int touch_active_y_min;
    int touch_active_y_max;
    gboolean touch_swap_xy;
    gboolean touch_invert_x;
    gboolean touch_invert_y;
    int touch_offset_x;
    int touch_offset_y;
    gboolean touch_scroll_fallback;
    gboolean touch_native_scroll_fallback;
    gboolean touch_js_scroll_fallback;
    gboolean touch_horizontal_scroll;
    gboolean touch_scroll_invert_y;
    gboolean send_touch_events;
    gboolean synthesize_pointer_tap;
    char input_profile[16];
    gboolean game_input_active;
    gboolean native_scroll_scheduled;
    guint native_scroll_source_id;
    gboolean scroll_active;
    guint scroll_stop_source_id;
    double touch_scroll_scale;
    double touch_scroll_max_step;
    double touch_scroll_pending_limit;
    double touch_tap_max_move;
    int touch_scroll_interval_ms;
    int touch_scroll_stop_delay_ms;
    double pending_native_scroll_x;
    double pending_native_scroll_y;
    double last_scroll_x;
    double last_scroll_y;
    guint32 last_scroll_time_ms;
    guint64 scroll_event_count;
    guint64 scroll_drop_count;
    gint64 scroll_stats_last_us;
    guint64 scroll_stats_last_event_count;
    guint64 scroll_stats_last_frame_count;
    char *keyboard_dir;
    guint keyboard_response_source_id;
    guint64 keyboard_next_id;
    char *keyboard_active_id;
    char *keyboard_active_kind;
    char *keyboard_pending_text;
    gboolean keyboard_pending_text_valid;
    guint64 keyboard_last_sequence;
    gboolean keyboard_apply_in_flight;
    char *keyboard_queued_text;
    guint64 keyboard_queued_sequence;
    gboolean keyboard_queued_commit;
    char *keyboard_sent_text;
    guint64 keyboard_sent_sequence;
    gboolean keyboard_sent_commit;
    gboolean keyboard_terminal_pending;
    WebKitScriptMessageReply *keyboard_frame_reply;
    JSCContext *keyboard_frame_context;
    gint64 keyboard_active_us;
    gint64 last_pointer_tap_us;
    BrowserChrome chrome;
    TouchSlot slots[MAX_TOUCH_SLOTS];
} AppState;

static WebKitCookieAcceptPolicy webkit_cookie_policy(BrowserCookiePolicy policy);
static void chrome_switch_profile(AppState *state, int64_t profile_id, gboolean guest);

static gboolean env_enabled(const char *name, gboolean default_value)
{
    const char *value = g_getenv(name);
    if (!value || !value[0])
        return default_value;
    if (!g_ascii_strcasecmp(value, "0") || !g_ascii_strcasecmp(value, "false") ||
        !g_ascii_strcasecmp(value, "off") || !g_ascii_strcasecmp(value, "no"))
        return FALSE;
    return TRUE;
}

static gboolean parse_viewport_string(const char *value, int *width, int *height)
{
    if (!value)
        return FALSE;

    while (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r')
        value++;
    if (!*value)
        return FALSE;

    int parsed_width = 0;
    int parsed_height = 0;
    char separator = 0;
    if (sscanf(value, "%d%c%d", &parsed_width, &separator, &parsed_height) != 3)
        return FALSE;
    if (separator != 'x' && separator != 'X' && separator != ',')
        return FALSE;
    if (parsed_width < 64 || parsed_height < 64 || parsed_width > 4096 || parsed_height > 4096)
        return FALSE;

    *width = parsed_width;
    *height = parsed_height;
    return TRUE;
}

static double env_double(const char *name, double default_value)
{
    const char *value = g_getenv(name);
    if (!value || !value[0])
        return default_value;
    char *end = NULL;
    double result = g_ascii_strtod(value, &end);
    if (end == value)
        return default_value;
    return result;
}

static gboolean chrome_layout_resize_enabled(void)
{
    const char *value = g_getenv("WPE_CHROME_LAYOUT");
    if (!value || !value[0])
        return FALSE;
    if (!g_ascii_strcasecmp(value, "resize"))
        return TRUE;
    if (!g_ascii_strcasecmp(value, "inset") ||
        !g_ascii_strcasecmp(value, "visual-inset") ||
        !g_ascii_strcasecmp(value, "overlay"))
        return FALSE;
    return env_enabled("WPE_CHROME_LAYOUT", TRUE);
}

static gboolean parse_int_range(const char *value, int *minimum, int *maximum)
{
    if (!value || !value[0] || !minimum || !maximum)
        return FALSE;

    char *end = NULL;
    long first = strtol(value, &end, 10);
    if (end == value)
        return FALSE;
    while (*end == ' ' || *end == '\t')
        end++;
    if (end[0] == '.' && end[1] == '.')
        end += 2;
    else if (*end == ':' || *end == ',' || *end == 'x' || *end == 'X')
        end++;
    else
        return FALSE;
    while (*end == ' ' || *end == '\t')
        end++;

    char *end2 = NULL;
    long second = strtol(end, &end2, 10);
    if (end2 == end || first == second)
        return FALSE;
    if (first > second) {
        long tmp = first;
        first = second;
        second = tmp;
    }
    if (first < -32768 || second > 32768)
        return FALSE;

    *minimum = (int)first;
    *maximum = (int)second;
    return TRUE;
}

static int normalize_rotation_degrees(int rotation)
{
    rotation %= 360;
    if (rotation < 0)
        rotation += 360;
    if (rotation == 90 || rotation == 180 || rotation == 270)
        return rotation;
    return 0;
}

static const char *chrome_panel_name(ChromePanel panel)
{
    switch (panel) {
    case CHROME_PANEL_TABS: return "tabs";
    case CHROME_PANEL_MENU: return "menu";
    case CHROME_PANEL_SETTINGS: return "settings";
    case CHROME_PANEL_PRIVACY: return "privacy & cookies";
    case CHROME_PANEL_CLEAR_SITE_DATA: return "clear site data?";
    case CHROME_PANEL_PROFILES: return "profiles";
    case CHROME_PANEL_PROFILE_MANAGE: return "profile";
    case CHROME_PANEL_PROFILE_DELETE: return "delete profile?";
    case CHROME_PANEL_HISTORY: return "history";
    case CHROME_PANEL_BOOKMARKS: return "bookmarks";
    case CHROME_PANEL_CLEAR_HISTORY: return "clear history?";
    case CHROME_PANEL_NONE:
    default:
        return "none";
    }
}

static ChromePanel chrome_panel_from_name(const char *name)
{
    if (!name)
        return CHROME_PANEL_NONE;
    if (!g_ascii_strcasecmp(name, "tabs"))
        return CHROME_PANEL_TABS;
    if (!g_ascii_strcasecmp(name, "menu"))
        return CHROME_PANEL_MENU;
    if (!g_ascii_strcasecmp(name, "settings"))
        return CHROME_PANEL_SETTINGS;
    if (!g_ascii_strcasecmp(name, "privacy & cookies"))
        return CHROME_PANEL_PRIVACY;
    if (!g_ascii_strcasecmp(name, "profiles"))
        return CHROME_PANEL_PROFILES;
    if (!g_ascii_strcasecmp(name, "profile"))
        return CHROME_PANEL_PROFILE_MANAGE;
    if (!g_ascii_strcasecmp(name, "history"))
        return CHROME_PANEL_HISTORY;
    if (!g_ascii_strcasecmp(name, "bookmarks"))
        return CHROME_PANEL_BOOKMARKS;
    return CHROME_PANEL_NONE;
}

static ChromePanel chrome_panel_parent(ChromePanel panel)
{
    switch (panel) {
    case CHROME_PANEL_SETTINGS:
    case CHROME_PANEL_PRIVACY:
    case CHROME_PANEL_PROFILES:
    case CHROME_PANEL_HISTORY:
    case CHROME_PANEL_BOOKMARKS:
        return CHROME_PANEL_MENU;
    case CHROME_PANEL_PROFILE_MANAGE:
        return CHROME_PANEL_PROFILES;
    case CHROME_PANEL_PROFILE_DELETE:
        return CHROME_PANEL_PROFILE_MANAGE;
    case CHROME_PANEL_CLEAR_HISTORY:
        return CHROME_PANEL_HISTORY;
    case CHROME_PANEL_CLEAR_SITE_DATA:
        return CHROME_PANEL_PRIVACY;
    case CHROME_PANEL_TABS:
    case CHROME_PANEL_MENU:
    case CHROME_PANEL_NONE:
    default:
        return CHROME_PANEL_NONE;
    }
}

static BrowserTab *chrome_active_tab(BrowserChrome *chrome)
{
    if (!chrome || chrome->tab_count <= 0 || chrome->active < 0 || chrome->active >= chrome->tab_count)
        return NULL;
    return &chrome->tabs[chrome->active];
}

static void history_clear(char **items, int *count)
{
    if (!items || !count)
        return;
    for (int i = 0; i < *count; ++i)
        g_clear_pointer(&items[i], g_free);
    *count = 0;
}

static void history_push(char **items, int *count, const char *url)
{
    if (!items || !count || !url || !url[0])
        return;
    if (*count > 0 && !g_strcmp0(items[*count - 1], url))
        return;
    if (*count >= MAX_BROWSER_HISTORY) {
        g_free(items[0]);
        memmove(items, items + 1, sizeof(char *) * (MAX_BROWSER_HISTORY - 1));
        *count = MAX_BROWSER_HISTORY - 1;
    }
    items[*count] = g_strdup(url);
    (*count)++;
}

static char *history_pop(char **items, int *count)
{
    if (!items || !count || *count <= 0)
        return NULL;
    (*count)--;
    char *url = items[*count];
    items[*count] = NULL;
    return url;
}

static void chrome_clear_tab(BrowserTab *tab)
{
    if (!tab)
        return;
    history_clear(tab->back, &tab->back_count);
    history_clear(tab->forward, &tab->forward_count);
    memset(tab, 0, sizeof(*tab));
}

static const char *default_home_url(void)
{
    const char *url = g_getenv("WPE_DEFAULT_URL");
    return url && url[0] ? url : "https://m.baidu.com/";
}

static const char *site_profile_mobile_user_agent(void)
{
    return "Mozilla/5.0 (Linux; Android 12; Pixel 5) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";
}

static const char *site_profile_desktop_user_agent(void)
{
    return "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
}

static const char *normalize_site_profile(const char *profile)
{
    return profile && !g_ascii_strcasecmp(profile, "desktop") ? "desktop" : "mobile";
}

static const char *site_profile_user_agent(const char *profile)
{
    return !g_strcmp0(normalize_site_profile(profile), "desktop")
        ? site_profile_desktop_user_agent()
        : site_profile_mobile_user_agent();
}

static void chrome_apply_site_profile(AppState *state)
{
    if (!state || !state->web_view)
        return;
    const char *profile = normalize_site_profile(state->chrome.site_profile);
    g_strlcpy(state->chrome.site_profile, profile, sizeof(state->chrome.site_profile));
    WebKitSettings *settings = webkit_web_view_get_settings(state->web_view);
    const char *user_agent = site_profile_user_agent(profile);
    if (settings)
        webkit_settings_set_user_agent(settings, user_agent);
    g_print("Site profile: site_profile=%s user_agent=%s\n", profile, user_agent);
}

static void chrome_apply_layout(AppState *state, const char *reason);
static void chrome_set_visible(AppState *state, gboolean visible);

static void chrome_request_frame(AppState *state)
{
    if (!state || !state->view)
        return;
    int width = state->viewport_width > 0 ? state->viewport_width : wpe_view_get_width(state->view);
    int height = state->viewport_height > 0 ? state->viewport_height : wpe_view_get_height(state->view);
    wpe_view_resized(state->view, width, height);
}

typedef struct {
    int panel_x;
    int panel_y;
    int panel_width;
    int panel_height;
    int list_top;
    int list_height;
    int footer_top;
    int row_height;
    int header_height;
    int footer_height;
    double max_scroll;
} ChromeTabsGeometry;

static int chrome_toolbar_legacy_button_width(int panel_width)
{
    int button_x = panel_width / 2 + CHROME_TOOLBAR_MARGIN;
    return MAX(36, (panel_width - button_x - CHROME_TOOLBAR_MARGIN)
        / CHROME_TOOLBAR_LEGACY_BUTTON_COUNT);
}

static int chrome_toolbar_controls_x(int panel_width)
{
    return panel_width / 2 + CHROME_TOOLBAR_MARGIN
        + chrome_toolbar_legacy_button_width(panel_width);
}

static int chrome_address_right(int panel_width)
{
    return chrome_toolbar_controls_x(panel_width) - CHROME_TOOLBAR_MARGIN;
}

static ChromeTabsGeometry chrome_tabs_geometry(AppState *state)
{
    int panel_width = state && state->panel_width > 0 ? state->panel_width : 960;
    int panel_height = state && state->panel_height > 0 ? state->panel_height : 266;
    int chrome_height = state ? state->chrome.height : 44;
    ChromeTabsGeometry geometry = {
        .panel_x = chrome_address_right(panel_width),
        .panel_y = chrome_height,
        .panel_width = panel_width - chrome_address_right(panel_width),
        .panel_height = MAX(0, panel_height - chrome_height),
        .row_height = CHROME_TABS_ROW_HEIGHT,
        .header_height = CHROME_TABS_HEADER_HEIGHT,
        .footer_height = CHROME_TABS_FOOTER_HEIGHT,
    };
    geometry.list_top = geometry.panel_y + geometry.header_height;
    geometry.footer_top = panel_height - geometry.footer_height;
    geometry.list_height = MAX(0, geometry.footer_top - geometry.list_top);
    int content_height = (state ? state->chrome.tab_count : 0) * geometry.row_height;
    geometry.max_scroll = MAX(0, content_height - geometry.list_height);
    return geometry;
}

static void chrome_clamp_tabs_scroll(AppState *state)
{
    ChromeTabsGeometry geometry = chrome_tabs_geometry(state);
    state->chrome.tabs_scroll_offset = CLAMP(state->chrome.tabs_scroll_offset, 0.0,
                                              geometry.max_scroll);
}

static void chrome_position_active_tab(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    ChromeTabsGeometry geometry = chrome_tabs_geometry(state);
    if (chrome->tab_count <= 1 || chrome->active <= 0)
        chrome->tabs_scroll_offset = 0;
    else if (chrome->active >= chrome->tab_count - 1)
        chrome->tabs_scroll_offset = geometry.max_scroll;
    else {
        double active_center = ((double)chrome->active + 0.5) * geometry.row_height;
        chrome->tabs_scroll_offset = active_center - (double)geometry.list_height / 2.0;
        chrome_clamp_tabs_scroll(state);
    }
}

static void chrome_open_tabs_panel(AppState *state)
{
    state->chrome.panel = CHROME_PANEL_TABS;
    chrome_set_visible(state, TRUE);
    chrome_position_active_tab(state);
}

static void chrome_refresh_profiles(BrowserChrome *chrome)
{
    g_clear_pointer(&chrome->panel_profiles, g_ptr_array_unref);
    if (!profile_runtime.store)
        return;
    GError *error = NULL;
    chrome->panel_profiles = browser_profile_store_list_profiles(profile_runtime.store, &error);
    if (!chrome->panel_profiles)
        g_warning("Profile list failed: %s", error ? error->message : "unknown");
    g_clear_error(&error);
}

static void chrome_refresh_pages(BrowserChrome *chrome, gboolean bookmarks)
{
    g_clear_pointer(&chrome->panel_pages, g_ptr_array_unref);
    if (profile_runtime.guest || !profile_runtime.store)
        return;
    GError *error = NULL;
    guint offset = chrome->panel_page * 6;
    chrome->panel_pages = bookmarks
        ? browser_profile_store_list_bookmarks(profile_runtime.store, profile_runtime.profile.id,
                                               offset, 6, &error)
        : browser_profile_store_list_visits(profile_runtime.store, profile_runtime.profile.id,
                                            offset, 6, &error);
    if (!chrome->panel_pages)
        g_warning("%s list failed: %s", bookmarks ? "Bookmark" : "History",
                  error ? error->message : "unknown");
    g_clear_error(&error);
}

static const char *cookie_policy_label(BrowserCookiePolicy policy)
{
    switch (policy) {
    case BROWSER_COOKIE_NO_THIRD_PARTY:
        return "NO THIRD PARTY";
    case BROWSER_COOKIE_ACCEPT_NEVER:
        return "NEVER";
    case BROWSER_COOKIE_ACCEPT_ALL:
    default:
        return "ALL";
    }
}

static void chrome_write_panel_lines(GKeyFile *key_file, AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    BrowserTab *tab = chrome_active_tab(chrome);
    char line[128];
    int line_count = 0;

    g_key_file_remove_group(key_file, "panel", NULL);
    if (chrome->panel == CHROME_PANEL_NONE)
        return;

    if (chrome->panel == CHROME_PANEL_MENU) {
        gboolean bookmarked = !profile_runtime.guest && tab && tab->url[0]
            && browser_profile_store_has_bookmark(profile_runtime.store,
                                                  profile_runtime.profile.id, tab->url);
        g_key_file_set_string(key_file, "panel", "line0",
                              bookmarked ? "REMOVE BOOKMARK" : "ADD BOOKMARK");
        g_key_file_set_string(key_file, "panel", "line1", "HISTORY");
        g_key_file_set_string(key_file, "panel", "line2", "BOOKMARKS");
        snprintf(line, sizeof(line), "PROFILES  %s%s",
                 profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
                 profile_runtime.guest ? " (GUEST)" : "");
        g_key_file_set_string(key_file, "panel", "line3", line);
        g_key_file_set_string(key_file, "panel", "line4", "PRIVACY & COOKIES");
        g_key_file_set_string(key_file, "panel", "line5", "SETTINGS");
        line_count = 6;
    } else if (chrome->panel == CHROME_PANEL_TABS) {
        for (int i = 0; i < chrome->tab_count && line_count < 8; ++i) {
            BrowserTab *item = &chrome->tabs[i];
            char value[96];
            snprintf(value, sizeof(value), "%d  %s", i + 1,
                     item->title[0] ? item->title : item->url);
            snprintf(line, sizeof(line), "line%d", line_count);
            g_key_file_set_string(key_file, "panel", line, value);
            line_count++;
        }
        ChromeTabsGeometry geometry = chrome_tabs_geometry(state);
        g_key_file_set_integer(key_file, "panel", "x", geometry.panel_x);
        g_key_file_set_integer(key_file, "panel", "y", geometry.panel_y);
        g_key_file_set_integer(key_file, "panel", "width", geometry.panel_width);
        g_key_file_set_integer(key_file, "panel", "height", geometry.panel_height);
        g_key_file_set_integer(key_file, "panel", "header_height", geometry.header_height);
        g_key_file_set_integer(key_file, "panel", "footer_height", geometry.footer_height);
        g_key_file_set_integer(key_file, "panel", "row_height", geometry.row_height);
        g_key_file_set_double(key_file, "panel", "scroll_offset", chrome->tabs_scroll_offset);
        g_key_file_set_boolean(key_file, "panel", "can_add",
                               chrome->tab_count < MAX_BROWSER_TABS);
    } else if (chrome->panel == CHROME_PANEL_SETTINGS) {
        const char *home = chrome->home_url && chrome->home_url[0] ? chrome->home_url : default_home_url();
        snprintf(line, sizeof(line), "SET HOME %.80s", home);
        g_key_file_set_string(key_file, "panel", "line0", line);
        snprintf(line, sizeof(line), "SITE MODE %s",
                 !g_strcmp0(normalize_site_profile(chrome->site_profile), "desktop") ? "DESKTOP" : "MOBILE");
        g_key_file_set_string(key_file, "panel", "line1", line);
        g_key_file_set_string(key_file, "panel", "line2", chrome->touch_debug ? "TOUCH DEBUG ON" : "TOUCH DEBUG OFF");
        g_key_file_set_string(key_file, "panel", "line3", "CLEAR CACHE");
        g_key_file_set_string(key_file, "panel", "line4", "ABOUT WPE DRM2");
        line_count = 5;
    } else if (chrome->panel == CHROME_PANEL_PRIVACY) {
        snprintf(line, sizeof(line), "COOKIE POLICY %s", cookie_policy_label(chrome->cookie_policy));
        g_key_file_set_string(key_file, "panel", "line0", line);
        g_key_file_set_string(key_file, "panel", "line1",
                              chrome->site_data_clearing ? "CLEARING SITE DATA..." : "CLEAR COOKIES & SITE DATA");
        line_count = 2;
        if (chrome->site_data_status[0]) {
            g_key_file_set_string(key_file, "panel", "line2", chrome->site_data_status);
            line_count = 3;
        }
    } else if (chrome->panel == CHROME_PANEL_CLEAR_SITE_DATA) {
        g_key_file_set_string(key_file, "panel", "line0", "CANCEL");
        g_key_file_set_string(key_file, "panel", "line1", "CONFIRM CLEAR COOKIES & SITE DATA");
        line_count = 2;
    } else if (chrome->panel == CHROME_PANEL_PROFILES) {
        if (!chrome->panel_profiles)
            chrome_refresh_profiles(chrome);
        guint offset = chrome->panel_page * 4;
        for (guint index = 0; chrome->panel_profiles
                && index < 4 && offset + index < chrome->panel_profiles->len; ++index) {
            BrowserProfile *profile = g_ptr_array_index(chrome->panel_profiles, offset + index);
            snprintf(line, sizeof(line), "line%d", line_count);
            char value[128];
            snprintf(value, sizeof(value), "%c %s%s",
                     !profile_runtime.guest && profile->id == profile_runtime.profile.id ? '*' : ' ',
                     profile->name, profile->is_default ? " [DEFAULT]" : "");
            g_key_file_set_string(key_file, "panel", line, value);
            line_count++;
        }
        g_key_file_set_string(key_file, "panel", "line4", "GUEST                 NEW PROFILE");
        line_count = 5;
        g_key_file_set_string(key_file, "panel", "line5", "PREV       NEXT       MANAGE ACTIVE");
        line_count = 6;
    } else if (chrome->panel == CHROME_PANEL_PROFILE_MANAGE) {
        snprintf(line, sizeof(line), "RENAME %s",
                 profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT");
        g_key_file_set_string(key_file, "panel", "line0", line);
        g_key_file_set_string(key_file, "panel", "line1",
                              profile_runtime.profile.is_default ? "DELETE DISABLED (DEFAULT)" : "DELETE PROFILE");
        line_count = 2;
    } else if (chrome->panel == CHROME_PANEL_PROFILE_DELETE) {
        g_key_file_set_string(key_file, "panel", "line0", "CANCEL");
        g_key_file_set_string(key_file, "panel", "line1", "CONFIRM DELETE PROFILE");
        line_count = 2;
    } else if (chrome->panel == CHROME_PANEL_HISTORY
               || chrome->panel == CHROME_PANEL_BOOKMARKS) {
        gboolean bookmarks = chrome->panel == CHROME_PANEL_BOOKMARKS;
        if (!chrome->panel_pages)
            chrome_refresh_pages(chrome, bookmarks);
        for (guint index = 0; chrome->panel_pages && index < chrome->panel_pages->len; ++index) {
            BrowserStoredPage *page = g_ptr_array_index(chrome->panel_pages, index);
            snprintf(line, sizeof(line), "line%d", line_count);
            char value[128];
            snprintf(value, sizeof(value), "%s%s",
                     chrome->panel_delete_mode ? "DELETE " : "",
                     page->title && page->title[0] ? page->title : page->url);
            g_key_file_set_string(key_file, "panel", line, value);
            line_count++;
        }
        while (line_count < 6) {
            snprintf(line, sizeof(line), "line%d", line_count++);
            g_key_file_set_string(key_file, "panel", line, "-");
        }
        g_key_file_set_string(key_file, "panel", "line6", "PREV PAGE                 NEXT PAGE");
        g_key_file_set_string(key_file, "panel", "line7",
                              bookmarks
                                ? (chrome->panel_delete_mode ? "DONE DELETING" : "DELETE MODE")
                                : (chrome->panel_delete_mode ? "DONE DELETING      CLEAR HISTORY" : "DELETE MODE        CLEAR HISTORY"));
        line_count = 8;
    } else if (chrome->panel == CHROME_PANEL_CLEAR_HISTORY) {
        g_key_file_set_string(key_file, "panel", "line0", "CANCEL");
        g_key_file_set_string(key_file, "panel", "line1", "CONFIRM CLEAR HISTORY");
        line_count = 2;
    }

    g_key_file_set_integer(key_file, "panel", "line_count", line_count);
}

/* 内容未变化时跳过写盘：chrome 状态在 load/title/滚动等事件里高频重写，
 * 而合成器侧按文件 hash 轮询，重复写既浪费 IO 又触发无谓的重绘检查。 */
static gboolean chrome_write_file_if_changed(const char *path, const char *data, gsize length, gchar **last_written, gsize *last_length)
{
    if (*last_written && *last_length == length && !memcmp(*last_written, data, length))
        return TRUE;
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    if (!g_file_set_contents(path, data, length, NULL))
        return FALSE;
    chmod(path, 0600);
    g_free(*last_written);
    *last_written = g_strndup(data, length);
    *last_length = length;
    return TRUE;
}

static void chrome_update_render_state(AppState *state)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->enabled)
        return;

    BrowserTab *tab = chrome_active_tab(chrome);
    GKeyFile *key_file = g_key_file_new();
    g_key_file_set_boolean(key_file, "chrome", "enabled", chrome->enabled);
    g_key_file_set_boolean(key_file, "chrome", "visible", chrome->visible);
    g_key_file_set_boolean(key_file, "chrome", "loading", chrome->loading);
    g_key_file_set_double(key_file, "chrome", "load_progress", chrome->load_progress);
    g_key_file_set_boolean(key_file, "chrome", "can_back", tab && tab->back_count > 0);
    g_key_file_set_boolean(key_file, "chrome", "can_forward", tab && tab->forward_count > 0);
    g_key_file_set_boolean(key_file, "chrome", "touch_debug", chrome->touch_debug);
    g_key_file_set_integer(key_file, "chrome", "height", chrome->height);
    g_key_file_set_integer(key_file, "chrome", "tab_count", chrome->tab_count);
    g_key_file_set_integer(key_file, "chrome", "active_tab", chrome->active);
    g_key_file_set_string(key_file, "chrome", "site_profile", normalize_site_profile(chrome->site_profile));
    g_key_file_set_string(key_file, "chrome", "profile",
                          profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT");
    g_key_file_set_boolean(key_file, "chrome", "guest_profile", profile_runtime.guest);
    char *transition_us = g_strdup_printf("%" G_GINT64_FORMAT, chrome->transition_us);
    g_key_file_set_string(key_file, "chrome", "transition_us", transition_us);
    g_free(transition_us);
    g_key_file_set_string(key_file, "chrome", "panel", chrome_panel_name(chrome->panel));
    g_key_file_set_string(key_file, "chrome", "url", tab && tab->url[0] ? tab->url : (chrome->home_url ? chrome->home_url : default_home_url()));
    g_key_file_set_string(key_file, "chrome", "title", tab && tab->title[0] ? tab->title : "");
    chrome_write_panel_lines(key_file, state);

    gsize length = 0;
    gchar *data = g_key_file_to_data(key_file, &length, NULL);
    if (data) {
        static gchar *last_render_state;
        static gsize last_render_state_length;
        if (!chrome_write_file_if_changed(chrome->render_state_path, data, length, &last_render_state, &last_render_state_length))
            g_warning("Failed to write chrome render state: %s", chrome->render_state_path);
    }
    g_free(data);
    g_key_file_unref(key_file);
}

static void chrome_save_state(AppState *state)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->enabled)
        return;

    if (!profile_runtime.guest && profile_runtime.store) {
        GError *error = NULL;
        GPtrArray *tabs = g_ptr_array_new_with_free_func((GDestroyNotify)browser_stored_tab_free);
        for (int index = 0; index < chrome->tab_count; ++index) {
            BrowserTab *source = &chrome->tabs[index];
            BrowserStoredTab *tab = g_new0(BrowserStoredTab, 1);
            tab->logical_id = source->id;
            tab->url = g_strdup(source->url);
            tab->title = g_strdup(source->title);
            tab->active = index == chrome->active;
            tab->back = g_ptr_array_new_with_free_func(g_free);
            tab->forward = g_ptr_array_new_with_free_func(g_free);
            for (int item = 0; item < source->back_count; ++item)
                g_ptr_array_add(tab->back, g_strdup(source->back[item]));
            for (int item = 0; item < source->forward_count; ++item)
                g_ptr_array_add(tab->forward, g_strdup(source->forward[item]));
            g_ptr_array_add(tabs, tab);
        }
        gboolean success = browser_profile_store_set_home_url(profile_runtime.store,
                profile_runtime.profile.id,
                chrome->home_url ? chrome->home_url : default_home_url(), &error)
            && browser_profile_store_set_site_profile(profile_runtime.store,
                profile_runtime.profile.id, normalize_site_profile(chrome->site_profile), &error)
            && browser_profile_store_set_cookie_policy(profile_runtime.store,
                profile_runtime.profile.id, chrome->cookie_policy, &error)
            && browser_profile_store_save_tabs(profile_runtime.store,
                profile_runtime.profile.id, tabs, chrome->next_tab_id, &error);
        if (!success)
            g_warning("Profile state save failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        g_ptr_array_unref(tabs);
    }
    chrome_update_render_state(state);
    chrome_apply_layout(state, "chrome_state");
}

static void chrome_set_tab_url(BrowserTab *tab, const char *url)
{
    if (!tab || !url)
        return;
    g_strlcpy(tab->url, url, sizeof(tab->url));
}

static void chrome_set_visible(AppState *state, gboolean visible)
{
    BrowserChrome *chrome = &state->chrome;
    if (chrome->visible == visible)
        return;
    chrome->visible = visible;
    chrome->transition_us = g_get_monotonic_time();
}

static int chrome_animation_duration_ms(void)
{
    const char *value = g_getenv("WPE_CHROME_ANIMATION_MS");
    if (!value || !value[0])
        return 160;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0)
        return 160;
    if (parsed > 500)
        return 500;
    return (int)parsed;
}

static double chrome_shown_fraction(BrowserChrome *chrome)
{
    if (!chrome || chrome->panel != CHROME_PANEL_NONE)
        return 1.0;

    double fraction = chrome->visible ? 1.0 : 0.0;
    if (chrome->transition_us <= 0)
        return fraction;

    int duration_ms = chrome_animation_duration_ms();
    gint64 elapsed_us = g_get_monotonic_time() - chrome->transition_us;
    if (elapsed_us < 0)
        elapsed_us = 0;
    double t = (double)elapsed_us / ((double)duration_ms * 1000.0);
    if (t > 1.0)
        t = 1.0;
    double eased = 1.0 - pow(1.0 - t, 3.0);
    return chrome->visible ? eased : (1.0 - eased);
}

static int chrome_toolbar_y(BrowserChrome *chrome)
{
    if (!chrome)
        return 0;
    double fraction = chrome_shown_fraction(chrome);
    return (int)lround((fraction - 1.0) * chrome->height);
}

static int chrome_toolbar_hit_slop(void)
{
    int slop = (int)env_double("WPE_CHROME_HIT_SLOP", 6);
    return CLAMP(slop, 0, 20);
}

static int chrome_reveal_height(void)
{
    int height = (int)env_double("WPE_CHROME_REVEAL_HEIGHT", 22);
    return CLAMP(height, 12, 40);
}

static int chrome_page_top_inset(AppState *state)
{
    if (!state || !state->chrome.enabled)
        return 0;
    BrowserChrome *chrome = &state->chrome;
    if (chrome->panel != CHROME_PANEL_NONE)
        return chrome->height;
    if (chrome_layout_resize_enabled())
        return chrome->visible ? chrome->height : 0;
    double fraction = chrome_shown_fraction(chrome);
    return (int)lround((double)chrome->height * fraction);
}

static double web_event_y(AppState *state, double screen_y)
{
    double y = screen_y - chrome_page_top_inset(state);
    int height = chrome_layout_resize_enabled() && state->viewport_height > 0
        ? state->viewport_height
        : (state->panel_height > 0 ? state->panel_height : state->viewport_height);
    if (height <= 0)
        height = 1;
    if (y < 0)
        return 0;
    if (y > height - 1)
        return height - 1;
    return y;
}

static void chrome_load_url(AppState *state, const char *url, gboolean push_history)
{
    if (!state || !state->web_view || !url || !url[0])
        return;
    BrowserChrome *chrome = &state->chrome;
    BrowserTab *tab = chrome_active_tab(chrome);
    if (!tab)
        return;

    if (push_history && tab->url[0] && g_strcmp0(tab->url, url)) {
        history_push(tab->back, &tab->back_count, tab->url);
        history_clear(tab->forward, &tab->forward_count);
    }
    chrome_set_tab_url(tab, url);
    chrome->suppress_history = TRUE;
    chrome->loading = TRUE;
    chrome->load_progress = 0.0;
    chrome->panel = CHROME_PANEL_NONE;
    chrome_set_visible(state, TRUE);
    chrome_save_state(state);
    chrome_request_frame(state);
    load_uri_preserving_local_html(state->web_view, url);
}

static void chrome_new_tab(AppState *state, const char *url)
{
    BrowserChrome *chrome = &state->chrome;
    if (chrome->tab_count >= MAX_BROWSER_TABS)
        chrome->active = chrome->tab_count - 1;
    else {
        chrome->active = chrome->tab_count++;
        BrowserTab *tab = chrome_active_tab(chrome);
        tab->id = chrome->next_tab_id++;
        chrome_set_tab_url(tab, url && url[0] ? url : (chrome->home_url ? chrome->home_url : default_home_url()));
        tab->title[0] = 0;
    }
    chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
}

static void chrome_close_tab(AppState *state, int index)
{
    BrowserChrome *chrome = &state->chrome;
    if (chrome->tab_count <= 0 || index < 0 || index >= chrome->tab_count)
        return;
    gboolean closed_active = index == chrome->active;
    chrome_clear_tab(&chrome->tabs[index]);
    for (int i = index; i < chrome->tab_count - 1; ++i)
        chrome->tabs[i] = chrome->tabs[i + 1];
    memset(&chrome->tabs[chrome->tab_count - 1], 0, sizeof(BrowserTab));
    chrome->tab_count--;
    if (chrome->tab_count <= 0) {
        chrome->tab_count = 1;
        chrome->active = 0;
        chrome->tabs[0].id = chrome->next_tab_id++;
        chrome_set_tab_url(&chrome->tabs[0], chrome->home_url ? chrome->home_url : default_home_url());
        closed_active = TRUE;
    } else if (index < chrome->active)
        chrome->active--;
    else if (chrome->active >= chrome->tab_count)
        chrome->active = chrome->tab_count - 1;
    chrome_clamp_tabs_scroll(state);
    if (closed_active) {
        chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
        chrome->panel = CHROME_PANEL_TABS;
        chrome_clamp_tabs_scroll(state);
        chrome_save_state(state);
        chrome_request_frame(state);
    } else {
        chrome_save_state(state);
        chrome_request_frame(state);
    }
}

static void chrome_switch_tab(AppState *state, int index)
{
    BrowserChrome *chrome = &state->chrome;
    if (index < 0 || index >= chrome->tab_count || index == chrome->active)
        return;
    chrome->active = index;
    chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
}

static void chrome_go_back(AppState *state)
{
    BrowserTab *tab = chrome_active_tab(&state->chrome);
    if (!tab || tab->back_count <= 0)
        return;
    if (tab->url[0])
        history_push(tab->forward, &tab->forward_count, tab->url);
    char *url = history_pop(tab->back, &tab->back_count);
    chrome_load_url(state, url, FALSE);
    g_free(url);
}

static void chrome_go_forward(AppState *state)
{
    BrowserTab *tab = chrome_active_tab(&state->chrome);
    if (!tab || tab->forward_count <= 0)
        return;
    if (tab->url[0])
        history_push(tab->back, &tab->back_count, tab->url);
    char *url = history_pop(tab->forward, &tab->forward_count);
    chrome_load_url(state, url, FALSE);
    g_free(url);
}

static char *json_escape_string(const char *value)
{
    GString *out = g_string_new(NULL);
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    for (; *p; ++p) {
        switch (*p) {
        case '\\': g_string_append(out, "\\\\"); break;
        case '"': g_string_append(out, "\\\""); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:
            if (*p < 0x20)
                g_string_append_printf(out, "\\u%04x", *p);
            else
                g_string_append_c(out, (char)*p);
            break;
        }
    }
    char *result = out->str;
    g_slice_free(GString, out);
    return result;
}

static int json_hex_value(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static char *json_payload_text(const char *payload, gboolean *has_text)
{
    if (has_text)
        *has_text = FALSE;
    if (!payload)
        return g_strdup("");
    if (payload[0] != '{') {
        if (has_text)
            *has_text = TRUE;
        return g_strdup(payload);
    }

    const char *position = strstr(payload, "\"text\"");
    if (!position)
        return g_strdup("");
    position = strchr(position + 6, ':');
    if (!position)
        return g_strdup("");
    position++;
    while (g_ascii_isspace(*position))
        position++;
    if (*position != '"')
        return g_strdup("");
    position++;

    GString *result = g_string_new(NULL);
    while (*position && *position != '"') {
        if (*position != '\\') {
            g_string_append_c(result, *position++);
            continue;
        }
        position++;
        switch (*position) {
        case '"': g_string_append_c(result, '"'); position++; break;
        case '\\': g_string_append_c(result, '\\'); position++; break;
        case 'n': g_string_append_c(result, '\n'); position++; break;
        case 'r': g_string_append_c(result, '\r'); position++; break;
        case 't': g_string_append_c(result, '\t'); position++; break;
        case 'u': {
            gunichar character = 0;
            gboolean valid = TRUE;
            for (int index = 1; index <= 4; ++index) {
                int digit = json_hex_value(position[index]);
                if (digit < 0) {
                    valid = FALSE;
                    break;
                }
                character = (character << 4) | (gunichar)digit;
            }
            if (valid) {
                g_string_append_unichar(result, character);
                position += 5;
            } else
                position++;
            break;
        }
        case '\0': break;
        default: g_string_append_c(result, *position++); break;
        }
    }
    if (has_text)
        *has_text = TRUE;
    return g_string_free(result, FALSE);
}

static guint64 json_payload_sequence(const char *payload)
{
    if (!payload || payload[0] != '{')
        return 0;
    const char *position = strstr(payload, "\"sequence\"");
    if (!position)
        return 0;
    position = strchr(position + 10, ':');
    if (!position)
        return 0;
    return g_ascii_strtoull(position + 1, NULL, 10);
}

static char *uri_scheme_dup(const char *uri)
{
    if (!uri || !g_ascii_isalpha(uri[0]))
        return NULL;

    const char *p = uri + 1;
    while (*p) {
        if (*p == ':')
            return g_ascii_strdown(uri, p - uri);
        if (*p == '/' || *p == '?' || *p == '#' || g_ascii_isspace(*p))
            return NULL;
        if (!(g_ascii_isalnum(*p) || *p == '+' || *p == '-' || *p == '.'))
            return NULL;
        p++;
    }
    return NULL;
}

static gboolean browser_scheme_is_allowed(const char *scheme)
{
    if (!scheme || !scheme[0])
        return TRUE;
    return !g_ascii_strcasecmp(scheme, "http")
        || !g_ascii_strcasecmp(scheme, "https")
        || !g_ascii_strcasecmp(scheme, "about")
        || !g_ascii_strcasecmp(scheme, "file");
}

static gboolean is_bvid_char(char c)
{
    return g_ascii_isalnum(c);
}

static char *bilibili_web_url_from_uri(const char *uri)
{
    if (!uri || !uri[0])
        return NULL;

    const char *bv = NULL;
    for (const char *p = uri; *p; ++p) {
        if (p[0] == 'B' && p[1] == 'V' && is_bvid_char(p[2])) {
            bv = p;
            break;
        }
    }
    if (bv) {
        const char *end = bv;
        while (*end && is_bvid_char(*end) && end - bv < 32)
            end++;
        if (end - bv >= 4) {
            char *bvid = g_strndup(bv, end - bv);
            char *url = g_strdup_printf("https://www.bilibili.com/video/%s/", bvid);
            g_free(bvid);
            return url;
        }
    }

    const char *prefix = "bilibili://video/";
    if (!g_ascii_strncasecmp(uri, prefix, strlen(prefix))) {
        const char *id = uri + strlen(prefix);
        while (*id && !g_ascii_isdigit(*id))
            id++;
        const char *end = id;
        while (*end && g_ascii_isdigit(*end))
            end++;
        if (end > id) {
            char *avid = g_strndup(id, end - id);
            char *url = g_strdup_printf("https://www.bilibili.com/video/av%s/", avid);
            g_free(avid);
            return url;
        }
    }

    return NULL;
}

static char *normalize_user_url(const char *raw)
{
    char *value = g_strdup(raw ? raw : "");
    g_strstrip(value);
    if (!value[0]) {
        g_free(value);
        return g_strdup(default_home_url());
    }
    if (!g_ascii_strcasecmp(value, "baidu")) {
        g_free(value);
        return g_strdup("https://m.baidu.com/");
    }
    if (!g_ascii_strcasecmp(value, "bing")) {
        g_free(value);
        return g_strdup("https://www.bing.com/");
    }

    char *scheme = uri_scheme_dup(value);
    if (scheme) {
        if (browser_scheme_is_allowed(scheme)) {
            g_free(scheme);
            return value;
        }
        if (!g_ascii_strcasecmp(scheme, "bilibili")) {
            char *web_url = bilibili_web_url_from_uri(value);
            if (web_url) {
                g_print("Converted Bilibili user URL: raw=%s url=%s\n", value, web_url);
                g_free(scheme);
                g_free(value);
                return web_url;
            }
        }
        g_warning("Blocked user URL scheme: scheme=%s raw=%s", scheme, value);
        g_free(scheme);
        g_free(value);
        return NULL;
    }
    if (strstr(value, "://")) {
        g_warning("Blocked malformed URL scheme: raw=%s", value);
        g_free(value);
        return NULL;
    }

    if (strchr(value, ' ') || strchr(value, '\t') || !strchr(value, '.')) {
        char *escaped = g_uri_escape_string(value, NULL, TRUE);
        char *url = g_strdup_printf("https://m.baidu.com/s?word=%s", escaped ? escaped : "");
        g_free(escaped);
        g_free(value);
        return url;
    }

    char *url = g_strdup_printf("https://%s", value);
    g_free(value);
    return url;
}

static char *keyboard_path(AppState *state, const char *subdir, const char *filename)
{
    if (!state || !state->keyboard_dir || !state->keyboard_dir[0] || !subdir || !filename)
        return NULL;
    char *dir = g_build_filename(state->keyboard_dir, subdir, NULL);
    char *path = g_build_filename(dir, filename, NULL);
    g_free(dir);
    return path;
}

static gboolean keyboard_write_atomic(const char *path, const char *contents)
{
    if (!path || !contents)
        return FALSE;
    char *temporary = g_strdup_printf("%s.tmp.XXXXXX", path);
    int fd = g_mkstemp(temporary);
    if (fd < 0) {
        g_free(temporary);
        return FALSE;
    }
    fchmod(fd, 0600);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    const char *cursor = contents;
    size_t remaining = strlen(contents);
    gboolean ok = TRUE;
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            ok = FALSE;
            break;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    if (ok && fsync(fd) != 0)
        ok = FALSE;
    if (close(fd) != 0)
        ok = FALSE;
    if (ok && g_rename(temporary, path) != 0)
        ok = FALSE;
    if (!ok)
        g_unlink(temporary);
    else
        chmod(path, 0600);
    g_free(temporary);
    return ok;
}

static void keyboard_write_completion(AppState *state, const char *status, const char *detail)
{
    if (!state || !state->keyboard_active_id)
        return;
    char *filename = g_strdup_printf("%s.json", state->keyboard_active_id);
    char *path = keyboard_path(state, "status", filename);
    char *status_json = json_escape_string(status ? status : "unknown");
    char *detail_json = json_escape_string(detail ? detail : "");
    char *payload = g_strdup_printf(
        "{\"id\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}",
        state->keyboard_active_id, status_json, detail_json);
    if (!path || !keyboard_write_atomic(path, payload))
        g_warning("Keyboard completion write failed: id=%s", state->keyboard_active_id);
    g_free(payload);
    g_free(detail_json);
    g_free(status_json);
    g_free(path);
    g_free(filename);
}

static void keyboard_release_frame_waiter(AppState *state)
{
    if (!state)
        return;
    if (state->keyboard_frame_reply) {
        webkit_script_message_reply_unref(state->keyboard_frame_reply);
        state->keyboard_frame_reply = NULL;
    }
    g_clear_object(&state->keyboard_frame_context);
}

static void keyboard_reply_message(JSCValue *message, WebKitScriptMessageReply *reply,
                                   const char *payload)
{
    if (!message || !reply)
        return;
    JSCContext *context = jsc_value_get_context(message);
    JSCValue *value = jsc_value_new_string(context, payload ? payload : "");
    webkit_script_message_reply_return_value(reply, value);
    g_object_unref(value);
}

static gboolean keyboard_send_frame_payload(AppState *state, const char *payload)
{
    if (!state || !state->keyboard_frame_reply || !state->keyboard_frame_context)
        return FALSE;
    JSCValue *value = jsc_value_new_string(state->keyboard_frame_context,
                                           payload ? payload : "");
    webkit_script_message_reply_return_value(state->keyboard_frame_reply, value);
    g_object_unref(value);
    keyboard_release_frame_waiter(state);
    return TRUE;
}

static void keyboard_store_frame_waiter(AppState *state, JSCValue *message,
                                        WebKitScriptMessageReply *reply)
{
    if (!state || !message || !reply)
        return;
    if (state->keyboard_frame_reply)
        keyboard_send_frame_payload(state, "op=superseded");
    state->keyboard_frame_reply = webkit_script_message_reply_ref(reply);
    state->keyboard_frame_context = g_object_ref(jsc_value_get_context(message));
}

static void keyboard_dispatch_frame_command(AppState *state)
{
    if (!state || !state->keyboard_active_id || state->keyboard_apply_in_flight ||
        !state->keyboard_queued_text || !state->keyboard_frame_reply)
        return;

    char *text = g_steal_pointer(&state->keyboard_queued_text);
    guint64 sequence = state->keyboard_queued_sequence;
    gboolean commit = state->keyboard_queued_commit;
    state->keyboard_queued_sequence = 0;
    state->keyboard_queued_commit = FALSE;
    g_free(state->keyboard_sent_text);
    state->keyboard_sent_text = g_strdup(text);
    state->keyboard_sent_sequence = sequence;
    state->keyboard_sent_commit = commit;
    state->keyboard_apply_in_flight = TRUE;

    char *escaped = g_uri_escape_string(text, NULL, TRUE);
    char *payload = g_strdup_printf(
        "op=%s&id=%s&sequence=%" G_GUINT64_FORMAT "&text=%s",
        commit ? "commit" : "update",
        state->keyboard_active_id,
        sequence,
        escaped ? escaped : "");
    g_print("Keyboard frame command: id=%s sequence=%" G_GUINT64_FORMAT
            " bytes=%zu commit=%d\n",
            state->keyboard_active_id, sequence, strlen(text), commit);
    keyboard_send_frame_payload(state, payload);
    g_free(payload);
    g_free(escaped);
    g_free(text);
}

static void keyboard_clear_active(AppState *state)
{
    if (!state)
        return;
    if (state->keyboard_response_source_id) {
        g_source_remove(state->keyboard_response_source_id);
        state->keyboard_response_source_id = 0;
    }
    if (state->keyboard_active_id) {
        char *request_path = keyboard_path(state, "requests", "current.json");
        if (request_path)
            g_unlink(request_path);
        g_free(request_path);
        const char *suffixes[] = { ".update", ".ok", ".cancel" };
        for (unsigned index = 0; index < G_N_ELEMENTS(suffixes); ++index) {
            char *filename = g_strdup_printf("%s%s", state->keyboard_active_id, suffixes[index]);
            char *response_path = keyboard_path(state, "responses", filename);
            if (response_path)
                g_unlink(response_path);
            g_free(response_path);
            g_free(filename);
        }
    }
    g_clear_pointer(&state->keyboard_active_id, g_free);
    g_clear_pointer(&state->keyboard_active_kind, g_free);
    g_clear_pointer(&state->keyboard_pending_text, g_free);
    g_clear_pointer(&state->keyboard_queued_text, g_free);
    g_clear_pointer(&state->keyboard_sent_text, g_free);
    keyboard_release_frame_waiter(state);
    state->keyboard_pending_text_valid = FALSE;
    state->keyboard_last_sequence = 0;
    state->keyboard_apply_in_flight = FALSE;
    state->keyboard_queued_sequence = 0;
    state->keyboard_queued_commit = FALSE;
    state->keyboard_sent_sequence = 0;
    state->keyboard_sent_commit = FALSE;
    state->keyboard_terminal_pending = FALSE;
    state->keyboard_active_us = 0;
}

static void keyboard_finish_active(AppState *state, const char *status, const char *detail)
{
    if (!state || !state->keyboard_active_id)
        return;
    g_print("Keyboard terminal: id=%s kind=%s status=%s detail=%s\n",
            state->keyboard_active_id,
            state->keyboard_active_kind ? state->keyboard_active_kind : "",
            status ? status : "unknown", detail ? detail : "");
    keyboard_write_completion(state, status, detail);
    char *status_escaped = g_uri_escape_string(status ? status : "unknown", NULL, TRUE);
    char *detail_escaped = g_uri_escape_string(detail ? detail : "", NULL, TRUE);
    char *payload = g_strdup_printf("op=complete&status=%s&detail=%s",
                                    status_escaped ? status_escaped : "",
                                    detail_escaped ? detail_escaped : "");
    keyboard_send_frame_payload(state, payload);
    g_free(payload);
    g_free(detail_escaped);
    g_free(status_escaped);
    keyboard_clear_active(state);
}

static void keyboard_queue_web_apply(AppState *state, const char *text,
                                     guint64 sequence, gboolean commit)
{
    if (!state)
        return;
    g_free(state->keyboard_queued_text);
    state->keyboard_queued_text = g_strdup(text ? text : "");
    state->keyboard_queued_sequence = sequence;
    state->keyboard_queued_commit = commit;
    keyboard_dispatch_frame_command(state);
}

static void keyboard_handle_update(AppState *state, const char *text, guint64 sequence)
{
    if (!state || !state->keyboard_active_id || !state->keyboard_active_kind)
        return;
    if (state->keyboard_terminal_pending) {
        g_print("Keyboard update ignored after terminal: id=%s sequence=%"
                G_GUINT64_FORMAT "\n",
                state->keyboard_active_id, sequence);
        return;
    }

    if (!sequence)
        sequence = state->keyboard_last_sequence + 1;
    if (sequence <= state->keyboard_last_sequence) {
        g_print("Keyboard update ignored: id=%s sequence=%" G_GUINT64_FORMAT " last=%" G_GUINT64_FORMAT "\n",
                state->keyboard_active_id, sequence, state->keyboard_last_sequence);
        return;
    }
    state->keyboard_last_sequence = sequence;
    g_free(state->keyboard_pending_text);
    state->keyboard_pending_text = g_strdup(text ? text : "");
    state->keyboard_pending_text_valid = TRUE;
    g_print("Keyboard update: id=%s kind=%s sequence=%" G_GUINT64_FORMAT " bytes=%zu\n",
            state->keyboard_active_id,
            state->keyboard_active_kind,
            sequence,
            strlen(text ? text : ""));

    if (!g_strcmp0(state->keyboard_active_kind, "web_input"))
        keyboard_queue_web_apply(state, text, sequence, FALSE);
}

static void keyboard_handle_response(AppState *state, gboolean confirmed, const char *text,
                                     gboolean has_text, guint64 sequence)
{
    if (!state || !state->keyboard_active_id || !state->keyboard_active_kind)
        return;

    const char *final_text = has_text ? (text ? text : "") : "";
    if (confirmed && (!has_text || (sequence && sequence < state->keyboard_last_sequence)) &&
        state->keyboard_pending_text_valid && state->keyboard_pending_text)
        final_text = state->keyboard_pending_text;

    g_print("Keyboard %s: id=%s kind=%s bytes=%zu\n",
            confirmed ? "confirmed" : "cancelled",
            state->keyboard_active_id,
            state->keyboard_active_kind,
            strlen(final_text));

    if (confirmed) {
        if (!g_strcmp0(state->keyboard_active_kind, "address")) {
            char *url = normalize_user_url(final_text);
            if (url)
                chrome_load_url(state, url, TRUE);
            else
                g_warning("Ignoring blocked address input: bytes=%zu", strlen(final_text));
            g_free(url);
        } else if (!g_strcmp0(state->keyboard_active_kind, "home_url")) {
            char *url = normalize_user_url(final_text);
            if (url) {
                g_free(state->chrome.home_url);
                state->chrome.home_url = url;
                state->chrome.panel = CHROME_PANEL_NONE;
                chrome_save_state(state);
                chrome_request_frame(state);
            } else
                g_warning("Ignoring blocked home URL input: bytes=%zu", strlen(final_text));
        } else if (!g_strcmp0(state->keyboard_active_kind, "profile_new")) {
            BrowserProfile profile = { 0 };
            GError *error = NULL;
            if (!browser_profile_store_create_profile(profile_runtime.store, final_text,
                                                      &profile, &error))
                g_warning("Profile create failed: %s", error ? error->message : "unknown");
            else {
                g_print("Profile created: id=%" G_GINT64_FORMAT " name=%s\n",
                        profile.id, profile.name);
                chrome_switch_profile(state, profile.id, FALSE);
            }
            browser_profile_clear(&profile);
            g_clear_error(&error);
        } else if (!g_strcmp0(state->keyboard_active_kind, "profile_rename")) {
            GError *error = NULL;
            if (!browser_profile_store_rename_profile(profile_runtime.store,
                    profile_runtime.profile.id, final_text, &error))
                g_warning("Profile rename failed: %s", error ? error->message : "unknown");
            else {
                char *normalized = NULL;
                if (browser_profile_store_is_name_valid(final_text, &normalized, NULL)) {
                    g_free(profile_runtime.profile.name);
                    profile_runtime.profile.name = normalized;
                }
                state->chrome.panel = CHROME_PANEL_PROFILE_MANAGE;
                chrome_update_render_state(state);
                g_print("Profile renamed: id=%" G_GINT64_FORMAT " name=%s\n",
                        profile_runtime.profile.id, profile_runtime.profile.name);
            }
            g_clear_error(&error);
        } else if (!g_strcmp0(state->keyboard_active_kind, "web_input")) {
            guint64 final_sequence = MAX(sequence, state->keyboard_last_sequence);
            if (!final_sequence)
                final_sequence = 1;
            state->keyboard_terminal_pending = TRUE;
            keyboard_queue_web_apply(state, final_text, final_sequence, TRUE);
            return;
        }
    }
    keyboard_finish_active(state, confirmed ? "confirmed" : "cancelled",
                           confirmed ? "accepted" : "user_cancelled");
}

/* 注意:该轮询只在键盘会话激活期间存在(keyboard_request 创建、
 * 确认/取消/超时即移除),空闲时无任何开销,因此维持轮询而非 inotify。 */
static gboolean keyboard_response_tick(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (!state || !state->keyboard_active_id) {
        if (state)
            state->keyboard_response_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    gint64 timeout_ms = (gint64)env_double("WPE_KEYBOARD_TIMEOUT_MS", 120000);
    if (timeout_ms > 0 && state->keyboard_active_us > 0 &&
        g_get_monotonic_time() - state->keyboard_active_us > timeout_ms * 1000) {
        g_warning("Keyboard timeout: id=%s kind=%s", state->keyboard_active_id,
                  state->keyboard_active_kind ? state->keyboard_active_kind : "");
        state->keyboard_response_source_id = 0;
        keyboard_finish_active(state, "expired", "timeout");
        return G_SOURCE_REMOVE;
    }

    char *ok_name = g_strdup_printf("%s.ok", state->keyboard_active_id);
    char *cancel_name = g_strdup_printf("%s.cancel", state->keyboard_active_id);
    char *update_name = g_strdup_printf("%s.update", state->keyboard_active_id);
    char *ok_path = keyboard_path(state, "responses", ok_name);
    char *cancel_path = keyboard_path(state, "responses", cancel_name);
    char *update_path = keyboard_path(state, "responses", update_name);
    g_free(ok_name);
    g_free(cancel_name);
    g_free(update_name);

    char *contents = NULL;
    gsize length = 0;
    if (update_path && g_file_get_contents(update_path, &contents, &length, NULL)) {
        g_unlink(update_path);
        gboolean has_text = FALSE;
        guint64 sequence = json_payload_sequence(contents);
        char *text = json_payload_text(contents, &has_text);
        if (has_text)
            keyboard_handle_update(state, text, sequence);
        else
            g_warning("Keyboard update ignored: malformed payload id=%s", state->keyboard_active_id);
        g_free(text);
        g_free(contents);
        contents = NULL;
    }
    g_free(update_path);

    if (ok_path && g_file_get_contents(ok_path, &contents, &length, NULL)) {
        g_unlink(ok_path);
        g_free(ok_path);
        if (cancel_path) {
            g_unlink(cancel_path);
            g_free(cancel_path);
        }
        gboolean web_input = !g_strcmp0(state->keyboard_active_kind, "web_input");
        gboolean has_text = FALSE;
        guint64 sequence = json_payload_sequence(contents);
        char *text = json_payload_text(contents, &has_text);
        if (!web_input)
            state->keyboard_response_source_id = 0;
        keyboard_handle_response(state, TRUE, text, has_text, sequence);
        g_free(text);
        g_free(contents);
        if (web_input && state->keyboard_active_id)
            return G_SOURCE_CONTINUE;
        return G_SOURCE_REMOVE;
    }
    g_free(ok_path);

    if (cancel_path && g_file_test(cancel_path, G_FILE_TEST_EXISTS)) {
        g_unlink(cancel_path);
        g_free(cancel_path);
        state->keyboard_response_source_id = 0;
        keyboard_handle_response(state, FALSE, "", FALSE, 0);
        return G_SOURCE_REMOVE;
    }
    g_free(cancel_path);

    return G_SOURCE_CONTINUE;
}

static gboolean keyboard_request(AppState *state, const char *kind, const char *text,
                                 const char *placeholder, const char *input_type,
                                 int maxlength, gboolean multiline)
{
    if (!state || !state->keyboard_dir || !state->keyboard_dir[0]) {
        g_warning("Keyboard request ignored: no WPE_KEYBOARD_DIR kind=%s", kind ? kind : "");
        return FALSE;
    }
    if (state->keyboard_active_id) {
        g_warning("Keyboard request ignored: active id=%s kind=%s new=%s",
                  state->keyboard_active_id,
                  state->keyboard_active_kind ? state->keyboard_active_kind : "",
                  kind ? kind : "");
        return FALSE;
    }

    char *requests_dir = g_build_filename(state->keyboard_dir, "requests", NULL);
    char *responses_dir = g_build_filename(state->keyboard_dir, "responses", NULL);
    char *status_dir = g_build_filename(state->keyboard_dir, "status", NULL);
    g_mkdir_with_parents(requests_dir, 0700);
    g_mkdir_with_parents(responses_dir, 0700);
    g_mkdir_with_parents(status_dir, 0700);
    g_free(requests_dir);
    g_free(responses_dir);
    g_free(status_dir);

    state->keyboard_next_id++;
    char *id = g_strdup_printf("%" G_GUINT64_FORMAT "_%" G_GINT64_FORMAT,
                               state->keyboard_next_id, g_get_monotonic_time());
    char *path = keyboard_path(state, "requests", "current.json");

    char *kind_json = json_escape_string(kind ? kind : "");
    char *text_json = json_escape_string(text ? text : "");
    char *placeholder_json = json_escape_string(placeholder ? placeholder : "");
    char *input_type_json = json_escape_string(input_type && input_type[0] ? input_type : "ZhCNPreferred");
    char *payload = g_strdup_printf(
        "{\"id\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\",\"placeholder\":\"%s\","
        "\"inputType\":\"%s\",\"maxlength\":%d,\"multiLinesEditVisible\":%s,"
        "\"confirmButtonDisabledOnTextEmpty\":false}",
        id, kind_json, text_json, placeholder_json, input_type_json,
        maxlength == 0 ? 100 : maxlength,
        multiline ? "true" : "false");

    gboolean ok = path && keyboard_write_atomic(path, payload);
    if (ok) {
        state->keyboard_active_id = g_strdup(id);
        state->keyboard_active_kind = g_strdup(kind ? kind : "");
        state->keyboard_pending_text = g_strdup(text ? text : "");
        state->keyboard_pending_text_valid = FALSE;
        state->keyboard_last_sequence = 0;
        state->keyboard_active_us = g_get_monotonic_time();
        guint poll_ms = (guint)env_double("WPE_KEYBOARD_POLL_MS", 30);
        if (poll_ms < 16)
            poll_ms = 16;
        if (poll_ms > 250)
            poll_ms = 250;
        state->keyboard_response_source_id = g_timeout_add(poll_ms, keyboard_response_tick, state);
        g_print("Keyboard request: id=%s kind=%s placeholder=%s maxlength=%d multiline=%d\n",
                id, kind ? kind : "", placeholder ? placeholder : "", maxlength, multiline);
    } else
        g_warning("Keyboard request write failed: %s", path ? path : "(null)");

    g_free(payload);
    g_free(kind_json);
    g_free(text_json);
    g_free(placeholder_json);
    g_free(input_type_json);
    g_free(path);
    g_free(id);
    return ok;
}

/* 一次性把 "k=v&k=v" 消息解析成哈希表,取代之前每取一个参数就 g_strsplit
 * 整条消息的做法(单条消息要连取 6 个参数)。值已做 URI 反转义。 */
static GHashTable *query_parse_params(const char *message)
{
    GHashTable *params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!message)
        return params;
    char **pairs = g_strsplit(message, "&", -1);
    for (int i = 0; pairs && pairs[i]; ++i) {
        char *equals = strchr(pairs[i], '=');
        if (!equals)
            continue;
        *equals = 0;
        char *value = g_uri_unescape_string(equals + 1, NULL);
        if (value)
            g_hash_table_replace(params, g_strdup(pairs[i]), value);
    }
    g_strfreev(pairs);
    return params;
}

static const char *params_get_string(GHashTable *params, const char *key, const char *fallback)
{
    const char *value = g_hash_table_lookup(params, key);
    return value && value[0] ? value : fallback;
}

static int params_get_int(GHashTable *params, const char *key, int fallback)
{
    const char *value = g_hash_table_lookup(params, key);
    if (!value)
        return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    return end != value ? (int)parsed : fallback;
}

static gboolean params_get_bool(GHashTable *params, const char *key, gboolean fallback)
{
    const char *value = g_hash_table_lookup(params, key);
    if (!value)
        return fallback;
    return !g_ascii_strcasecmp(value, "1") || !g_ascii_strcasecmp(value, "true") ||
        !g_ascii_strcasecmp(value, "yes");
}

static guint64 params_get_uint64(GHashTable *params, const char *key, guint64 fallback)
{
    const char *value = g_hash_table_lookup(params, key);
    if (!value || !value[0])
        return fallback;
    char *end = NULL;
    guint64 parsed = g_ascii_strtoull(value, &end, 10);
    return end != value ? parsed : fallback;
}

static gboolean on_keyboard_script_message_with_reply(
    WebKitUserContentManager *manager, JSCValue *value,
    WebKitScriptMessageReply *reply, gpointer user_data)
{
    (void)manager;
    AppState *state = (AppState *)user_data;
    if (!state || !value || !reply)
        return FALSE;

    char *message = jsc_value_to_string(value);
    GHashTable *params = query_parse_params(message);
    const char *operation = params_get_string(params, "op", "");

    if (!g_strcmp0(operation, "open")) {
        gint64 pointer_gate_us =
            (gint64)env_double("WPE_KEYBOARD_POINTER_GATE_MS", 2000) * 1000;
        gint64 since_pointer_us = state->last_pointer_tap_us > 0
            ? g_get_monotonic_time() - state->last_pointer_tap_us
            : G_MAXINT64;
        gboolean opened = FALSE;
        if (pointer_gate_us <= 0 || since_pointer_us <= pointer_gate_us) {
            opened = keyboard_request(state, "web_input",
                params_get_string(params, "text", ""),
                params_get_string(params, "placeholder", "请输入内容"),
                params_get_string(params, "inputType", "ZhCNPreferred"),
                params_get_int(params, "maxlength", 100),
                params_get_bool(params, "multiLinesEditVisible", FALSE));
        } else {
            g_print("Keyboard web_input ignored: no recent pointer tap since_us=%"
                    G_GINT64_FORMAT "\n", since_pointer_us);
        }
        if (opened && state->keyboard_active_id) {
            char *payload = g_strdup_printf("op=opened&id=%s",
                                            state->keyboard_active_id);
            keyboard_reply_message(value, reply, payload);
            g_free(payload);
        } else
            keyboard_reply_message(value, reply, "op=rejected");
        g_hash_table_unref(params);
        g_free(message);
        return TRUE;
    }

    if (!g_strcmp0(operation, "wait")) {
        const char *request_id = params_get_string(params, "id", "");
        if (!state->keyboard_active_id ||
            g_strcmp0(request_id, state->keyboard_active_id) ||
            g_strcmp0(state->keyboard_active_kind, "web_input")) {
            keyboard_reply_message(value, reply, "op=expired");
            g_hash_table_unref(params);
            g_free(message);
            return TRUE;
        }

        guint64 ack_sequence = params_get_uint64(params, "ackSequence", 0);
        if (state->keyboard_apply_in_flight) {
            gboolean sequence_matches = ack_sequence == state->keyboard_sent_sequence;
            gboolean applied = params_get_bool(params, "applied", FALSE);
            const char *observed = g_hash_table_lookup(params, "observed");
            gboolean value_matches = observed && state->keyboard_sent_text &&
                !g_strcmp0(observed, state->keyboard_sent_text);
            gboolean verified = sequence_matches && applied && value_matches;
            g_print("Keyboard frame ack: id=%s sequence=%" G_GUINT64_FORMAT
                    " expected=%" G_GUINT64_FORMAT " applied=%d value_match=%d commit=%d\n",
                    state->keyboard_active_id, ack_sequence,
                    state->keyboard_sent_sequence, applied, value_matches,
                    state->keyboard_sent_commit);
            gboolean terminal_commit =
                state->keyboard_sent_commit && state->keyboard_terminal_pending;
            state->keyboard_apply_in_flight = FALSE;
            state->keyboard_sent_sequence = 0;
            state->keyboard_sent_commit = FALSE;
            g_clear_pointer(&state->keyboard_sent_text, g_free);
            if (terminal_commit) {
                keyboard_store_frame_waiter(state, value, reply);
                keyboard_finish_active(state, "confirmed",
                                       verified ? "applied" : "apply_failed");
                g_hash_table_unref(params);
                g_free(message);
                return TRUE;
            }
        }

        keyboard_store_frame_waiter(state, value, reply);
        keyboard_dispatch_frame_command(state);
        g_hash_table_unref(params);
        g_free(message);
        return TRUE;
    }

    keyboard_reply_message(value, reply, "op=invalid");
    g_hash_table_unref(params);
    g_free(message);
    return TRUE;
}

static void setup_keyboard_bridge(AppState *state)
{
    if (!state)
        return;
    const char *keyboard_dir = g_getenv("WPE_KEYBOARD_DIR");
    if (!keyboard_dir || !keyboard_dir[0]) {
        g_print("Keyboard bridge disabled: WPE_KEYBOARD_DIR empty\n");
        return;
    }
    state->keyboard_dir = g_strdup(keyboard_dir);
    char *requests_dir = g_build_filename(state->keyboard_dir, "requests", NULL);
    char *responses_dir = g_build_filename(state->keyboard_dir, "responses", NULL);
    char *status_dir = g_build_filename(state->keyboard_dir, "status", NULL);
    g_mkdir_with_parents(requests_dir, 0700);
    g_mkdir_with_parents(responses_dir, 0700);
    g_mkdir_with_parents(status_dir, 0700);
    g_print("Keyboard bridge: dir=%s requests=%s responses=%s status=%s\n",
            state->keyboard_dir, requests_dir, responses_dir, status_dir);
    g_free(requests_dir);
    g_free(responses_dir);
    g_free(status_dir);
}

static void setup_keyboard_user_script(WebKitUserContentManager *manager, AppState *state)
{
    if (!manager || !state)
        return;
    g_signal_connect(manager, "script-message-with-reply-received::haasKeyboard",
                     G_CALLBACK(on_keyboard_script_message_with_reply), state);
    if (!webkit_user_content_manager_register_script_message_handler_with_reply(
            manager, "haasKeyboard", NULL))
        g_warning("Keyboard script message handler already registered");

    const char *source =
        "(function(){"
        "if(window.__haasKeyboardInstalled)return;"
        "window.__haasKeyboardInstalled=true;"
        "window.__haasKeyboardTarget=null;"
        "window.__haasKeyboardTargetId='';"
        "window.__haasKeyboardSeq=1;"
        "window.__haasKeyboardInFlight=false;"
        "function editable(el){return !!(el&&((el.tagName==='INPUT'&&!/^(button|submit|reset|checkbox|radio|file|image|range|color)$/i.test(el.type||''))||el.tagName==='TEXTAREA'||el.isContentEditable)&&!el.disabled&&!el.readOnly);}"
        "function closestEditable(el){while(el&&el!==document){if(editable(el))return el;el=el.parentElement;}return null;}"
        "function enc(v){return encodeURIComponent(v==null?'':String(v));}"
        "function dec(v){try{return decodeURIComponent(String(v||'').replace(/\\+/g,' '));}catch(e){return String(v||'');}}"
        "function parse(v){var out={};String(v||'').split('&').forEach(function(part){var at=part.indexOf('=');var k=at<0?part:part.slice(0,at);var x=at<0?'':part.slice(at+1);if(k)out[k]=dec(x);});return out;}"
        "function post(v){try{return Promise.resolve(window.webkit.messageHandlers.haasKeyboard.postMessage(v));}catch(e){return Promise.reject(e);}}"
        "function valueOf(el){return el.isContentEditable?(el.innerText||el.textContent||''):(el.value||'');}"
        "function typeOf(el){var t=String(el.getAttribute('type')||'').toLowerCase();if(t==='number'||t==='tel')return 'Number';if(t==='email'||t==='url'||t==='password')return 'EnUSPreferred';return 'ZhCNPreferred';}"
        "function mark(el){var id=el.getAttribute('data-haas-keyboard-id');if(!id){id='hk'+Date.now().toString(36)+(window.__haasKeyboardSeq++).toString(36);try{el.setAttribute('data-haas-keyboard-id',id);}catch(e){}}window.__haasKeyboardTargetId=id||'';return id||'';}"
        "function findTarget(){var el=window.__haasKeyboardTarget;if(editable(el)&&document.documentElement&&document.documentElement.contains(el))return el;var id=window.__haasKeyboardTargetId;if(id&&document.querySelector){try{el=document.querySelector('[data-haas-keyboard-id=\"'+id+'\"]');if(editable(el))return el;}catch(e){}}el=document.activeElement;if(editable(el))return el;return null;}"
        "function diffType(oldv,newv){oldv=String(oldv==null?'':oldv);newv=String(newv==null?'':newv);if(newv.length<oldv.length){return oldv.indexOf(newv)===0?'deleteContentBackward':'deleteContentForward';}if(newv.length>oldv.length){return newv.indexOf(oldv)===0?'insertText':'insertReplacementText';}return 'insertReplacementText';}"
        "function makeInputEvent(name,typ,data,cancelable){try{return new InputEvent(name,{bubbles:true,cancelable:!!cancelable,inputType:typ,data:data});}catch(e){var ev=document.createEvent('Event');ev.initEvent(name,true,!!cancelable);try{ev.inputType=typ;ev.data=data;}catch(_e){}return ev;}}"
        "function nativeSet(el,value,oldv){if(el.isContentEditable){var ok=false;try{var sel=window.getSelection();var range=document.createRange();range.selectNodeContents(el);sel.removeAllRanges();sel.addRange(range);ok=document.execCommand&&document.execCommand('insertText',false,value);}catch(e){}if(!ok||valueOf(el)!==value)el.textContent=value;return;}var proto=el.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;var desc=Object.getOwnPropertyDescriptor(proto,'value');if(desc&&desc.set)desc.set.call(el,value);else el.value=value;var tracker=el._valueTracker;if(tracker){try{tracker.setValue(oldv);}catch(e){}}if(el.setSelectionRange){try{el.setSelectionRange(String(value).length,String(value).length);}catch(e){}}}"
        "function applyCore(el,value){var oldv=valueOf(el);var typ=diffType(oldv,value);var data=typ.indexOf('delete')===0?null:value;try{el.focus();}catch(e){}el.dispatchEvent(makeInputEvent('beforeinput',typ,data,true));nativeSet(el,value,oldv);el.dispatchEvent(makeInputEvent('input',typ,data,false));return typ;}"
        "function nextFrame(fn){var done=false;var run=function(){if(done)return;done=true;fn();};setTimeout(run,50);if(window.requestAnimationFrame)requestAnimationFrame(run);}"
        "function applyText(value,commit){return new Promise(function(resolve){var el=findTarget();if(!editable(el)){resolve({applied:false,observed:'',alive:false});return;}value=String(value==null?'':value);try{applyCore(el,value);}catch(e){resolve({applied:false,observed:valueOf(el),alive:true});return;}nextFrame(function(){if(valueOf(el)!==value){try{applyCore(el,value);}catch(e){}}nextFrame(function(){var observed=valueOf(el);var applied=observed===value;if(commit&&applied){try{var ev=document.createEvent('HTMLEvents');ev.initEvent('change',true,false);el.dispatchEvent(ev);}catch(e){}}resolve({applied:applied,observed:observed,alive:editable(el)});});});});}"
        "function finish(){window.__haasKeyboardInFlight=false;window.__haasKeyboardTarget=null;window.__haasKeyboardTargetId='';}"
        "function wait(id,ack){var msg='op=wait&id='+enc(id);if(ack){msg+='&ackSequence='+enc(ack.sequence)+'&applied='+(ack.applied?'1':'0')+'&observed='+enc(ack.observed||'');}return post(msg).then(function(raw){var cmd=parse(raw);if(cmd.op==='update'||cmd.op==='commit'){var sequence=parseInt(cmd.sequence||'0',10)||0;return applyText(cmd.text||'',cmd.op==='commit').then(function(result){return wait(id,{sequence:sequence,applied:result.applied,observed:result.observed});});}if(cmd.op==='complete'||cmd.op==='expired'||cmd.op==='superseded'||cmd.op==='rejected'){finish();return;}return wait(id,null);}).catch(function(){finish();});}"
        "function request(el){el=closestEditable(el);if(window.__haasKeyboardInFlight||!editable(el)||!window.webkit||!window.webkit.messageHandlers||!window.webkit.messageHandlers.haasKeyboard)return;window.__haasKeyboardInFlight=true;window.__haasKeyboardTarget=el;mark(el);var ml=(el.tagName==='TEXTAREA'||el.isContentEditable);var max=parseInt(el.getAttribute('maxlength')||'',10);if(!isFinite(max)||max<=0)max=ml?512:100;var ph=el.getAttribute('placeholder')||el.getAttribute('aria-label')||'请输入内容';var msg='op=open&text='+enc(valueOf(el))+'&placeholder='+enc(ph)+'&inputType='+enc(typeOf(el))+'&maxlength='+max+'&multiLinesEditVisible='+(ml?'1':'0');post(msg).then(function(raw){var opened=parse(raw);if(opened.op!=='opened'||!opened.id){finish();return;}wait(opened.id,null);}).catch(function(){finish();});}"
        "document.addEventListener('click',function(e){if(e.isTrusted===false)return;var path=e.composedPath?e.composedPath():null;var el=closestEditable(path&&path.length?path[0]:e.target);if(el)setTimeout(function(){request(el);},0);},true);"
        "})();";
    WebKitUserScript *script = webkit_user_script_new(source,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        NULL,
        NULL);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
    g_print("Keyboard user script installed\n");
}

static gboolean remove_dir_contents(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return FALSE;
    const char *name = NULL;
    while ((name = g_dir_read_name(dir))) {
        char *child = g_build_filename(path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
            remove_dir_contents(child);
            g_rmdir(child);
        } else
            g_unlink(child);
        g_free(child);
    }
    g_dir_close(dir);
    return TRUE;
}

static void chrome_clear_cache(void)
{
    const char *cache = g_getenv("XDG_CACHE_HOME");
    if (cache && cache[0])
        remove_dir_contents(cache);
}

static void chrome_toggle_site_profile(AppState *state)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    const char *current = normalize_site_profile(chrome->site_profile);
    const char *next = !g_strcmp0(current, "desktop") ? "mobile" : "desktop";
    g_strlcpy(chrome->site_profile, next, sizeof(chrome->site_profile));
    chrome_apply_site_profile(state);
    BrowserTab *tab = chrome_active_tab(chrome);
    const char *reload_url = tab && tab->url[0] ? tab->url : (chrome->home_url ? chrome->home_url : default_home_url());
    g_print("Site profile changed: %s reload=%s\n", next, reload_url);
    chrome->panel = CHROME_PANEL_NONE;
    chrome_save_state(state);
    chrome_load_url(state, reload_url, FALSE);
}

static void chrome_apply_cookie_policy(AppState *state, BrowserCookiePolicy policy)
{
    if (!state || !state->network_session)
        return;
    state->chrome.cookie_policy = policy;
    profile_runtime.profile.cookie_policy = policy;
    WebKitCookieManager *manager = webkit_network_session_get_cookie_manager(state->network_session);
    webkit_cookie_manager_set_accept_policy(manager, webkit_cookie_policy(policy));
    chrome_save_state(state);
    g_print("Cookie policy changed: profile=%s policy=%s\n",
            profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
            browser_cookie_policy_name(policy));
}

static void chrome_cycle_cookie_policy(AppState *state)
{
    BrowserCookiePolicy policy = state->chrome.cookie_policy;
    if (profile_runtime.guest) {
        g_print("Guest cookie policy remains session-only\n");
        return;
    }
    policy = policy == BROWSER_COOKIE_ACCEPT_ALL ? BROWSER_COOKIE_NO_THIRD_PARTY
        : policy == BROWSER_COOKIE_NO_THIRD_PARTY ? BROWSER_COOKIE_ACCEPT_NEVER
        : BROWSER_COOKIE_ACCEPT_ALL;
    chrome_apply_cookie_policy(state, policy);
}

static gboolean chrome_write_profile_switch(const char *value)
{
    const char *path = g_getenv("WPE_PROFILE_SWITCH_FILE");
    char *fallback = NULL;
    if (!path || !path[0]) {
        const char *var_dir = g_getenv("WPE_VAR_DIR");
        fallback = g_build_filename(var_dir && var_dir[0] ? var_dir : "/tmp",
                                    "profile-switch.request", NULL);
        path = fallback;
    }
    char *directory = g_path_get_dirname(path);
    g_mkdir_with_parents(directory, 0700);
    chmod(directory, 0700);
    g_free(directory);
    char *contents = g_strdup_printf("%s\n", value);
    gboolean success = g_file_set_contents(path, contents, -1, NULL);
    if (success)
        chmod(path, 0600);
    else
        g_warning("Profile switch file write failed: %s", path);
    g_free(contents);
    g_free(fallback);
    return success;
}

static void chrome_switch_profile(AppState *state, int64_t profile_id, gboolean guest)
{
    if (!state)
        return;
    chrome_save_state(state);
    GError *error = NULL;
    char value[32];
    if (guest)
        g_strlcpy(value, "guest", sizeof(value));
    else {
        if (!browser_profile_store_set_active_profile(profile_runtime.store, profile_id, &error)) {
            g_warning("Profile switch rejected: %s", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }
        g_snprintf(value, sizeof(value), "%" G_GINT64_FORMAT, profile_id);
    }
    if (!chrome_write_profile_switch(value))
        return;
    g_print("Profile switch requested: from=%s to=%s exit=75\n",
            profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT", value);
    runtime_exit_code = 75;
    if (main_loop)
        g_main_loop_quit(main_loop);
}

static void on_clear_site_data_finished(GObject *object, GAsyncResult *result, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (!state)
        return;

    GError *error = NULL;
    gboolean success = webkit_website_data_manager_clear_finish(
        WEBKIT_WEBSITE_DATA_MANAGER(object), result, &error);
    state->chrome.site_data_clearing = FALSE;
    g_strlcpy(state->chrome.site_data_status,
              success ? "SITE DATA CLEARED" : "CLEAR FAILED",
              sizeof(state->chrome.site_data_status));
    if (success)
        g_print("Profile site data cleared: profile=%s guest=%d\n",
                profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
                profile_runtime.guest);
    else
        g_warning("Profile site data clear failed: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static void chrome_clear_site_data(AppState *state)
{
    if (!state || !state->network_session || state->chrome.site_data_clearing)
        return;
    WebKitWebsiteDataManager *manager = webkit_network_session_get_website_data_manager(state->network_session);
    state->chrome.site_data_clearing = TRUE;
    state->chrome.site_data_status[0] = '\0';
    state->chrome.panel = CHROME_PANEL_PRIVACY;
    webkit_website_data_manager_clear(manager, WEBKIT_WEBSITE_DATA_ALL, 0, NULL,
                                      on_clear_site_data_finished, state);
    g_print("Profile site data clear requested: profile=%s guest=%d\n",
            profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
            profile_runtime.guest);
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static void chrome_open_history(AppState *state, gboolean bookmarks)
{
    BrowserChrome *chrome = &state->chrome;
    chrome->panel = bookmarks ? CHROME_PANEL_BOOKMARKS : CHROME_PANEL_HISTORY;
    chrome->panel_page = 0;
    chrome->panel_delete_mode = FALSE;
    chrome_refresh_pages(chrome, bookmarks);
    chrome_update_render_state(state);
}

static void chrome_go_panel_back(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    ChromePanel parent = chrome_panel_parent(chrome->panel);
    chrome->panel = parent;
    chrome->panel_page = 0;
    chrome->panel_delete_mode = FALSE;
    if (parent == CHROME_PANEL_PROFILES)
        chrome_refresh_profiles(chrome);
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static void chrome_select_panel_row(AppState *state, double x, int row)
{
    BrowserChrome *chrome = &state->chrome;
    int panel_width = state->panel_width > 0 ? state->panel_width : 960;
    if (chrome->panel == CHROME_PANEL_MENU) {
        if (row == 0 && !profile_runtime.guest) {
            BrowserTab *tab = chrome_active_tab(chrome);
            if (tab && tab->url[0]) {
                GError *error = NULL;
                gboolean added = FALSE;
                if (!browser_profile_store_toggle_bookmark(profile_runtime.store,
                        profile_runtime.profile.id, tab->url, tab->title, &added, &error))
                    g_warning("Bookmark toggle failed: %s", error ? error->message : "unknown");
                else
                    g_print("Bookmark %s: %s\n", added ? "added" : "removed", tab->url);
                g_clear_error(&error);
            }
        } else if (row == 1)
            chrome_open_history(state, FALSE);
        else if (row == 2)
            chrome_open_history(state, TRUE);
        else if (row == 3) {
            chrome->panel = CHROME_PANEL_PROFILES;
            chrome->panel_page = 0;
            chrome_refresh_profiles(chrome);
        } else if (row == 4) {
            chrome->panel = CHROME_PANEL_PRIVACY;
            chrome->site_data_status[0] = '\0';
        } else if (row == 5)
            chrome->panel = CHROME_PANEL_SETTINGS;
    } else if (chrome->panel == CHROME_PANEL_SETTINGS) {
        if (row == 0) {
            chrome->panel = CHROME_PANEL_NONE;
            chrome_save_state(state);
            keyboard_request(state, "home_url", chrome->home_url ? chrome->home_url : default_home_url(),
                             "设置主页 URL", "EnUSPreferred", 512, FALSE);
        } else if (row == 1) {
            chrome_toggle_site_profile(state);
        } else if (row == 2) {
            chrome->touch_debug = !chrome->touch_debug;
            chrome_save_state(state);
        } else if (row == 3) {
            chrome_clear_cache();
            chrome->panel = CHROME_PANEL_MENU;
            chrome_save_state(state);
        } else if (row == 4) {
            g_print("About: Direct WPE DRM browser profile=%s guest=%d\n",
                    profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
                    profile_runtime.guest);
        }
    } else if (chrome->panel == CHROME_PANEL_PRIVACY) {
        if (row == 0)
            chrome_cycle_cookie_policy(state);
        else if (row == 1 && !chrome->site_data_clearing) {
            chrome->panel = CHROME_PANEL_CLEAR_SITE_DATA;
            chrome_update_render_state(state);
        }
    } else if (chrome->panel == CHROME_PANEL_CLEAR_SITE_DATA) {
        if (row == 0)
            chrome->panel = CHROME_PANEL_PRIVACY;
        else if (row == 1)
            chrome_clear_site_data(state);
    } else if (chrome->panel == CHROME_PANEL_PROFILES) {
        guint offset = chrome->panel_page * 4;
        if (row >= 0 && row < 4 && chrome->panel_profiles
                && offset + row < chrome->panel_profiles->len) {
            BrowserProfile *profile = g_ptr_array_index(chrome->panel_profiles, offset + row);
            if (!profile_runtime.guest && profile->id == profile_runtime.profile.id) {
                chrome->panel = CHROME_PANEL_PROFILE_MANAGE;
                chrome_update_render_state(state);
            } else
                chrome_switch_profile(state, profile->id, FALSE);
        } else if (row == 4) {
            if (x < panel_width / 2)
                chrome_switch_profile(state, 0, TRUE);
            else
                keyboard_request(state, "profile_new", "", "新建 Profile 名称",
                                 "ZhCNPreferred", 20, FALSE);
        } else if (row == 5) {
            if (x < panel_width / 3) {
                if (chrome->panel_page > 0)
                    chrome->panel_page--;
                chrome_refresh_profiles(chrome);
            } else if (x < panel_width * 2 / 3) {
                if (chrome->panel_profiles
                        && (chrome->panel_page + 1) * 4 < chrome->panel_profiles->len)
                    chrome->panel_page++;
                chrome_refresh_profiles(chrome);
            } else {
                chrome->panel = CHROME_PANEL_PROFILE_MANAGE;
                chrome_update_render_state(state);
            }
        }
    } else if (chrome->panel == CHROME_PANEL_PROFILE_MANAGE) {
        if (row == 0 && !profile_runtime.guest)
            keyboard_request(state, "profile_rename", profile_runtime.profile.name,
                             "重命名 Profile", "ZhCNPreferred", 20, FALSE);
        else if (row == 1 && !profile_runtime.guest && !profile_runtime.profile.is_default) {
            chrome->panel = CHROME_PANEL_PROFILE_DELETE;
            chrome_update_render_state(state);
        }
    } else if (chrome->panel == CHROME_PANEL_PROFILE_DELETE) {
        if (row == 0) {
            chrome->panel = CHROME_PANEL_PROFILE_MANAGE;
        } else if (row == 1) {
            GError *error = NULL;
            int64_t fallback = 0;
            if (!browser_profile_store_prepare_delete_profile(profile_runtime.store,
                    profile_runtime.profile.id, &fallback, &error))
                g_warning("Profile delete failed: %s", error ? error->message : "unknown");
            else {
                char value[32];
                g_snprintf(value, sizeof(value), "%" G_GINT64_FORMAT, fallback);
                if (chrome_write_profile_switch(value)) {
                    runtime_exit_code = 75;
                    if (main_loop)
                        g_main_loop_quit(main_loop);
                }
            }
            g_clear_error(&error);
        }
    } else if (chrome->panel == CHROME_PANEL_HISTORY
               || chrome->panel == CHROME_PANEL_BOOKMARKS) {
        gboolean bookmarks = chrome->panel == CHROME_PANEL_BOOKMARKS;
        if (row >= 0 && row < 6 && chrome->panel_pages
                && row < (int)chrome->panel_pages->len) {
            BrowserStoredPage *page = g_ptr_array_index(chrome->panel_pages, row);
            if (chrome->panel_delete_mode) {
                GError *error = NULL;
                gboolean success = bookmarks
                    ? browser_profile_store_delete_bookmark(profile_runtime.store,
                        profile_runtime.profile.id, page->id, &error)
                    : browser_profile_store_delete_visit(profile_runtime.store,
                        profile_runtime.profile.id, page->id, &error);
                if (!success)
                    g_warning("Page entry delete failed: %s", error ? error->message : "unknown");
                g_clear_error(&error);
                chrome_refresh_pages(chrome, bookmarks);
            } else {
                char *url = g_strdup(page->url);
                chrome->panel = CHROME_PANEL_NONE;
                chrome_load_url(state, url, TRUE);
                g_free(url);
            }
        } else if (row == 6) {
            if (x < panel_width / 2) {
                if (chrome->panel_page > 0)
                    chrome->panel_page--;
            } else if (chrome->panel_pages && chrome->panel_pages->len == 6)
                chrome->panel_page++;
            chrome_refresh_pages(chrome, bookmarks);
        } else if (row == 7) {
            if (bookmarks || x < panel_width / 2)
                chrome->panel_delete_mode = !chrome->panel_delete_mode;
            else {
                chrome->panel = CHROME_PANEL_CLEAR_HISTORY;
                chrome_update_render_state(state);
            }
        }
    } else if (chrome->panel == CHROME_PANEL_CLEAR_HISTORY) {
        if (row == 0) {
            chrome->panel = CHROME_PANEL_HISTORY;
            chrome_refresh_pages(chrome, FALSE);
        } else if (row == 1 && !profile_runtime.guest) {
            GError *error = NULL;
            if (!browser_profile_store_clear_visits(profile_runtime.store,
                    profile_runtime.profile.id, &error))
                g_warning("History clear failed: %s", error ? error->message : "unknown");
            g_clear_error(&error);
            chrome->panel = CHROME_PANEL_HISTORY;
            chrome->panel_page = 0;
            chrome_refresh_pages(chrome, FALSE);
        }
    }
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static gboolean chrome_handle_tabs_panel_tap(AppState *state, double x, double y)
{
    BrowserChrome *chrome = &state->chrome;
    if (chrome->panel != CHROME_PANEL_TABS)
        return FALSE;
    ChromeTabsGeometry geometry = chrome_tabs_geometry(state);
    if (y < geometry.panel_y)
        return FALSE;

    if (x < geometry.panel_x || x >= geometry.panel_x + geometry.panel_width) {
        chrome->panel = CHROME_PANEL_NONE;
        chrome_save_state(state);
        chrome_request_frame(state);
        g_print("Chrome tabs: outside dismiss x=%.1f y=%.1f\n", x, y);
        return TRUE;
    }
    if (y < geometry.list_top) {
        chrome_go_panel_back(state);
        return TRUE;
    }
    if (y >= geometry.footer_top) {
        if (chrome->tab_count >= MAX_BROWSER_TABS) {
            g_print("Chrome tabs: new tab disabled count=%d max=%d\n",
                    chrome->tab_count, MAX_BROWSER_TABS);
            return TRUE;
        }
        g_print("Chrome tabs: new tab x=%.1f y=%.1f\n", x, y);
        chrome_new_tab(state, chrome->home_url);
        return TRUE;
    }
    if (geometry.list_height <= 0)
        return TRUE;

    int row = (int)floor((y - geometry.list_top + chrome->tabs_scroll_offset)
                         / geometry.row_height);
    if (row < 0 || row >= chrome->tab_count)
        return TRUE;
    int close_x = geometry.panel_x + geometry.panel_width
        - CHROME_TABS_CLOSE_HIT_WIDTH;
    if (x >= close_x) {
        g_print("Chrome tabs: close row=%d active=%d\n", row, chrome->active);
        chrome_close_tab(state, row);
        return TRUE;
    }

    g_print("Chrome tabs: select row=%d active=%d\n", row, chrome->active);
    chrome->panel = CHROME_PANEL_NONE;
    if (row == chrome->active) {
        chrome_save_state(state);
        chrome_request_frame(state);
    } else
        chrome_switch_tab(state, row);
    return TRUE;
}

static void chrome_handle_toolbar_tap(AppState *state, double x, double y)
{
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->enabled)
        return;
    int hidden_hot_zone = chrome_reveal_height();
    if (!chrome->visible && chrome->panel == CHROME_PANEL_NONE && y <= hidden_hot_zone) {
        chrome_set_visible(state, TRUE);
        chrome->panel = CHROME_PANEL_NONE;
        chrome_save_state(state);
        chrome_request_frame(state);
        g_print("Chrome tap: show toolbar x=%.1f y=%.1f\n", x, y);
        return;
    }

    int toolbar_y = chrome_toolbar_y(chrome);
    int panel_y = toolbar_y + chrome->height;
    if (chrome_handle_tabs_panel_tap(state, x, y))
        return;
    if (chrome->panel != CHROME_PANEL_NONE && y >= panel_y && y < panel_y + 22) {
        g_print("Chrome tap: panel back from=%s x=%.1f y=%.1f\n",
                chrome_panel_name(chrome->panel), x, y);
        chrome_go_panel_back(state);
        return;
    }
    if (chrome->panel != CHROME_PANEL_NONE && y >= panel_y + 22) {
        int row = (int)((y - panel_y - 22) / 24);
        if (row >= 0) {
            g_print("Chrome tap: panel=%s row=%d x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), row, x, y);
            chrome_select_panel_row(state, x, row);
        }
        return;
    }

    int hit_height = chrome->height + chrome_toolbar_hit_slop();
    int hit_top = toolbar_y < 0 ? 0 : toolbar_y;
    int hit_bottom = toolbar_y + hit_height;
    if (!chrome->visible || y < hit_top || y > hit_bottom) {
        g_print("Chrome tap ignored: visible=%d x=%.1f y=%.1f hit=%d..%d panel=%s\n",
                chrome->visible, x, y, hit_top, hit_bottom, chrome_panel_name(chrome->panel));
        return;
    }

    int panel_width = state->panel_width > 0 ? state->panel_width : 960;
    if (x < chrome_address_right(panel_width)) {
        BrowserTab *tab = chrome_active_tab(chrome);
        const char *current = tab && tab->url[0]
            ? tab->url
            : (chrome->home_url ? chrome->home_url : default_home_url());
        chrome->panel = CHROME_PANEL_NONE;
        chrome_set_visible(state, TRUE);
        chrome_save_state(state);
        chrome_request_frame(state);
        g_print("Chrome tap: edit address x=%.1f y=%.1f\n", x, y);
        keyboard_request(state, "address", current, "输入网址或搜索",
                         "EnUSPreferred", 512, FALSE);
        return;
    }

    int button_x = chrome_toolbar_controls_x(panel_width);
    int button_w = chrome_toolbar_legacy_button_width(panel_width);
    if (x < button_x) {
        g_print("Chrome tap ignored: toolbar gap x=%.1f button_x=%d\n", x, button_x);
        return;
    }
    int index = (int)((x - button_x) / button_w);
    if (index < 0 || index >= CHROME_TOOLBAR_BUTTON_COUNT) {
        g_print("Chrome tap ignored: button index=%d x=%.1f y=%.1f button_x=%d button_w=%d\n",
                index, x, y, button_x, button_w);
        return;
    }

    if (index == 0) {
        g_print("Chrome tap: back x=%.1f y=%.1f\n", x, y);
        chrome_go_back(state);
    } else if (index == 1) {
        g_print("Chrome tap: forward x=%.1f y=%.1f\n", x, y);
        chrome_go_forward(state);
    } else if (index == 2) {
        if (chrome->panel == CHROME_PANEL_TABS)
            chrome->panel = CHROME_PANEL_NONE;
        else
            chrome_open_tabs_panel(state);
        g_print("Chrome tap: tabs panel=%s x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), x, y);
        chrome_save_state(state);
    } else if (index == 3) {
        g_print("Chrome tap: %s x=%.1f y=%.1f\n", chrome->loading ? "stop" : "reload", x, y);
        if (chrome->loading)
            webkit_web_view_stop_loading(state->web_view);
        else if (chrome_active_tab(chrome))
            chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
    } else if (index == 4) {
        chrome->panel = chrome->panel == CHROME_PANEL_MENU ? CHROME_PANEL_NONE : CHROME_PANEL_MENU;
        chrome->panel_page = 0;
        chrome->panel_delete_mode = FALSE;
        g_print("Chrome tap: menu panel=%s x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), x, y);
        chrome_save_state(state);
    }
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static gboolean chrome_touch_down_consumes(AppState *state, double x, double y)
{
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->enabled)
        return FALSE;
    int hidden_hot_zone = chrome_reveal_height();
    if (!chrome->visible && chrome->panel == CHROME_PANEL_NONE && y <= hidden_hot_zone)
        return TRUE;
    int toolbar_y = chrome_toolbar_y(chrome);
    int panel_y = toolbar_y + chrome->height;
    if (chrome->panel != CHROME_PANEL_NONE && y >= panel_y
            && y <= (state->panel_height > 0 ? state->panel_height : 266))
        return TRUE;
    int hit_top = toolbar_y < 0 ? 0 : toolbar_y;
    int hit_bottom = toolbar_y + chrome->height + chrome_toolbar_hit_slop();
    if (chrome->visible && y >= hit_top && y <= hit_bottom)
        return TRUE;
    return FALSE;
}

static void chrome_note_scroll(AppState *state, double finger_delta_y, guint32 time_ms)
{
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->enabled)
        return;

    int direction = 0;
    double distance = 0;
    if (finger_delta_y < -1.0) {
        direction = -1;
        distance = -finger_delta_y;
    } else if (finger_delta_y > 1.0) {
        direction = 1;
        distance = finger_delta_y;
    } else
        return;

    if (chrome->scroll_direction != direction) {
        chrome->scroll_direction = direction;
        chrome->scroll_accum = 0;
        chrome->scroll_velocity_peak = 0;
        chrome->scroll_last_time_ms = time_ms;
    }

    if (chrome->scroll_last_time_ms) {
        guint32 elapsed_ms = time_ms - chrome->scroll_last_time_ms;
        if (elapsed_ms > 0 && elapsed_ms < 1000) {
            double velocity = distance * 1000.0 / (double)elapsed_ms;
            if (velocity > chrome->scroll_velocity_peak)
                chrome->scroll_velocity_peak = velocity;
        }
    }
    chrome->scroll_last_time_ms = time_ms;

    chrome->scroll_accum += distance;

    if (direction < 0) {
        if (chrome->scroll_accum >= chrome->hide_down_px && chrome->visible && chrome->panel == CHROME_PANEL_NONE) {
            chrome_set_visible(state, FALSE);
            chrome->panel = CHROME_PANEL_NONE;
            chrome->scroll_accum = 0;
            chrome->scroll_velocity_peak = 0;
            chrome->scroll_last_time_ms = 0;
            chrome->scroll_direction = 0;
            chrome_save_state(state);
            chrome_request_frame(state);
        }
    } else if (direction > 0) {
        gboolean fast_enough = chrome->show_up_min_velocity_px_s <= 0 ||
            chrome->scroll_velocity_peak >= chrome->show_up_min_velocity_px_s;
        if (chrome->scroll_accum >= chrome->show_up_px && fast_enough && !chrome->visible) {
            chrome_set_visible(state, TRUE);
            chrome->scroll_accum = 0;
            chrome->scroll_velocity_peak = 0;
            chrome->scroll_last_time_ms = 0;
            chrome->scroll_direction = 0;
            chrome_save_state(state);
            chrome_request_frame(state);
        }
    }
}

static void chrome_load_state(AppState *state, const char *initial_url)
{
    BrowserChrome *chrome = &state->chrome;
    chrome->enabled = env_enabled("WPE_CHROME_ENABLED", TRUE);
    chrome->visible = TRUE;
    chrome->height = (int)env_double("WPE_CHROME_HEIGHT", 44);
    if (chrome->height < 24)
        chrome->height = 24;
    if (chrome->height > 80)
        chrome->height = 80;
    chrome->hide_down_px = env_double("WPE_CHROME_HIDE_DOWN_PX", 96);
    chrome->show_up_px = env_double("WPE_CHROME_SHOW_UP_PX", 180);
    chrome->show_up_min_velocity_px_s = env_double("WPE_CHROME_SHOW_UP_MIN_VELOCITY", 700);
    chrome->render_state_path = g_strdup(g_getenv("WPE_CHROME_RENDER_STATE") && g_getenv("WPE_CHROME_RENDER_STATE")[0] ? g_getenv("WPE_CHROME_RENDER_STATE") : "/tmp/wpe-drm2-chrome-state.ini");
    chrome->home_url = g_strdup(profile_runtime.profile.home_url && profile_runtime.profile.home_url[0]
        ? profile_runtime.profile.home_url : default_home_url());
    g_strlcpy(chrome->site_profile,
              normalize_site_profile(profile_runtime.profile.site_profile),
              sizeof(chrome->site_profile));
    chrome->cookie_policy = profile_runtime.profile.cookie_policy;
    chrome->tab_count = 1;
    chrome->active = 0;
    chrome->next_tab_id = 2;
    chrome->tabs[0].id = 1;
    chrome_set_tab_url(&chrome->tabs[0], initial_url && initial_url[0] ? initial_url : chrome->home_url);
    g_print("Chrome config: enabled=%d height=%d hide_down=%.1f show_up=%.1f show_min_velocity=%.1f\n",
            chrome->enabled, chrome->height,
            chrome->hide_down_px, chrome->show_up_px, chrome->show_up_min_velocity_px_s);

    if (!profile_runtime.guest && profile_runtime.store) {
        GError *error = NULL;
        guint next_tab_id = 2;
        GPtrArray *stored_tabs = browser_profile_store_load_tabs(profile_runtime.store,
            profile_runtime.profile.id, &next_tab_id, &error);
        if (!stored_tabs)
            g_warning("Profile tabs load failed: %s", error ? error->message : "unknown");
        else if (stored_tabs->len) {
            chrome_clear_tab(&chrome->tabs[0]);
            chrome->tab_count = MIN((int)stored_tabs->len, MAX_BROWSER_TABS);
            chrome->active = 0;
            chrome->next_tab_id = MAX(2, next_tab_id);
            for (int index = 0; index < chrome->tab_count; ++index) {
                BrowserStoredTab *stored = g_ptr_array_index(stored_tabs, index);
                BrowserTab *tab = &chrome->tabs[index];
                tab->id = stored->logical_id ? stored->logical_id : (guint)index + 1;
                chrome_set_tab_url(tab, stored->url && stored->url[0] ? stored->url : chrome->home_url);
                if (stored->title)
                    g_strlcpy(tab->title, stored->title, sizeof(tab->title));
                for (guint item = 0; stored->back && item < stored->back->len
                        && tab->back_count < MAX_BROWSER_HISTORY; ++item)
                    tab->back[tab->back_count++] = g_strdup(g_ptr_array_index(stored->back, item));
                for (guint item = 0; stored->forward && item < stored->forward->len
                        && tab->forward_count < MAX_BROWSER_HISTORY; ++item)
                    tab->forward[tab->forward_count++] = g_strdup(g_ptr_array_index(stored->forward, item));
                if (stored->active)
                    chrome->active = index;
            }
        }
        g_clear_error(&error);
        g_clear_pointer(&stored_tabs, g_ptr_array_unref);
    }
    g_print("Chrome profile state: name=%s guest=%d home=%s site_profile=%s cookie_policy=%s tabs=%d active=%d\n",
            profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
            profile_runtime.guest, chrome->home_url,
            chrome->site_profile, browser_cookie_policy_name(chrome->cookie_policy),
            chrome->tab_count, chrome->active);
    chrome_update_render_state(state);
}

static void chrome_destroy(AppState *state)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    if (runtime_exit_code != 75)
        chrome_save_state(state);
    else
        chrome_update_render_state(state);
    if (chrome->layout_timer) {
        g_source_remove(chrome->layout_timer);
        chrome->layout_timer = 0;
    }
    for (int i = 0; i < chrome->tab_count; ++i)
        chrome_clear_tab(&chrome->tabs[i]);
    g_clear_pointer(&chrome->panel_profiles, g_ptr_array_unref);
    g_clear_pointer(&chrome->panel_pages, g_ptr_array_unref);
    g_free(chrome->render_state_path);
    g_free(chrome->home_url);
}

static void apply_viewport(AppState *state, int width, int height, const char *reason)
{
    if (!state || !state->view || width <= 0 || height <= 0)
        return;
    if (state->viewport_width == width && state->viewport_height == height)
        return;

    gboolean resize_ok = FALSE;
    if (state->toplevel) {
        resize_ok = wpe_toplevel_resize(state->toplevel, width, height);
        if (!resize_ok)
            wpe_toplevel_resized(state->toplevel, width, height);
    }
    wpe_view_resized(state->view, width, height);

    state->viewport_width = width;
    state->viewport_height = height;
    g_print("Viewport applied: %dx%d reason=%s resize_ok=%d view=%dx%d\n",
            width, height, reason ? reason : "unknown", resize_ok,
            wpe_view_get_width(state->view), wpe_view_get_height(state->view));
}

static void ensure_panel_size(AppState *state)
{
    if (!state)
        return;
    if (state->panel_width > 0 && state->panel_height > 0)
        return;
    if (!parse_viewport_string(g_getenv("WPE_PANEL_SIZE"), &state->panel_width, &state->panel_height)) {
        if (!parse_viewport_string(g_getenv("WPE_VIEWPORT"), &state->panel_width, &state->panel_height)) {
            state->panel_width = state->viewport_width > 0 ? state->viewport_width : (state->view ? wpe_view_get_width(state->view) : 960);
            state->panel_height = state->viewport_height > 0 ? state->viewport_height : (state->view ? wpe_view_get_height(state->view) : 266);
        }
    }
}

static gboolean chrome_deferred_layout_cb(gpointer user_data)
{
    AppState *state = user_data;
    state->chrome.layout_timer = 0;
    chrome_apply_layout(state, "deferred");
    return G_SOURCE_REMOVE;
}

static void chrome_apply_layout(AppState *state, const char *reason)
{
    if (!state || !state->view || !state->chrome.enabled || !chrome_layout_resize_enabled())
        return;

    ensure_panel_size(state);
    int panel_width = state->panel_width > 0 ? state->panel_width : 960;
    int panel_height = state->panel_height > 0 ? state->panel_height : 266;
    int top_inset = chrome_page_top_inset(state);
    int content_height = panel_height - top_inset;
    if (content_height < 64)
        content_height = 64;

    /* 工具栏收起（内容区变大）时把 resize 推迟到滑出动画结束：立刻 resize 会让
     * WebKit 在动画期间就为底部新增区域重排重绘，未及绘制的部分露出基底背景。
     * 展开方向内容区变小，不会露底，仍立即应用。 */
    gboolean growing = state->viewport_height > 0 && content_height > state->viewport_height;
    if (growing && !g_str_equal(reason ? reason : "", "deferred")) {
        if (state->chrome.layout_timer)
            g_source_remove(state->chrome.layout_timer);
        state->chrome.layout_timer = g_timeout_add(chrome_animation_duration_ms(),
                                                   chrome_deferred_layout_cb, state);
        return;
    }
    if (state->chrome.layout_timer) {
        g_source_remove(state->chrome.layout_timer);
        state->chrome.layout_timer = 0;
    }

    char detail[96];
    snprintf(detail, sizeof(detail), "chrome_%s inset=%d panel=%dx%d", reason ? reason : "layout", top_inset, panel_width, panel_height);
    apply_viewport(state, panel_width, content_height, detail);
}

static void init_touch_slots(AppState *state)
{
    state->current_slot = 0;
    for (int i = 0; i < MAX_TOUCH_SLOTS; ++i) {
        state->slots[i].tracking_id = -1;
        state->slots[i].raw_x = -1;
        state->slots[i].raw_y = -1;
    }
}

static gboolean read_abs_range(int fd, int code, int *min_value, int *max_value)
{
    struct input_absinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, EVIOCGABS(code), &info) < 0)
        return FALSE;
    if (info.maximum <= info.minimum)
        return FALSE;
    *min_value = info.minimum;
    *max_value = info.maximum;
    return TRUE;
}

static double clamp_double(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static void update_touch_position(AppState *state, TouchSlot *slot)
{
    int width = state->panel_width > 0 ? state->panel_width : wpe_view_get_width(state->view);
    int height = state->panel_height > 0 ? state->panel_height : wpe_view_get_height(state->view);
    if (width <= 0)
        width = state->viewport_width > 0 ? state->viewport_width : 1;
    if (height <= 0)
        height = state->viewport_height > 0 ? state->viewport_height : 1;

    int x_min = state->touch_active_x_enabled ? state->touch_active_x_min : state->abs_x_min;
    int x_max = state->touch_active_x_enabled ? state->touch_active_x_max : state->abs_x_max;
    int y_min = state->touch_active_y_enabled ? state->touch_active_y_min : state->abs_y_min;
    int y_max = state->touch_active_y_enabled ? state->touch_active_y_max : state->abs_y_max;

    double nx = 0;
    double ny = 0;
    if (x_max > x_min)
        nx = (double)(slot->raw_x - x_min) / (double)(x_max - x_min);
    if (y_max > y_min)
        ny = (double)(slot->raw_y - y_min) / (double)(y_max - y_min);

    nx = clamp_double(nx, 0, 1);
    ny = clamp_double(ny, 0, 1);

    if (state->touch_swap_xy) {
        double tmp = nx;
        nx = ny;
        ny = tmp;
    }
    if (state->touch_invert_x)
        nx = 1 - nx;
    if (state->touch_invert_y)
        ny = 1 - ny;

    double tx = nx;
    double ty = ny;
    switch (state->touch_rotation) {
    case 90:
        tx = ny;
        ty = 1 - nx;
        break;
    case 180:
        tx = 1 - nx;
        ty = 1 - ny;
        break;
    case 270:
        tx = 1 - ny;
        ty = nx;
        break;
    default:
        break;
    }

    slot->x = clamp_double(tx, 0, 1) * width;
    slot->y = clamp_double(ty, 0, 1) * height;
}

static guint32 input_event_time_ms(const struct input_event *event)
{
    return (guint32)(((uint64_t)event->time.tv_sec * 1000 + (uint64_t)event->time.tv_usec / 1000) & 0xffffffffu);
}

static gboolean native_scroll_tick(gpointer user_data);
static gboolean scroll_stop_tick(gpointer user_data);

static double clamp_step(double value, double max_abs)
{
    if (max_abs <= 0)
        return value;
    if (value > max_abs)
        return max_abs;
    if (value < -max_abs)
        return -max_abs;
    return value;
}

static void schedule_native_scroll(AppState *state)
{
    if (state->native_scroll_scheduled)
        return;
    state->native_scroll_scheduled = TRUE;
    guint interval = state->touch_scroll_interval_ms > 0 ? (guint)state->touch_scroll_interval_ms : 16;
    state->native_scroll_source_id = g_timeout_add(interval, native_scroll_tick, state);
}

static void schedule_scroll_stop(AppState *state)
{
    if (!state->scroll_active)
        return;
    if (state->scroll_stop_source_id)
        g_source_remove(state->scroll_stop_source_id);
    guint delay = state->touch_scroll_stop_delay_ms > 0 ? (guint)state->touch_scroll_stop_delay_ms : 80;
    state->scroll_stop_source_id = g_timeout_add(delay, scroll_stop_tick, state);
}

static void send_touch_event(AppState *state, WPEEventType type, guint32 sequence_id, double x, double y, guint32 time_ms)
{
    if (!state->send_touch_events && !state->game_input_active)
        return;

    double view_y = web_event_y(state, y);
    WPEEvent *event = wpe_event_touch_new(type, state->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time_ms, 0, sequence_id, x, view_y);
    wpe_view_event(state->view, event);
    wpe_event_unref(event);

    state->touch_event_count++;
    if (state->touch_event_count <= 8 || !(state->touch_event_count % 80))
        g_print("Touch event: type=%d seq=%u x=%.1f y=%.1f screen_y=%.1f count=%" G_GUINT64_FORMAT "\n",
                type, sequence_id, x, view_y, y, state->touch_event_count);
}

static void send_pointer_tap(AppState *state, double x, double y, guint32 time_ms)
{
    if (!state->synthesize_pointer_tap)
        return;

    state->last_pointer_tap_us = g_get_monotonic_time();
    wpe_view_focus_in(state->view);
    double view_y = web_event_y(state, y);

    WPEEvent *move = wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, state->view,
                                                WPE_INPUT_SOURCE_MOUSE, time_ms,
                                                0, x, view_y, 0, 0);
    if (move) {
        wpe_view_event(state->view, move);
        wpe_event_unref(move);
    }

    WPEEvent *down = wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, state->view,
                                                  WPE_INPUT_SOURCE_MOUSE, time_ms,
                                                  WPE_MODIFIER_POINTER_BUTTON1,
                                                  WPE_BUTTON_PRIMARY, x, view_y, 1);
    if (down) {
        wpe_view_event(state->view, down);
        wpe_event_unref(down);
    }

    WPEEvent *up = wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, state->view,
                                                WPE_INPUT_SOURCE_MOUSE, time_ms,
                                                0, WPE_BUTTON_PRIMARY, x, view_y, 0);
    if (up) {
        wpe_view_event(state->view, up);
        wpe_event_unref(up);
    }

    g_print("Pointer tap: x=%.1f y=%.1f screen_y=%.1f\n", x, view_y, y);
}

static void send_js_scroll_event(AppState *state, double x, double y, double delta_y)
{
    if (!state->touch_js_scroll_fallback || !state->web_view)
        return;
    if (delta_y > -0.5 && delta_y < 0.5)
        return;

    char xbuf[G_ASCII_DTOSTR_BUF_SIZE];
    char ybuf[G_ASCII_DTOSTR_BUF_SIZE];
    char dybuf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(xbuf, sizeof(xbuf), x);
    g_ascii_dtostr(ybuf, sizeof(ybuf), web_event_y(state, y));
    g_ascii_dtostr(dybuf, sizeof(dybuf), -delta_y);

    char *script = g_strdup_printf(
        "(function(){"
        "var x=%s,y=%s,dy=%s;"
        "var e=document.elementFromPoint(x,y);"
        "function scrollable(n){"
        " if(!n||n===document||n===document.documentElement||n===document.body)return false;"
        " var s=getComputedStyle(n);"
        " var oy=(s.overflowY||'')+(s.overflow||'');"
        " return /(auto|scroll|overlay)/.test(oy)&&n.scrollHeight>n.clientHeight+1;"
        "}"
        "for(var n=e;n;n=n.parentElement){"
        " if(scrollable(n)){var b=n.scrollTop;n.scrollTop+=dy;if(n.scrollTop!==b)return 1;}"
        "}"
        "window.scrollBy(0,dy);"
        "return 0;"
        "})()",
        xbuf, ybuf, dybuf);
    webkit_web_view_evaluate_javascript(state->web_view, script, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(script);
}

static void maybe_log_scroll_stats(AppState *state, const char *mode, double delta_x, double delta_y)
{
    gint64 now_us = g_get_monotonic_time();
    if (!state->scroll_stats_last_us) {
        state->scroll_stats_last_us = now_us;
        state->scroll_stats_last_event_count = state->scroll_event_count;
        state->scroll_stats_last_frame_count = frame_count;
        return;
    }

    gint64 elapsed_us = now_us - state->scroll_stats_last_us;
    if (elapsed_us < G_USEC_PER_SEC && state->scroll_event_count > 8)
        return;
    if (elapsed_us <= 0)
        elapsed_us = 1;

    guint64 scroll_delta = state->scroll_event_count - state->scroll_stats_last_event_count;
    guint64 frame_delta = frame_count - state->scroll_stats_last_frame_count;
    double fps = (double)frame_delta * G_USEC_PER_SEC / (double)elapsed_us;
    g_print("Scroll stats: mode=%s dx=%.1f dy=%.1f total=%" G_GUINT64_FORMAT " +%" G_GUINT64_FORMAT
            " frame=%" G_GUINT64_FORMAT " +%" G_GUINT64_FORMAT " fps=%.1f pending=%.1f,%.1f native=%d js=%d\n",
            mode, delta_x, delta_y, state->scroll_event_count, scroll_delta,
            frame_count, frame_delta, fps,
            state->pending_native_scroll_x, state->pending_native_scroll_y,
            state->touch_native_scroll_fallback, state->touch_js_scroll_fallback);

    state->scroll_stats_last_us = now_us;
    state->scroll_stats_last_event_count = state->scroll_event_count;
    state->scroll_stats_last_frame_count = frame_count;
}

static void send_scroll_event_now(AppState *state, double x, double y, double delta_x, double delta_y, guint32 time_ms, gboolean is_stop)
{
    double view_y = web_event_y(state, y);
    if (state->touch_native_scroll_fallback) {
        WPEEvent *event = wpe_event_scroll_new(state->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time_ms, 0,
                                               delta_x, delta_y, TRUE, is_stop, x, view_y);
        wpe_view_event(state->view, event);
        wpe_event_unref(event);
    }

    if (is_stop) {
        state->scroll_active = FALSE;
        if (state->touch_native_scroll_fallback)
            g_print("Scroll fallback stop: count=%" G_GUINT64_FORMAT "\n", state->scroll_event_count);
        return;
    }

    state->scroll_active = TRUE;
    send_js_scroll_event(state, x, y, delta_y);
    if (state->touch_native_scroll_fallback) {
        state->scroll_event_count++;
        if (state->scroll_event_count <= 8 || !(state->scroll_event_count % 60))
            g_print("Native scroll: dx=%.1f dy=%.1f count=%" G_GUINT64_FORMAT "\n",
                    delta_x, delta_y, state->scroll_event_count);
        maybe_log_scroll_stats(state, "native", delta_x, delta_y);
    } else if (state->touch_js_scroll_fallback) {
        state->scroll_event_count++;
        if (state->scroll_event_count <= 8 || !(state->scroll_event_count % 60))
            g_print("JS scroll fallback: dy=%.1f count=%" G_GUINT64_FORMAT "\n",
                    delta_y, state->scroll_event_count);
        maybe_log_scroll_stats(state, "js", delta_x, delta_y);
    }

    schedule_scroll_stop(state);
}

static gboolean scroll_stop_tick(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    state->scroll_stop_source_id = 0;
    if (!state->scroll_active)
        return G_SOURCE_REMOVE;
    if (state->pending_native_scroll_x <= -0.5 || state->pending_native_scroll_x >= 0.5 ||
        state->pending_native_scroll_y <= -0.5 || state->pending_native_scroll_y >= 0.5) {
        schedule_scroll_stop(state);
        return G_SOURCE_REMOVE;
    }
    send_scroll_event_now(state, state->last_scroll_x, state->last_scroll_y, 0, 0, state->last_scroll_time_ms, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean native_scroll_tick(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    state->native_scroll_scheduled = FALSE;
    state->native_scroll_source_id = 0;

    double delta_x = state->pending_native_scroll_x;
    double delta_y = state->pending_native_scroll_y;
    if (delta_x > -0.5 && delta_x < 0.5 && delta_y > -0.5 && delta_y < 0.5) {
        state->pending_native_scroll_x = 0;
        state->pending_native_scroll_y = 0;
        return G_SOURCE_REMOVE;
    }

    double send_x = clamp_step(delta_x, state->touch_scroll_max_step);
    double send_y = clamp_step(delta_y, state->touch_scroll_max_step);
    state->pending_native_scroll_x -= send_x;
    state->pending_native_scroll_y -= send_y;

    send_scroll_event_now(state, state->last_scroll_x, state->last_scroll_y, send_x, send_y, state->last_scroll_time_ms, FALSE);

    if (state->pending_native_scroll_x <= -0.5 || state->pending_native_scroll_x >= 0.5 ||
        state->pending_native_scroll_y <= -0.5 || state->pending_native_scroll_y >= 0.5)
        schedule_native_scroll(state);

    return G_SOURCE_REMOVE;
}

static void queue_scroll_event(AppState *state, double x, double y, double delta_x, double delta_y, guint32 time_ms)
{
    if (!state->touch_scroll_fallback)
        return;
    if (delta_x > -0.5 && delta_x < 0.5 && delta_y > -0.5 && delta_y < 0.5)
        return;

    if (!state->touch_horizontal_scroll)
        delta_x = 0;
    chrome_note_scroll(state, delta_y, time_ms);
    delta_x *= state->touch_scroll_scale;
    delta_y *= state->touch_scroll_scale;
    /* delta 来自 update_touch_position 旋转映射后的 panel 坐标，已是屏幕方向，
     * 不能再按 panel_rotation 取反（历史上此处的二次取反靠 C 默认
     * INVERT_Y=TRUE 抵消，而 run.sh 固定 export 0，导致 90/270 度滚动反向）。
     * WPE_TOUCH_SCROLL_INVERT_Y 仅作为面向用户的手动覆盖保留。 */
    if (state->touch_scroll_invert_y)
        delta_y = -delta_y;

    double pending_limit = state->touch_scroll_pending_limit > 0 ? state->touch_scroll_pending_limit : state->touch_scroll_max_step * 2.0;
    double next_x = state->pending_native_scroll_x + delta_x;
    double next_y = state->pending_native_scroll_y + delta_y;
    double clamped_x = clamp_step(next_x, pending_limit);
    double clamped_y = clamp_step(next_y, pending_limit);
    if (clamped_x != next_x || clamped_y != next_y) {
        state->scroll_drop_count++;
        if (state->scroll_drop_count <= 8 || !(state->scroll_drop_count % 60))
            g_print("Scroll backlog clamp: pending=(%.1f,%.1f) limit=%.1f drops=%" G_GUINT64_FORMAT "\n",
                    next_x, next_y, pending_limit, state->scroll_drop_count);
    }
    state->pending_native_scroll_x = clamped_x;
    state->pending_native_scroll_y = clamped_y;
    state->last_scroll_x = x;
    state->last_scroll_y = y;
    state->last_scroll_time_ms = time_ms;
    schedule_native_scroll(state);
}

static void process_touch_syn(AppState *state, guint32 time_ms)
{
    for (int i = 0; i < MAX_TOUCH_SLOTS; ++i) {
        TouchSlot *slot = &state->slots[i];
        if (!slot->just_down && !slot->just_up && !slot->active)
            continue;
        if (slot->raw_x < 0 || slot->raw_y < 0)
            continue;

        update_touch_position(state, slot);

        if (slot->just_down) {
            if (state->native_scroll_source_id) {
                g_source_remove(state->native_scroll_source_id);
                state->native_scroll_source_id = 0;
                state->native_scroll_scheduled = FALSE;
            }
            if (state->scroll_stop_source_id) {
                g_source_remove(state->scroll_stop_source_id);
                state->scroll_stop_source_id = 0;
            }
            state->pending_native_scroll_x = 0;
            state->pending_native_scroll_y = 0;
            state->scroll_active = FALSE;
            state->chrome.scroll_accum = 0;
            state->chrome.scroll_direction = 0;
            slot->last_x = slot->x;
            slot->last_y = slot->y;
            slot->down_x = slot->x;
            slot->down_y = slot->y;
            slot->max_move_sq = 0;
            slot->scrolling = FALSE;
            slot->chrome_consumed = chrome_touch_down_consumes(state, slot->x, slot->y);
            slot->tabs_scroll_candidate = FALSE;
            if (slot->chrome_consumed && state->chrome.panel == CHROME_PANEL_TABS) {
                ChromeTabsGeometry geometry = chrome_tabs_geometry(state);
                slot->tabs_scroll_candidate = slot->x >= geometry.panel_x
                    && slot->x < geometry.panel_x + geometry.panel_width
                    && slot->y >= geometry.list_top
                    && slot->y < geometry.footer_top;
            }
            g_print("Touch down: raw=%d,%d mapped=%.1f,%.1f chrome=%d visible=%d panel=%s\n",
                    slot->raw_x, slot->raw_y, slot->x, slot->y, slot->chrome_consumed,
                    state->chrome.visible, chrome_panel_name(state->chrome.panel));
            if (!slot->chrome_consumed && (state->game_input_active || !state->touch_scroll_fallback))
                send_touch_event(state, WPE_EVENT_TOUCH_DOWN, i, slot->x, slot->y, time_ms);
        } else if (slot->just_up) {
            if (!slot->chrome_consumed && (state->game_input_active || !state->touch_scroll_fallback))
                send_touch_event(state, WPE_EVENT_TOUCH_UP, i, slot->last_x, slot->last_y, time_ms);
            double tap_limit = state->touch_tap_max_move > 0 ? state->touch_tap_max_move : 24;
            if (slot->chrome_consumed) {
                double chrome_tap_limit = env_double("WPE_CHROME_TAP_MAX_MOVE", 32);
                if (chrome_tap_limit > tap_limit)
                    tap_limit = chrome_tap_limit;
            }
            if (slot->chrome_consumed && slot->tabs_scroll_candidate && slot->scrolling) {
                chrome_clamp_tabs_scroll(state);
                chrome_update_render_state(state);
                chrome_request_frame(state);
            } else if (!slot->scrolling && slot->max_move_sq <= tap_limit * tap_limit) {
                if (slot->chrome_consumed)
                    chrome_handle_toolbar_tap(state, slot->last_x, slot->last_y);
                else if (!state->game_input_active)
                    send_pointer_tap(state, slot->last_x, slot->last_y, time_ms);
            }
            else {
                state->pending_native_scroll_x = 0;
                state->pending_native_scroll_y = 0;
                if (state->scroll_stop_source_id) {
                    g_source_remove(state->scroll_stop_source_id);
                    state->scroll_stop_source_id = 0;
                }
                if (state->scroll_active)
                    send_scroll_event_now(state, slot->last_x, slot->last_y, 0, 0, time_ms, TRUE);
            }
            slot->scrolling = FALSE;
            slot->chrome_consumed = FALSE;
            slot->tabs_scroll_candidate = FALSE;
            slot->tracking_id = -1;
        } else if (slot->active && (slot->x != slot->last_x || slot->y != slot->last_y)) {
            if (!slot->chrome_consumed && (state->game_input_active || !state->touch_scroll_fallback))
                send_touch_event(state, WPE_EVENT_TOUCH_MOVE, i, slot->x, slot->y, time_ms);
            double from_down_x = slot->x - slot->down_x;
            double from_down_y = slot->y - slot->down_y;
            double move_sq = from_down_x * from_down_x + from_down_y * from_down_y;
            if (move_sq > slot->max_move_sq)
                slot->max_move_sq = move_sq;
            double tap_limit = state->touch_tap_max_move > 0 ? state->touch_tap_max_move : 24;
            if (slot->chrome_consumed && slot->tabs_scroll_candidate) {
                double panel_drag_threshold = env_double("WPE_CHROME_PANEL_DRAG_PX", 8);
                if (slot->scrolling || slot->max_move_sq > panel_drag_threshold * panel_drag_threshold) {
                    slot->scrolling = TRUE;
                    state->chrome.tabs_scroll_offset -= slot->y - slot->last_y;
                    chrome_clamp_tabs_scroll(state);
                    gint64 now_us = g_get_monotonic_time();
                    if (now_us - state->chrome.tabs_scroll_last_publish_us >= 16000) {
                        state->chrome.tabs_scroll_last_publish_us = now_us;
                        chrome_update_render_state(state);
                        chrome_request_frame(state);
                    }
                }
            } else if (!slot->chrome_consumed && !state->game_input_active) {
                if (slot->scrolling) {
                    queue_scroll_event(state, slot->x, slot->y, slot->x - slot->last_x, slot->y - slot->last_y, time_ms);
                } else if (slot->max_move_sq > tap_limit * tap_limit) {
                    slot->scrolling = TRUE;
                    queue_scroll_event(state, slot->x, slot->y, slot->x - slot->last_x, slot->y - slot->last_y, time_ms);
                }
            }
            slot->last_x = slot->x;
            slot->last_y = slot->y;
        }

        slot->just_down = FALSE;
        slot->just_up = FALSE;
        if (!slot->active)
            slot->tracking_id = -1;
    }
}

static gboolean touch_io_cb(gint fd, GIOCondition condition, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        g_warning("Touch fd closed: condition=0x%x", condition);
        state->touch_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    struct input_event event;
    while (read(fd, &event, sizeof(event)) == (ssize_t)sizeof(event)) {
        guint32 time_ms = input_event_time_ms(&event);
        TouchSlot *slot = &state->slots[state->current_slot];

        if (event.type == EV_ABS) {
            if (event.code == ABS_MT_SLOT) {
                if (event.value >= 0 && event.value < MAX_TOUCH_SLOTS)
                    state->current_slot = event.value;
                continue;
            }

            slot = &state->slots[state->current_slot];
            if (event.code == ABS_MT_TRACKING_ID) {
                if (event.value >= 0) {
                    slot->tracking_id = event.value;
                    slot->active = TRUE;
                    slot->just_down = TRUE;
                    slot->just_up = FALSE;
                } else if (slot->active || slot->tracking_id >= 0) {
                    slot->active = FALSE;
                    slot->just_up = TRUE;
                    slot->just_down = FALSE;
                }
            } else if (event.code == state->abs_x_code)
                slot->raw_x = event.value;
            else if (event.code == state->abs_y_code)
                slot->raw_y = event.value;
        } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
            slot = &state->slots[0];
            state->current_slot = 0;
            if (event.value) {
                slot->tracking_id = 0;
                slot->active = TRUE;
                slot->just_down = TRUE;
                slot->just_up = FALSE;
            } else if (slot->active || slot->tracking_id >= 0) {
                slot->active = FALSE;
                slot->just_up = TRUE;
                slot->just_down = FALSE;
            }
        } else if (event.type == EV_SYN && event.code == SYN_REPORT)
            process_touch_syn(state, time_ms);
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK)
        g_warning("Touch read failed: errno=%d (%s)", errno, strerror(errno));
    return G_SOURCE_CONTINUE;
}

static void setup_raw_touch(AppState *state)
{
    if (!env_enabled("WPE_RAW_TOUCH", TRUE)) {
        g_print("Raw touch disabled by WPE_RAW_TOUCH\n");
        return;
    }

    const char *device = g_getenv("WPE_TOUCH_DEVICE");
    if (!device || !device[0]) {
        g_warning("Raw touch disabled: WPE_TOUCH_DEVICE is empty");
        return;
    }

    int fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_warning("Raw touch open failed: %s errno=%d (%s)", device, errno, strerror(errno));
        return;
    }

    state->touch_fd = fd;
    state->abs_x_code = ABS_MT_POSITION_X;
    state->abs_y_code = ABS_MT_POSITION_Y;
    if (!read_abs_range(fd, ABS_MT_POSITION_X, &state->abs_x_min, &state->abs_x_max) ||
        !read_abs_range(fd, ABS_MT_POSITION_Y, &state->abs_y_min, &state->abs_y_max)) {
        state->abs_x_code = ABS_X;
        state->abs_y_code = ABS_Y;
        if (!read_abs_range(fd, ABS_X, &state->abs_x_min, &state->abs_x_max) ||
            !read_abs_range(fd, ABS_Y, &state->abs_y_min, &state->abs_y_max)) {
            g_warning("Raw touch has no usable ABS ranges");
            close(fd);
            state->touch_fd = -1;
            return;
        }
    }

    state->touch_swap_xy = env_enabled("WPE_TOUCH_SWAP_XY", FALSE);
    state->touch_invert_x = env_enabled("WPE_TOUCH_INVERT_X", FALSE);
    state->touch_invert_y = env_enabled("WPE_TOUCH_INVERT_Y", FALSE);
    state->touch_active_x_enabled = parse_int_range(g_getenv("WPE_TOUCH_ACTIVE_X"), &state->touch_active_x_min, &state->touch_active_x_max);
    state->touch_active_y_enabled = parse_int_range(g_getenv("WPE_TOUCH_ACTIVE_Y"), &state->touch_active_y_min, &state->touch_active_y_max);
    if (state->touch_active_x_enabled) {
        state->touch_active_x_min = CLAMP(state->touch_active_x_min, state->abs_x_min, state->abs_x_max);
        state->touch_active_x_max = CLAMP(state->touch_active_x_max, state->abs_x_min, state->abs_x_max);
        if (state->touch_active_x_max <= state->touch_active_x_min)
            state->touch_active_x_enabled = FALSE;
    }
    if (state->touch_active_y_enabled) {
        state->touch_active_y_min = CLAMP(state->touch_active_y_min, state->abs_y_min, state->abs_y_max);
        state->touch_active_y_max = CLAMP(state->touch_active_y_max, state->abs_y_min, state->abs_y_max);
        if (state->touch_active_y_max <= state->touch_active_y_min)
            state->touch_active_y_enabled = FALSE;
    }
    if (!parse_viewport_string(g_getenv("WPE_PANEL_SIZE"), &state->panel_width, &state->panel_height)) {
        if (!parse_viewport_string(g_getenv("WPE_VIEWPORT"), &state->panel_width, &state->panel_height)) {
            state->panel_width = state->viewport_width > 0 ? state->viewport_width : wpe_view_get_width(state->view);
            state->panel_height = state->viewport_height > 0 ? state->viewport_height : wpe_view_get_height(state->view);
        }
    }
    state->panel_rotation = normalize_rotation_degrees((int)env_double("WPE_PANEL_ROTATION", 0));
    state->touch_rotation = normalize_rotation_degrees((int)env_double("WPE_TOUCH_ROTATION", state->panel_rotation));
    state->touch_offset_x = (int)env_double("WPE_TOUCH_OFFSET_X", 0);
    state->touch_offset_y = (int)env_double("WPE_TOUCH_OFFSET_Y", 0);
    if (!state->touch_active_x_enabled && state->touch_offset_x) {
        state->touch_active_x_enabled = TRUE;
        state->touch_active_x_min = CLAMP(state->abs_x_min + state->touch_offset_x, state->abs_x_min, state->abs_x_max);
        state->touch_active_x_max = state->abs_x_max;
        if (state->touch_active_x_max <= state->touch_active_x_min)
            state->touch_active_x_enabled = FALSE;
    }
    if (!state->touch_active_y_enabled && state->touch_offset_y) {
        state->touch_active_y_enabled = TRUE;
        state->touch_active_y_min = CLAMP(state->abs_y_min + state->touch_offset_y, state->abs_y_min, state->abs_y_max);
        state->touch_active_y_max = state->abs_y_max;
        if (state->touch_active_y_max <= state->touch_active_y_min)
            state->touch_active_y_enabled = FALSE;
    }
    /* 默认值以 run.sh 为唯一事实来源;此处兜底值必须与 run.sh 的 export 保持一致,
     * 仅在绕过 run.sh 直接启动本程序时生效。
     * 注意:生产 run.sh 固定 WPE_SEND_TOUCH_EVENTS=0 且 WPE_TOUCH_SCROLL_FALLBACK=1,
     * 因此 send_touch_event 原生触摸事件路径仅作为调试开关保留。 */
    state->send_touch_events = env_enabled("WPE_SEND_TOUCH_EVENTS", FALSE);
    state->synthesize_pointer_tap = env_enabled("WPE_SYNTHESIZE_POINTER_TAP", TRUE);
    state->touch_scroll_fallback = env_enabled("WPE_TOUCH_SCROLL_FALLBACK", TRUE);
    state->touch_native_scroll_fallback = env_enabled("WPE_TOUCH_NATIVE_SCROLL", TRUE);
    state->touch_js_scroll_fallback = env_enabled("WPE_TOUCH_JS_SCROLL", FALSE);
    state->touch_horizontal_scroll = env_enabled("WPE_TOUCH_HORIZONTAL_SCROLL", FALSE);
    state->touch_scroll_invert_y = env_enabled("WPE_TOUCH_SCROLL_INVERT_Y", FALSE);
    state->touch_scroll_scale = env_double("WPE_TOUCH_SCROLL_SCALE", 1.0);
    state->touch_scroll_max_step = env_double("WPE_TOUCH_SCROLL_MAX_STEP", 32);
    state->touch_scroll_pending_limit = env_double("WPE_TOUCH_SCROLL_PENDING_LIMIT", 64);
    state->touch_tap_max_move = env_double("WPE_TOUCH_TAP_MAX_MOVE", 32);
    state->touch_scroll_interval_ms = (int)env_double("WPE_TOUCH_SCROLL_INTERVAL_MS", 16);
    state->touch_scroll_stop_delay_ms = (int)env_double("WPE_TOUCH_SCROLL_STOP_DELAY_MS", 80);
    init_touch_slots(state);

    state->touch_source_id = g_unix_fd_add(fd, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL, touch_io_cb, state);
    g_print("Raw touch: device=%s fd=%d panel=%dx%d panel_rotation=%d touch_rotation=%d touch_offset=%d,%d x_code=%d range=%d..%d active_x=%s%d..%d y_code=%d range=%d..%d active_y=%s%d..%d swap=%d inv_x=%d inv_y=%d send_touch=%d pointer_tap=%d scroll_fallback=%d native_scroll=%d js_scroll=%d hscroll=%d scroll_invert_y=%d scroll_scale=%.2f max_step=%.1f pending_limit=%.1f tap_max=%.1f interval=%dms stop_delay=%dms\n",
            device, fd,
            state->panel_width, state->panel_height, state->panel_rotation,
            state->touch_rotation, state->touch_offset_x, state->touch_offset_y,
            state->abs_x_code, state->abs_x_min, state->abs_x_max,
            state->touch_active_x_enabled ? "" : "off:",
            state->touch_active_x_enabled ? state->touch_active_x_min : state->abs_x_min,
            state->touch_active_x_enabled ? state->touch_active_x_max : state->abs_x_max,
            state->abs_y_code, state->abs_y_min, state->abs_y_max,
            state->touch_active_y_enabled ? "" : "off:",
            state->touch_active_y_enabled ? state->touch_active_y_min : state->abs_y_min,
            state->touch_active_y_enabled ? state->touch_active_y_max : state->abs_y_max,
            state->touch_swap_xy, state->touch_invert_x, state->touch_invert_y,
            state->send_touch_events, state->synthesize_pointer_tap,
            state->touch_scroll_fallback, state->touch_native_scroll_fallback,
            state->touch_js_scroll_fallback,
            state->touch_horizontal_scroll, state->touch_scroll_invert_y, state->touch_scroll_scale,
            state->touch_scroll_max_step, state->touch_scroll_pending_limit,
            state->touch_tap_max_move, state->touch_scroll_interval_ms,
            state->touch_scroll_stop_delay_ms);
}


static gboolean profile_runtime_initialize(GError **error)
{
    const char *var_dir = g_getenv("WPE_VAR_DIR");
    if (!var_dir || !var_dir[0])
        var_dir = "/tmp/wpe-drm2-var";
    char *database_path = g_strdup(g_getenv("WPE_BROWSER_DB"));
    if (!database_path || !database_path[0]) {
        g_free(database_path);
        database_path = g_build_filename(var_dir, "browser.sqlite3", NULL);
    }
    char *profiles_directory = g_strdup(g_getenv("WPE_PROFILES_DIR"));
    if (!profiles_directory || !profiles_directory[0]) {
        g_free(profiles_directory);
        profiles_directory = g_build_filename(var_dir, "profiles", NULL);
    }
    const char *legacy_state = g_getenv("WPE_CHROME_STATE");
    char *legacy_state_default = NULL;
    if (!legacy_state || !legacy_state[0]) {
        legacy_state_default = g_build_filename(var_dir, "browser-state.ini", NULL);
        legacy_state = legacy_state_default;
    }
    const char *legacy_runtime = g_getenv("WPE_DRM_RUNTIME_DIR");
    char *legacy_runtime_default = NULL;
    if (!legacy_runtime || !legacy_runtime[0]) {
        legacy_runtime_default = g_build_filename(var_dir, "runtime-tmp", NULL);
        legacy_runtime = legacy_runtime_default;
    }

    profile_runtime.store = browser_profile_store_open(database_path, profiles_directory,
                                                       legacy_state, legacy_runtime, error);
    g_free(database_path);
    g_free(profiles_directory);
    g_free(legacy_state_default);
    g_free(legacy_runtime_default);
    if (!profile_runtime.store)
        return FALSE;

    const char *override = g_getenv("WPE_PROFILE_OVERRIDE");
    if (override && !g_ascii_strcasecmp(override, "guest")) {
        profile_runtime.guest = TRUE;
        profile_runtime.profile.id = 0;
        profile_runtime.profile.name = g_strdup("GUEST");
        profile_runtime.profile.home_url = g_strdup(default_home_url());
        profile_runtime.profile.site_profile = g_strdup("mobile");
        profile_runtime.profile.cookie_policy = BROWSER_COOKIE_ACCEPT_NEVER;
    } else {
        gboolean loaded = FALSE;
        if (override && override[0]) {
            char *end = NULL;
            int64_t profile_id = g_ascii_strtoll(override, &end, 10);
            if (end && !*end && profile_id > 0) {
                if (!browser_profile_store_get_profile(profile_runtime.store, profile_id,
                                                       &profile_runtime.profile, error))
                    return FALSE;
                loaded = TRUE;
            }
            if (loaded && !browser_profile_store_set_active_profile(profile_runtime.store,
                                                                    profile_id, error)) {
                browser_profile_clear(&profile_runtime.profile);
                return FALSE;
            }
        }
        if (!loaded && !browser_profile_store_get_active_profile(profile_runtime.store,
                                                                 &profile_runtime.profile, error))
            return FALSE;
        if (!browser_profile_store_set_active_profile(profile_runtime.store,
                                                      profile_runtime.profile.id, error))
            return FALSE;
    }

    if (profile_runtime.guest) {
        profile_runtime.root = g_dir_make_tmp("wpe-drm-guest-XXXXXX", error);
        if (!profile_runtime.root)
            return FALSE;
    } else
        profile_runtime.root = browser_profile_store_profile_root(profile_runtime.store,
                                                                  profile_runtime.profile.id);
    profile_runtime.runtime = g_build_filename(profile_runtime.root, "runtime", NULL);
    profile_runtime.data = g_build_filename(profile_runtime.runtime, "data", NULL);
    profile_runtime.cache = g_build_filename(profile_runtime.runtime, "cache", NULL);
    if (!profile_runtime.guest)
        profile_runtime.cookie_jar = g_build_filename(profile_runtime.root, "cookies.sqlite", NULL);
    g_mkdir_with_parents(profile_runtime.root, 0700);
    chmod(profile_runtime.root, 0700);
    g_setenv("WPE_DRM_RUNTIME_DIR", profile_runtime.runtime, TRUE);
    g_print("Browser profile: id=%" G_GINT64_FORMAT " name=%s guest=%d root=%s site_profile=%s cookie_policy=%s\n",
            profile_runtime.profile.id,
            profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
            profile_runtime.guest, profile_runtime.root,
            profile_runtime.profile.site_profile ? profile_runtime.profile.site_profile : "mobile",
            browser_cookie_policy_name(profile_runtime.profile.cookie_policy));
    return TRUE;
}

static WebKitCookieAcceptPolicy webkit_cookie_policy(BrowserCookiePolicy policy)
{
    switch (policy) {
    case BROWSER_COOKIE_NO_THIRD_PARTY:
        return WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
    case BROWSER_COOKIE_ACCEPT_NEVER:
        return WEBKIT_COOKIE_POLICY_ACCEPT_NEVER;
    case BROWSER_COOKIE_ACCEPT_ALL:
    default:
        return WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;
    }
}

static WebKitNetworkSession *profile_network_session_new(void)
{
    WebKitNetworkSession *session = NULL;
    if (profile_runtime.guest)
        session = webkit_network_session_new_ephemeral();
    else
        session = webkit_network_session_new(profile_runtime.data, profile_runtime.cache);
    if (!session)
        return NULL;

    WebKitCookieManager *cookie_manager = webkit_network_session_get_cookie_manager(session);
    if (!profile_runtime.guest) {
        webkit_cookie_manager_set_persistent_storage(cookie_manager, profile_runtime.cookie_jar,
                                                     WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        chmod(profile_runtime.cookie_jar, 0600);
    }
    webkit_cookie_manager_set_accept_policy(cookie_manager,
                                            webkit_cookie_policy(profile_runtime.profile.cookie_policy));
    g_print("Network session: ephemeral=%d data=%s cache=%s cookie_jar=%s policy=%s\n",
            profile_runtime.guest,
            profile_runtime.guest ? "(memory)" : profile_runtime.data,
            profile_runtime.guest ? "(memory)" : profile_runtime.cache,
            profile_runtime.guest ? "(memory)" : profile_runtime.cookie_jar,
            browser_cookie_policy_name(profile_runtime.profile.cookie_policy));
    return session;
}

static void profile_runtime_destroy(void)
{
    gboolean guest = profile_runtime.guest;
    char *guest_root = guest ? g_strdup(profile_runtime.root) : NULL;
    browser_profile_clear(&profile_runtime.profile);
    browser_profile_store_close(profile_runtime.store);
    g_free(profile_runtime.root);
    g_free(profile_runtime.runtime);
    g_free(profile_runtime.data);
    g_free(profile_runtime.cache);
    g_free(profile_runtime.cookie_jar);
    memset(&profile_runtime, 0, sizeof(profile_runtime));
    if (guest_root) {
        remove_dir_contents(guest_root);
        g_rmdir(guest_root);
        g_print("Guest profile removed: %s\n", guest_root);
        g_free(guest_root);
    }
}

static void prepare_runtime_dirs(void) {
    const char *base = g_getenv("WPE_DRM_RUNTIME_DIR");
    if (!base || !base[0])
        base = "/tmp/wpe-drm2-runtime";

    char *home = g_build_filename(base, "home", NULL);
    char *data = g_build_filename(base, "data", NULL);
    char *cache = g_build_filename(base, "cache", NULL);
    char *config = g_build_filename(base, "config", NULL);
    char *runtime = g_build_filename(base, "run", NULL);

    g_mkdir_with_parents(home, 0700);
    g_mkdir_with_parents(data, 0700);
    g_mkdir_with_parents(cache, 0700);
    g_mkdir_with_parents(config, 0700);
    g_mkdir_with_parents(runtime, 0700);

    g_setenv("HOME", home, TRUE);
    g_setenv("XDG_DATA_HOME", data, TRUE);
    g_setenv("XDG_CACHE_HOME", cache, TRUE);
    g_setenv("XDG_CONFIG_HOME", config, TRUE);
    g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);

    g_print("Runtime dirs: HOME=%s DATA=%s CACHE=%s CONFIG=%s RUN=%s\n",
            home, data, cache, config, runtime);

    g_free(home);
    g_free(data);
    g_free(cache);
    g_free(config);
    g_free(runtime);
}

static void configure_tls_database(void)
{
    const char *ca_file = g_getenv("G_TLS_CA_FILE");
    if (!ca_file || !ca_file[0])
        ca_file = g_getenv("SSL_CERT_FILE");
    if (!ca_file || !ca_file[0]) {
        g_warning("TLS CA file is not configured");
        return;
    }
    if (!g_file_test(ca_file, G_FILE_TEST_IS_REGULAR)) {
        g_warning("TLS CA file does not exist: %s", ca_file);
        return;
    }

    GError *error = NULL;
    GTlsDatabase *database = g_tls_file_database_new(ca_file, &error);
    if (!database) {
        g_warning("TLS CA database load failed: %s file=%s",
                  error ? error->message : "unknown", ca_file);
        g_clear_error(&error);
        return;
    }

    GTlsBackend *backend = g_tls_backend_get_default();
    if (!backend) {
        g_warning("TLS backend is unavailable for CA file: %s", ca_file);
        g_object_unref(database);
        return;
    }

    g_tls_backend_set_default_database(backend, database);
    g_print("TLS CA database: file=%s backend=%s tls=%d\n",
            ca_file,
            G_OBJECT_TYPE_NAME(backend),
            g_tls_backend_supports_tls(backend));
    g_object_unref(database);
}

static const char *load_event_name(WebKitLoadEvent event) {
    switch (event) {
    case WEBKIT_LOAD_STARTED: return "started";
    case WEBKIT_LOAD_REDIRECTED: return "redirected";
    case WEBKIT_LOAD_COMMITTED: return "committed";
    case WEBKIT_LOAD_FINISHED: return "finished";
    }
    return "unknown";
}

static const char *normalize_input_profile(const char *profile)
{
    if (profile && !g_ascii_strcasecmp(profile, "game"))
        return "game";
    if (profile && !g_ascii_strcasecmp(profile, "browser"))
        return "browser";
    return "auto";
}

static gboolean host_matches_game_allowlist(const char *host)
{
    if (!host || !host[0])
        return FALSE;
    const char *configured = g_getenv("WPE_GAME_HOSTS");
    if (!configured || !configured[0])
        configured = "ys.mihoyo.com,cloudgame.mihoyo.com";

    gchar **entries = g_strsplit(configured, ",", -1);
    gboolean matched = FALSE;
    for (guint i = 0; entries[i] && !matched; ++i) {
        char *candidate = g_strstrip(entries[i]);
        gsize host_len = strlen(host);
        gsize candidate_len = strlen(candidate);
        if (!candidate_len)
            continue;
        matched = !g_ascii_strcasecmp(host, candidate)
            || (host_len > candidate_len
                && host[host_len - candidate_len - 1] == '.'
                && !g_ascii_strcasecmp(host + host_len - candidate_len, candidate));
    }
    g_strfreev(entries);
    return matched;
}

static gboolean uri_uses_game_input(const char *uri)
{
    if (!uri || !uri[0])
        return FALSE;
    GError *error = NULL;
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        g_clear_error(&error);
        return FALSE;
    }
    gboolean matched = host_matches_game_allowlist(g_uri_get_host(parsed));
    g_uri_unref(parsed);
    return matched;
}

static void update_input_profile_for_uri(AppState *state, const char *uri)
{
    if (!state)
        return;
    const char *profile = normalize_input_profile(state->input_profile);
    gboolean game_active = !g_strcmp0(profile, "game")
        || (!g_strcmp0(profile, "auto") && uri_uses_game_input(uri));
    if (state->game_input_active == game_active)
        return;

    if (state->native_scroll_source_id) {
        g_source_remove(state->native_scroll_source_id);
        state->native_scroll_source_id = 0;
        state->native_scroll_scheduled = FALSE;
    }
    if (state->scroll_stop_source_id) {
        g_source_remove(state->scroll_stop_source_id);
        state->scroll_stop_source_id = 0;
    }
    state->pending_native_scroll_x = 0;
    state->pending_native_scroll_y = 0;
    state->scroll_active = FALSE;
    state->game_input_active = game_active;
    g_print("Input profile: configured=%s active=%s uri=%s\n",
            profile, game_active ? "game" : "browser", uri ? uri : "(null)");
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    g_print("Load changed: %s uri=%s progress=%.2f\n",
            load_event_name(load_event),
            webkit_web_view_get_uri(web_view) ? webkit_web_view_get_uri(web_view) : "(null)",
            webkit_web_view_get_estimated_load_progress(web_view));
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    BrowserTab *tab = chrome_active_tab(chrome);
    const char *uri = webkit_web_view_get_uri(web_view);
    if (load_event == WEBKIT_LOAD_STARTED || load_event == WEBKIT_LOAD_REDIRECTED || load_event == WEBKIT_LOAD_COMMITTED)
        update_input_profile_for_uri(state, uri);
    if (tab && uri && uri[0] && g_strcmp0(tab->url, uri)) {
        if (!chrome->suppress_history && tab->url[0]) {
            history_push(tab->back, &tab->back_count, tab->url);
            history_clear(tab->forward, &tab->forward_count);
        }
        chrome_set_tab_url(tab, uri);
    }
    chrome->loading = load_event != WEBKIT_LOAD_FINISHED;
    chrome->load_progress = chrome->loading ? webkit_web_view_get_estimated_load_progress(web_view) : 1.0;
    if (load_event == WEBKIT_LOAD_COMMITTED && !profile_runtime.guest && uri) {
        char *scheme = uri_scheme_dup(uri);
        gboolean record = scheme && (!g_ascii_strcasecmp(scheme, "http")
            || !g_ascii_strcasecmp(scheme, "https"));
        g_free(scheme);
        if (record) {
            GError *error = NULL;
            chrome->current_visit_id = browser_profile_store_record_visit(profile_runtime.store,
                profile_runtime.profile.id, uri,
                webkit_web_view_get_title(web_view) ? webkit_web_view_get_title(web_view) : "",
                &error);
            if (!chrome->current_visit_id)
                g_warning("History record failed: %s", error ? error->message : "unknown");
            g_clear_error(&error);
        } else
            chrome->current_visit_id = 0;
    }
    if (load_event == WEBKIT_LOAD_FINISHED) {
        chrome->suppress_history = FALSE;
        const char *title = webkit_web_view_get_title(web_view);
        if (tab && title)
            g_strlcpy(tab->title, title, sizeof(tab->title));
    }
    /* started/redirected 只刷新 render state(loading 指示),不做整份
     * tab 状态序列化;合成器侧 33ms 轮询会自行发现变化并重绘,
     * 无需再通过 wpe_view_resized 强制请求帧。 */
    if (load_event == WEBKIT_LOAD_COMMITTED || load_event == WEBKIT_LOAD_FINISHED)
        chrome_save_state(state);
    else
        chrome_update_render_state(state);
}

static void on_estimated_load_progress_changed(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppState *state = (AppState *)user_data;
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->loading)
        return;
    chrome->load_progress = webkit_web_view_get_estimated_load_progress(web_view);
    chrome_update_render_state(state);
}

static void on_title_changed(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    g_print("Title changed: %s\n", webkit_web_view_get_title(web_view) ? webkit_web_view_get_title(web_view) : "(null)");
    AppState *state = (AppState *)user_data;
    if (!state)
        return;
    BrowserTab *tab = chrome_active_tab(&state->chrome);
    const char *title = webkit_web_view_get_title(web_view);
    if (tab && title) {
        g_strlcpy(tab->title, title, sizeof(tab->title));
        if (!profile_runtime.guest && state->chrome.current_visit_id) {
            GError *error = NULL;
            if (!browser_profile_store_update_visit_title(profile_runtime.store,
                    state->chrome.current_visit_id, title, &error))
                g_warning("History title update failed: %s", error ? error->message : "unknown");
            g_clear_error(&error);
        }
        chrome_save_state(state);
    }
}

static gboolean print_view_state(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    WPEView *view = state->view;
    WebKitWebView *web_view = state->web_view;
    const char *title = webkit_web_view_get_title(web_view);
    const char *uri = webkit_web_view_get_uri(web_view);
    int top_width = 0;
    int top_height = 0;
    if (state->toplevel)
        wpe_toplevel_get_size(state->toplevel, &top_width, &top_height);
    g_print("View state: size=%dx%d top=%dx%d visible=%d mapped=%d focus=%d frame=%" G_GUINT64_FORMAT " touch=%" G_GUINT64_FORMAT " scroll=%" G_GUINT64_FORMAT " title=%s uri=%s\n",
            wpe_view_get_width(view),
            wpe_view_get_height(view),
            top_width, top_height,
            wpe_view_get_visible(view),
            wpe_view_get_mapped(view),
            wpe_view_get_has_focus(view),
            frame_count,
            state->touch_event_count,
            state->scroll_event_count,
            title ? title : "(null)",
            uri ? uri : "(null)");
    return G_SOURCE_CONTINUE;
}

/* Signal handler for clean shutdown */
static void on_signal(int sig) {
    (void)sig;
    if (main_loop)
        g_main_loop_quit(main_loop);
}

static gboolean gpu_first_frame_timeout(gpointer user_data)
{
    (void)user_data;
    gpu_first_frame_timeout_source_id = 0;
    if (!gpu_render_profile || frame_count > 0)
        return G_SOURCE_REMOVE;
    g_warning("GPU first-frame timeout; requesting CPU fallback");
    runtime_exit_code = 91;
    if (main_loop)
        g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

static void mark_gpu_first_frame_ready(void)
{
    const char *path = g_getenv("WPE_GPU_READY_FILE");
    if (!gpu_render_profile || !path || !path[0])
        return;

    int fd = g_open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        g_warning("GPU ready marker open failed: path=%s errno=%d", path, errno);
        return;
    }
    static const char ready[] = "ready\n";
    if (write(fd, ready, sizeof(ready) - 1) != (ssize_t)(sizeof(ready) - 1))
        g_warning("GPU ready marker write failed: path=%s errno=%d", path, errno);
    fsync(fd);
    close(fd);
    g_print("GPU first frame ready: marker=%s\n", path);
}

/* DRM display disconnected - exit */
static void on_display_disconnected(WPEDisplay *display, GError *error, gpointer user_data) {
    (void)display; (void)user_data;
    g_warning("DRM display disconnected: %s", error ? error->message : "unknown");
    if (main_loop)
        g_main_loop_quit(main_loop);
}

/* WebView load failed */
static gboolean on_load_failed(WebKitWebView *web_view, WebKitLoadEvent load_event,
                                const gchar *failing_uri, GError *error, gpointer user_data) {
    (void)web_view; (void)load_event; (void)user_data;
    g_warning("Load failed (%s): %s", failing_uri, error->message);
    return FALSE;
}

static gboolean on_decide_policy(WebKitWebView *web_view,
                                 WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type,
                                 gpointer user_data)
{
    (void)web_view;
    (void)user_data;

    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION
        && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;

    WebKitNavigationPolicyDecision *navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(navigation_decision);
    WebKitURIRequest *request = action ? webkit_navigation_action_get_request(action) : NULL;
    const char *uri = request ? webkit_uri_request_get_uri(request) : NULL;
    char *scheme = uri_scheme_dup(uri);
    if (!scheme)
        return FALSE;

    if (browser_scheme_is_allowed(scheme)) {
        if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION && uri && uri[0]) {
            g_print("Open new-window navigation in current view: uri=%s nav_type=%d user_gesture=%d frame=%s\n",
                    uri,
                    action ? webkit_navigation_action_get_navigation_type(action) : -1,
                    action ? webkit_navigation_action_is_user_gesture(action) : FALSE,
                    action && webkit_navigation_action_get_frame_name(action) ? webkit_navigation_action_get_frame_name(action) : "(main)");
            webkit_policy_decision_ignore(decision);
            load_uri_preserving_local_html(web_view, uri);
            g_free(scheme);
            return TRUE;
        }
        g_free(scheme);
        return FALSE;
    }

    if (!g_ascii_strcasecmp(scheme, "bilibili")) {
        char *web_url = bilibili_web_url_from_uri(uri);
        if (web_url) {
            g_print("Converted Bilibili navigation: uri=%s url=%s type=%d nav_type=%d user_gesture=%d\n",
                    uri ? uri : "(null)",
                    web_url,
                    type,
                    action ? webkit_navigation_action_get_navigation_type(action) : -1,
                    action ? webkit_navigation_action_is_user_gesture(action) : FALSE);
            webkit_policy_decision_ignore(decision);
            load_uri_preserving_local_html(web_view, web_url);
            g_free(web_url);
            g_free(scheme);
            return TRUE;
        }
    }

    g_warning("Blocked navigation: scheme=%s uri=%s type=%d nav_type=%d user_gesture=%d frame=%s",
              scheme,
              uri ? uri : "(null)",
              type,
              action ? webkit_navigation_action_get_navigation_type(action) : -1,
              action ? webkit_navigation_action_is_user_gesture(action) : FALSE,
              action && webkit_navigation_action_get_frame_name(action) ? webkit_navigation_action_get_frame_name(action) : "(main)");
    webkit_policy_decision_ignore(decision);
    g_free(scheme);
    return TRUE;
}

static gboolean on_permission_request(WebKitWebView *web_view, WebKitPermissionRequest *request, gpointer user_data)
{
    (void)web_view;
    (void)user_data;
    const char *capture_policy = g_getenv("WPE_WEBRTC_CAPTURE");
    gboolean deny_capture = !capture_policy || g_ascii_strcasecmp(capture_policy, "allow");
    if (deny_capture && (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)
        || WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(request))) {
        g_warning("Denied local media capture permission: type=%s policy=deny",
                  G_OBJECT_TYPE_NAME(request));
        webkit_permission_request_deny(request);
        return TRUE;
    }
    return FALSE;
}

typedef struct {
    WebKitWebView *web_view;
    char *uri;
} TLSReloadRequest;

static char *host_from_uri(const char *uri)
{
    if (!uri || !uri[0])
        return NULL;

    GError *error = NULL;
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        g_warning("TLS host parse failed: uri=%s error=%s", uri, error ? error->message : "unknown");
        g_clear_error(&error);
        return NULL;
    }

    const char *host = g_uri_get_host(parsed);
    char *result = host && host[0] ? g_strdup(host) : NULL;
    g_uri_unref(parsed);
    return result;
}

static gboolean reload_after_tls_exception(gpointer user_data)
{
    TLSReloadRequest *request = (TLSReloadRequest *)user_data;
    g_print("Reloading after TLS host exception: %s\n", request->uri ? request->uri : "(null)");
    load_uri_preserving_local_html(request->web_view, request->uri);
    g_object_unref(request->web_view);
    g_free(request->uri);
    g_free(request);
    return G_SOURCE_REMOVE;
}

static gboolean on_load_failed_with_tls_errors(WebKitWebView *web_view,
                                               const gchar *failing_uri,
                                               GTlsCertificate *certificate,
                                               GTlsCertificateFlags errors,
                                               gpointer user_data)
{
    WebKitNetworkSession *network_session = WEBKIT_NETWORK_SESSION(user_data);
    char *host = host_from_uri(failing_uri);
    if (!host || !certificate || !network_session) {
        g_warning("TLS error cannot be handled: uri=%s host=%s errors=0x%x",
                  failing_uri ? failing_uri : "(null)", host ? host : "(null)", errors);
        g_free(host);
        return FALSE;
    }

    if (!tls_exception_hosts)
        tls_exception_hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (g_hash_table_contains(tls_exception_hosts, host)) {
        g_warning("TLS error repeated after host exception: uri=%s host=%s errors=0x%x",
                  failing_uri ? failing_uri : "(null)", host, errors);
        g_free(host);
        return FALSE;
    }

    g_warning("TLS error: uri=%s host=%s errors=0x%x; allowing this certificate for host",
              failing_uri ? failing_uri : "(null)", host, errors);
    webkit_network_session_allow_tls_certificate_for_host(network_session, certificate, host);
    g_hash_table_add(tls_exception_hosts, g_strdup(host));

    TLSReloadRequest *request = g_new0(TLSReloadRequest, 1);
    request->web_view = WEBKIT_WEB_VIEW(g_object_ref(web_view));
    request->uri = g_strdup(failing_uri);
    g_idle_add(reload_after_tls_exception, request);

    g_free(host);
    return TRUE;
}

static const char *termination_reason_name(WebKitWebProcessTerminationReason reason)
{
    switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED:
        return "crashed";
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:
        return "memory-limit";
    case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:
        return "terminated-by-api";
    }
    return "unknown";
}

typedef struct {
    WebKitWebView *web_view;
    char *uri;
} ReloadRequest;

static gboolean filename_has_html_suffix(const char *filename)
{
    if (!filename || !filename[0])
        return FALSE;

    char *lower = g_ascii_strdown(filename, -1);
    gboolean result = g_str_has_suffix(lower, ".html") || g_str_has_suffix(lower, ".htm");
    g_free(lower);
    return result;
}

static void load_uri_preserving_local_html(WebKitWebView *web_view, const char *uri)
{
    if (!uri || !uri[0])
        return;

    if (!g_str_has_prefix(uri, "file://")) {
        webkit_web_view_load_uri(web_view, uri);
        return;
    }

    GError *file_error = NULL;
    char *filename = g_filename_from_uri(uri, NULL, &file_error);
    char *contents = NULL;
    gsize length = 0;

    if (filename && filename_has_html_suffix(filename)) {
        if (g_file_get_contents(filename, &contents, &length, &file_error)) {
            g_print("Loading local HTML through load_html: %s bytes=%zu\n", filename, (size_t)length);
            webkit_web_view_load_html(web_view, contents, uri);
        } else {
            g_warning("Local HTML load failed: %s", file_error ? file_error->message : "unknown");
            webkit_web_view_load_uri(web_view, uri);
        }
    } else {
        webkit_web_view_load_uri(web_view, uri);
    }

    if (file_error)
        g_error_free(file_error);
    g_free(contents);
    g_free(filename);
}

static gboolean reload_after_web_process_crash(gpointer user_data)
{
    ReloadRequest *request = (ReloadRequest *)user_data;
    const char *uri = request->uri && request->uri[0] ? request->uri : default_home_url();
    g_warning("Reloading after Web process termination: %s", uri);
    load_uri_preserving_local_html(request->web_view, uri);
    g_object_unref(request->web_view);
    g_free(request->uri);
    g_free(request);
    return G_SOURCE_REMOVE;
}

static void on_web_process_terminated(WebKitWebView *web_view,
                                      WebKitWebProcessTerminationReason reason,
                                      gpointer user_data)
{
    /* 熔断：同一 URL 在 120s 窗口内第 3 次被杀（典型是重型站反复撞内存上限）
     * 时不再原样重载——那只会杀→载→杀无限循环。回主页并提示。 */
    static char *last_crash_uri;
    static gint64 crash_window_start_us;
    static int crash_count_in_window;

    (void)user_data;
    const char *uri = webkit_web_view_get_uri(web_view);
    g_warning("Web process terminated: reason=%s(%d) uri=%s title=%s",
              termination_reason_name(reason), reason,
              uri ? uri : "(null)",
              webkit_web_view_get_title(web_view) ? webkit_web_view_get_title(web_view) : "(null)");

    if (gpu_render_profile && frame_count == 0) {
        g_warning("GPU WebProcess terminated before first frame; requesting CPU fallback");
        runtime_exit_code = 91;
        if (main_loop)
            g_main_loop_quit(main_loop);
        return;
    }

    gint64 now_us = g_get_monotonic_time();
    if (uri && last_crash_uri && !strcmp(uri, last_crash_uri) &&
        now_us - crash_window_start_us < 120 * G_USEC_PER_SEC) {
        crash_count_in_window++;
    } else {
        g_free(last_crash_uri);
        last_crash_uri = g_strdup(uri ? uri : "");
        crash_window_start_us = now_us;
        crash_count_in_window = 1;
    }

    ReloadRequest *request = g_new0(ReloadRequest, 1);
    request->web_view = WEBKIT_WEB_VIEW(g_object_ref(web_view));
    if (crash_count_in_window >= 3) {
        g_warning("Web process crash loop on %s (%d kills in window), falling back to home",
                  last_crash_uri, crash_count_in_window);
        request->uri = g_strdup(default_home_url());
        crash_count_in_window = 0;
        crash_window_start_us = 0;
    } else {
        request->uri = g_strdup(uri);
    }
    g_timeout_add(500, reload_after_web_process_crash, request);
}

/* Called each time a frame buffer has been rendered and scanned out */
static void on_buffer_rendered(WPEView *view, WPEBuffer *buffer, gpointer user_data) {
    (void)view; (void)user_data;
    frame_count++;
    if (frame_count == 1) {
        if (gpu_first_frame_timeout_source_id) {
            g_source_remove(gpu_first_frame_timeout_source_id);
            gpu_first_frame_timeout_source_id = 0;
        }
        mark_gpu_first_frame_ready();
    }
    gint64 now_us = g_get_monotonic_time();
    if (!frame_stats_window_start_us) {
        frame_stats_window_start_us = now_us;
        frame_stats_window_start_count = frame_count;
        g_print("Frame rendered: %dx%d frame=%" G_GUINT64_FORMAT " wpe_receive_fps=first\n",
                wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer), frame_count);
        return;
    }

    gint64 elapsed_us = now_us - frame_stats_window_start_us;
    if (elapsed_us >= 2 * G_USEC_PER_SEC) {
        guint64 frame_delta = frame_count - frame_stats_window_start_count;
        double fps = (double)frame_delta * G_USEC_PER_SEC / elapsed_us;
        g_print("Frame window: %dx%d wpe_receive_fps=%.1f frames=%" G_GUINT64_FORMAT " total=%" G_GUINT64_FORMAT "\n",
                wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer), fps, frame_delta, frame_count);
        frame_stats_window_start_us = now_us;
        frame_stats_window_start_count = frame_count;
    }
}

int main(int argc, char **argv) {
    const char *url = "https://webkit.org";
    const char *drm_device = NULL;  /* NULL = default DRM device */
    const char *viewport_arg = NULL;
    const char *rotation_arg = NULL;

    if (argc > 1) url = argv[1];
    if (argc > 2) drm_device = argv[2];
    if (argc > 3) viewport_arg = argv[3];
    if (argc > 4) rotation_arg = argv[4];
    if (viewport_arg && viewport_arg[0]) {
        int viewport_width = 0;
        int viewport_height = 0;
        if (parse_viewport_string(viewport_arg, &viewport_width, &viewport_height)) {
            g_setenv("WPE_VIEWPORT", viewport_arg, TRUE);
            g_setenv("WPE_DRM_VIEWPORT", viewport_arg, TRUE);
        } else
            g_warning("Ignoring invalid viewport argument '%s'", viewport_arg);
    }
    if (rotation_arg && rotation_arg[0])
        g_setenv("WPE_DRM_ROTATION", rotation_arg, TRUE);
    if (!g_getenv("WPE_PANEL_SIZE") || !g_getenv("WPE_PANEL_SIZE")[0])
        g_setenv("WPE_PANEL_SIZE", "960x266", TRUE);
    if (!g_getenv("WPE_VIEWPORT") || !g_getenv("WPE_VIEWPORT")[0]) {
        g_setenv("WPE_VIEWPORT", g_getenv("WPE_PANEL_SIZE"), TRUE);
        g_setenv("WPE_DRM_VIEWPORT", g_getenv("WPE_PANEL_SIZE"), TRUE);
    }
    if (!g_getenv("WPE_DRM_FIT") || !g_getenv("WPE_DRM_FIT")[0])
        g_setenv("WPE_DRM_FIT", "panel-native", TRUE);
    if (!g_getenv("WPE_PANEL_ROTATION") || !g_getenv("WPE_PANEL_ROTATION")[0])
        g_setenv("WPE_PANEL_ROTATION", g_getenv("WPE_DRM_ROTATION") ? g_getenv("WPE_DRM_ROTATION") : "0", TRUE);
    gpu_render_profile = g_strcmp0(g_getenv("WPE_RENDER_PROFILE"), "gpu") == 0;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_print("WPE DRM Minimal Launcher\n");
    g_print("URL: %s\n", url);
    g_print("DRM: %s\n", drm_device ? drm_device : "(default)");
    g_print("Panel: %s\n", g_getenv("WPE_PANEL_SIZE") ? g_getenv("WPE_PANEL_SIZE") : "960x266");
    g_print("Fit: %s\n", g_getenv("WPE_DRM_FIT") ? g_getenv("WPE_DRM_FIT") : "panel-native");
    g_print("Viewport: %s\n", g_getenv("WPE_VIEWPORT") ? g_getenv("WPE_VIEWPORT") : "(screen)");
    g_print("Rotation: %s\n", g_getenv("WPE_DRM_ROTATION") ? g_getenv("WPE_DRM_ROTATION") : "0");
    g_print("Panel rotation: %s\n", g_getenv("WPE_PANEL_ROTATION") ? g_getenv("WPE_PANEL_ROTATION") : "0");
    g_print("Render profile: %s\n", gpu_render_profile ? "gpu" : "cpu");

    GError *profile_error = NULL;
    if (!profile_runtime_initialize(&profile_error)) {
        g_printerr("Browser profile initialization failed: %s\n",
                   profile_error ? profile_error->message : "unknown error");
        g_clear_error(&profile_error);
        profile_runtime_destroy();
        return 2;
    }
    prepare_runtime_dirs();
    configure_tls_database();

    /* 1. Create and connect DRM display */
    GError *error = NULL;
    WPEDisplay *display = wpe_display_drm_new();
    if (!display) {
        g_error("Failed to create DRM display");
        return 1;
    }

    g_signal_connect(display, "disconnected",
                     G_CALLBACK(on_display_disconnected), NULL);

    if (!wpe_display_drm_connect(WPE_DISPLAY_DRM(display), drm_device, &error)) {
        g_error("Failed to connect DRM: %s", error->message);
        return 1;
    }

    /* Print DRM info */
    g_print("DRM connected: %s (%s)\n",
            wpe_display_drm_supports_atomic(WPE_DISPLAY_DRM(display)) ? "atomic" : "legacy",
            wpe_display_drm_supports_modifiers(WPE_DISPLAY_DRM(display)) ?
                "modifiers" : "no modifiers");

    guint n_screens = wpe_display_get_n_screens(display);
    g_print("DRM screens: %u\n", n_screens);
    for (guint i = 0; i < n_screens; ++i) {
        WPEScreen *screen = wpe_display_get_screen(display, i);
        if (!screen)
            continue;
        g_print("Screen[%u]: id=%u pos=%d,%d size=%dx%d mm=%dx%d scale=%.2f refresh=%d mHz\n",
                i,
                wpe_screen_get_id(screen),
                wpe_screen_get_x(screen),
                wpe_screen_get_y(screen),
                wpe_screen_get_width(screen),
                wpe_screen_get_height(screen),
                wpe_screen_get_physical_width(screen),
                wpe_screen_get_physical_height(screen),
                wpe_screen_get_scale(screen),
                wpe_screen_get_refresh_rate(screen));
    }

    /* 2. Create WebView (this internally creates WPEView + WPEToplevel) */
    /* 单任务设备的内存防线：上限按物理内存动态计算（55%，1GB 机型约 560MB），
     * 超限×0.92 主动 shrinkOrDie 自杀重启（由 on_web_process_terminated 崩溃
     * 重载接管恢复）。写死 300MB 在抖音这类重站上会触发杀→重载循环。 */
    guint memory_limit_mb = 675;
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0 && si.totalram > 0) {
            guint64 total_mb = (guint64)si.totalram * si.mem_unit / (1024 * 1024);
            /* 68%：bilibili/抖音这类重站实测稳态 ~620MB，55%（kill≈500MB）会
             * 30s 一轮杀→重载循环。设备有 512MB swap 且浏览器 oom_score_adj
             * 已置 -600，物理内存吃紧时由 swap 与其他进程让位。 */
            memory_limit_mb = (guint)(total_mb * 68 / 100);
            if (memory_limit_mb < 192)
                memory_limit_mb = 192;
        }
    }
    WebKitMemoryPressureSettings *memory_pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(memory_pressure, memory_limit_mb);
    /* setter 断言要求 conservative < strict < kill，必须先抬高 strict 再设 conservative */
    webkit_memory_pressure_settings_set_strict_threshold(memory_pressure, 0.7);
    webkit_memory_pressure_settings_set_conservative_threshold(memory_pressure, 0.5);
    webkit_memory_pressure_settings_set_kill_threshold(memory_pressure, 0.92);
    WebKitWebContext *context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
        "memory-pressure-settings", memory_pressure,
        NULL);
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
    webkit_web_context_add_path_to_sandbox(context, "/userdisk", TRUE);
    webkit_web_context_add_path_to_sandbox(context, "/tmp", FALSE);
    g_print("WebKit context: 2022 GLib API sandbox_paths=/userdisk,/tmp cache_model=document_browser mem_limit=%uMB kill=0.92\n", memory_limit_mb);
    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_javascript_markup(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, gpu_render_profile);
    webkit_settings_set_enable_2d_canvas_acceleration(settings, gpu_render_profile);
    webkit_settings_set_enable_media(settings, TRUE);
    webkit_settings_set_enable_webaudio(settings, TRUE);
    webkit_settings_set_enable_mediasource(settings, TRUE);
    gboolean webrtc_enabled = env_enabled("WPE_WEBRTC", TRUE);
    webkit_settings_set_enable_media_stream(settings, webrtc_enabled);
    webkit_settings_set_enable_webrtc(settings, webrtc_enabled);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    webkit_settings_set_media_playback_allows_inline(settings, TRUE);
    /* 移动 UA：不设时 WPE 默认桌面 UA，抖音/百度会喂桌面版页面，
     * 对 960x266 小屏是数倍的排版/内存/脚本开销（日志曾证实加载
     * www.douyin.com 桌面版）。 */
    webkit_settings_set_user_agent(settings, site_profile_user_agent("mobile"));
    /* 小内存设备：BFCache 在本机型 cache model 下本就是 0 页，显式关闭求确定性。
     * （DNS 预取 setter 自 2.48 起是 no-op，不再调用。） */
    webkit_settings_set_enable_page_cache(settings, FALSE);
    g_object_set(settings, "enable-write-console-messages-to-stdout", TRUE, NULL);
    g_print("Settings: javascript=%d javascript_markup=%d file_access=%d webgl=%d canvas_accel=%d media=%d webaudio=%d mediasource=%d media_stream=%d webrtc=%d capture=%s site_profile=mobile user_agent=%s\n",
            webkit_settings_get_enable_javascript(settings),
            webkit_settings_get_enable_javascript_markup(settings),
            webkit_settings_get_allow_file_access_from_file_urls(settings),
            webkit_settings_get_enable_webgl(settings),
            webkit_settings_get_enable_2d_canvas_acceleration(settings),
            webkit_settings_get_enable_media(settings),
            webkit_settings_get_enable_webaudio(settings),
            webkit_settings_get_enable_mediasource(settings),
            webkit_settings_get_enable_media_stream(settings),
            webkit_settings_get_enable_webrtc(settings),
            g_getenv("WPE_WEBRTC_CAPTURE") ? g_getenv("WPE_WEBRTC_CAPTURE") : "deny",
            webkit_settings_get_user_agent(settings));
    WebKitUserContentManager *user_content_manager = webkit_user_content_manager_new();
    WebKitWebsitePolicies *policies = webkit_website_policies_new_with_policies(
        "autoplay", WEBKIT_AUTOPLAY_ALLOW,
        NULL);
    webkit_network_session_set_memory_pressure_settings(memory_pressure);
    webkit_memory_pressure_settings_free(memory_pressure);
    WebKitNetworkSession *network_session = profile_network_session_new();
    if (!network_session) {
        g_printerr("Failed to create isolated WebKit network session\n");
        g_object_unref(user_content_manager);
        g_object_unref(settings);
        g_object_unref(policies);
        g_object_unref(context);
        g_object_unref(display);
        profile_runtime_destroy();
        return 2;
    }
    WebKitWebView *web_view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", context,
        "network-session", network_session,
        "settings", settings,
        "user-content-manager", user_content_manager,
        "website-policies", policies,
        "display", display,
        NULL);
    g_object_unref(settings);
    g_object_unref(policies);

    /* 基底背景保持默认白：大量页面不设自身背景色、依赖 UA 默认白底，
     * 设黑会让这类页面整页变黑（m.baidu.com 实测回归）。inset 布局下
     * 工具栏显隐不再 resize WebView，原"白条"根因已不存在。 */

    g_signal_connect(web_view, "load-failed",
                     G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(web_view, "load-failed-with-tls-errors",
                     G_CALLBACK(on_load_failed_with_tls_errors), network_session);
    g_signal_connect(web_view, "web-process-terminated",
                     G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(web_view, "decide-policy",
                     G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(web_view, "permission-request",
                     G_CALLBACK(on_permission_request), NULL);

    /* 3. Access the underlying WPEView for frame callbacks */
    AppState *state = NULL;
    WPEView *wpe_view = webkit_web_view_get_wpe_view(web_view);
    if (wpe_view) {
        WPEToplevel *toplevel = wpe_view_get_toplevel(wpe_view);
        int width = wpe_view_get_width(wpe_view);
        int height = wpe_view_get_height(wpe_view);
        int top_width = 0;
        int top_height = 0;
        if (toplevel)
            wpe_toplevel_get_size(toplevel, &top_width, &top_height);
        g_print("WPEView initial: view=%dx%d visible=%d mapped=%d focus=%d toplevel=%p top=%dx%d state=0x%x\n",
                width, height,
                wpe_view_get_visible(wpe_view),
                wpe_view_get_mapped(wpe_view),
                wpe_view_get_has_focus(wpe_view),
                toplevel,
                top_width, top_height,
                toplevel ? wpe_toplevel_get_state(toplevel) : 0);
        state = g_new0(AppState, 1);
        state->view = wpe_view;
        state->toplevel = toplevel;
        state->web_view = web_view;
        state->network_session = network_session;
        state->touch_fd = -1;
        state->viewport_width = width;
        state->viewport_height = height;
        chrome_load_state(state, url);
        g_strlcpy(state->input_profile, normalize_input_profile(g_getenv("WPE_INPUT_PROFILE")), sizeof(state->input_profile));
        const char *initial_input_uri = chrome_active_tab(&state->chrome) && chrome_active_tab(&state->chrome)->url[0]
            ? chrome_active_tab(&state->chrome)->url : url;
        state->game_input_active = !g_strcmp0(state->input_profile, "game")
            || (!g_strcmp0(state->input_profile, "auto") && uri_uses_game_input(initial_input_uri));
        g_print("Input profile: configured=%s active=%s uri=%s\n",
                state->input_profile, state->game_input_active ? "game" : "browser",
                initial_input_uri ? initial_input_uri : "(null)");
        chrome_apply_site_profile(state);
        setup_keyboard_bridge(state);
        setup_keyboard_user_script(user_content_manager, state);
        g_signal_connect(web_view, "load-changed",
                         G_CALLBACK(on_load_changed), state);
        g_signal_connect(web_view, "notify::estimated-load-progress",
                         G_CALLBACK(on_estimated_load_progress_changed), state);
        g_signal_connect(web_view, "notify::title",
                         G_CALLBACK(on_title_changed), state);

        if (top_width > 0 && top_height > 0 && (width != top_width || height != top_height))
            wpe_view_resized(wpe_view, top_width, top_height);
        const char *viewport_env = g_getenv("WPE_VIEWPORT");
        if (!viewport_env || !viewport_env[0])
            viewport_env = g_getenv("WPE_DRM_VIEWPORT");
        int viewport_width = 0;
        int viewport_height = 0;
        if (parse_viewport_string(viewport_env, &viewport_width, &viewport_height))
            apply_viewport(state, viewport_width, viewport_height, "env");
        else if (viewport_env && viewport_env[0])
            g_warning("Ignoring invalid WPE_VIEWPORT '%s'", viewport_env);

        wpe_view_set_visible(wpe_view, TRUE);
        wpe_view_focus_in(wpe_view);
        g_print("WPEView mapped: view=%dx%d visible=%d mapped=%d focus=%d\n",
                wpe_view_get_width(wpe_view),
                wpe_view_get_height(wpe_view),
                wpe_view_get_visible(wpe_view),
                wpe_view_get_mapped(wpe_view),
                wpe_view_get_has_focus(wpe_view));
        g_signal_connect(wpe_view, "buffer-rendered",
                         G_CALLBACK(on_buffer_rendered), NULL);
        setup_raw_touch(state);
        chrome_apply_layout(state, "startup");
        g_timeout_add_seconds(30, print_view_state, state);
        g_print("WPEView: buffers managed by WPEViewDRM (built-in scanout)\n");
    }
    g_object_unref(user_content_manager);

    /* 4. Load URL */
    const char *startup_url = url;
    if (state && chrome_active_tab(&state->chrome) && chrome_active_tab(&state->chrome)->url[0])
        startup_url = chrome_active_tab(&state->chrome)->url;
    g_print("Loading %s...\n", startup_url);
    load_uri_preserving_local_html(web_view, startup_url);

    /* 5. Run GLib main loop — WPEViewDRM handles everything internally */
    main_loop = g_main_loop_new(NULL, FALSE);
    if (gpu_render_profile)
        gpu_first_frame_timeout_source_id = g_timeout_add_seconds(12, gpu_first_frame_timeout, NULL);
    g_main_loop_run(main_loop);

    /* Cleanup */
    if (gpu_first_frame_timeout_source_id) {
        g_source_remove(gpu_first_frame_timeout_source_id);
        gpu_first_frame_timeout_source_id = 0;
    }
    GMainLoop *finished_loop = main_loop;
    main_loop = NULL;
    g_main_loop_unref(finished_loop);
    if (state) {
        if (state->scroll_stop_source_id)
            g_source_remove(state->scroll_stop_source_id);
        keyboard_clear_active(state);
        if (state->touch_source_id)
            g_source_remove(state->touch_source_id);
        if (state->touch_fd >= 0)
            close(state->touch_fd);
        chrome_destroy(state);
        g_free(state->keyboard_dir);
        g_free(state);
    }
    g_object_unref(web_view);
    g_object_unref(network_session);
    g_object_unref(context);
    g_object_unref(display);
    if (tls_exception_hosts) {
        g_hash_table_destroy(tls_exception_hosts);
        tls_exception_hosts = NULL;
    }
    profile_runtime_destroy();

    return runtime_exit_code;
}
