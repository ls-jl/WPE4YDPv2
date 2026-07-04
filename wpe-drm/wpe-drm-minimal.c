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
#include <sys/ioctl.h>
#include <unistd.h>

static GMainLoop *main_loop = NULL;
static guint64 frame_count = 0;
static gint64 frame_stats_start_us = 0;
static GHashTable *tls_exception_hosts = NULL;

static void load_uri_preserving_local_html(WebKitWebView *web_view, const char *uri);

#define MAX_TOUCH_SLOTS 10
#define MAX_BROWSER_TABS 8
#define MAX_BROWSER_HISTORY 16
#define BROWSER_URL_MAX 512
#define BROWSER_TITLE_MAX 160

typedef enum {
    CHROME_PANEL_NONE = 0,
    CHROME_PANEL_ADDRESS,
    CHROME_PANEL_TABS,
    CHROME_PANEL_SETTINGS
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
    gboolean touch_debug;
    int height;
    int touch_height;
    double hide_down_px;
    double show_up_px;
    double show_up_min_velocity_px_s;
    gint64 transition_us;
    char *state_path;
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
} BrowserChrome;

typedef struct {
    WPEView *view;
    WPEToplevel *toplevel;
    WebKitWebView *web_view;
    char *viewport_file;
    char *viewport_spec;
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
    gint64 keyboard_active_us;
    gint64 last_pointer_tap_us;
    BrowserChrome chrome;
    TouchSlot slots[MAX_TOUCH_SLOTS];
} AppState;

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
        return TRUE;
    if (!g_ascii_strcasecmp(value, "resize"))
        return TRUE;
    if (!g_ascii_strcasecmp(value, "overlay"))
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
    case CHROME_PANEL_ADDRESS: return "address";
    case CHROME_PANEL_TABS: return "tabs";
    case CHROME_PANEL_SETTINGS: return "settings";
    case CHROME_PANEL_NONE:
    default:
        return "none";
    }
}

static ChromePanel chrome_panel_from_name(const char *name)
{
    if (!name)
        return CHROME_PANEL_NONE;
    if (!g_ascii_strcasecmp(name, "address"))
        return CHROME_PANEL_ADDRESS;
    if (!g_ascii_strcasecmp(name, "tabs"))
        return CHROME_PANEL_TABS;
    if (!g_ascii_strcasecmp(name, "settings"))
        return CHROME_PANEL_SETTINGS;
    return CHROME_PANEL_NONE;
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

static void chrome_apply_layout(AppState *state, const char *reason);

static void chrome_request_frame(AppState *state)
{
    if (!state || !state->view)
        return;
    int width = state->viewport_width > 0 ? state->viewport_width : wpe_view_get_width(state->view);
    int height = state->viewport_height > 0 ? state->viewport_height : wpe_view_get_height(state->view);
    wpe_view_resized(state->view, width, height);
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

    if (chrome->panel == CHROME_PANEL_ADDRESS) {
        const char *entries[] = {
            "HOME  https://m.baidu.com/",
            "BAIDU https://m.baidu.com/",
            "BING  https://www.bing.com/"
        };
        for (unsigned i = 0; i < G_N_ELEMENTS(entries); ++i) {
            snprintf(line, sizeof(line), "line%d", line_count);
            g_key_file_set_string(key_file, "panel", line, entries[i]);
            line_count++;
        }
        if (tab && tab->url[0]) {
            snprintf(line, sizeof(line), "line%d", line_count);
            g_key_file_set_string(key_file, "panel", line, "RELOAD CURRENT");
            line_count++;
        }
    } else if (chrome->panel == CHROME_PANEL_TABS) {
        for (int i = 0; i < chrome->tab_count && line_count < 8; ++i) {
            BrowserTab *item = &chrome->tabs[i];
            char value[96];
            snprintf(value, sizeof(value), "%c TAB %d %s", i == chrome->active ? '*' : ' ', i + 1, item->title[0] ? item->title : item->url);
            snprintf(line, sizeof(line), "line%d", line_count);
            g_key_file_set_string(key_file, "panel", line, value);
            line_count++;
        }
        snprintf(line, sizeof(line), "line%d", line_count);
        g_key_file_set_string(key_file, "panel", line, "NEW TAB");
        line_count++;
        snprintf(line, sizeof(line), "line%d", line_count);
        g_key_file_set_string(key_file, "panel", line, "CLOSE TAB");
        line_count++;
    } else if (chrome->panel == CHROME_PANEL_SETTINGS) {
        const char *home = chrome->home_url && chrome->home_url[0] ? chrome->home_url : default_home_url();
        snprintf(line, sizeof(line), "SET HOME %.80s", home);
        g_key_file_set_string(key_file, "panel", "line0", line);
        g_key_file_set_string(key_file, "panel", "line1", chrome->touch_debug ? "TOUCH DEBUG ON" : "TOUCH DEBUG OFF");
        g_key_file_set_string(key_file, "panel", "line2", "CLEAR CACHE");
        g_key_file_set_string(key_file, "panel", "line3", "ABOUT WPE DRM2");
        line_count = 4;
    }

    g_key_file_set_integer(key_file, "panel", "line_count", line_count);
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
    g_key_file_set_boolean(key_file, "chrome", "can_back", tab && tab->back_count > 0);
    g_key_file_set_boolean(key_file, "chrome", "can_forward", tab && tab->forward_count > 0);
    g_key_file_set_boolean(key_file, "chrome", "touch_debug", chrome->touch_debug);
    g_key_file_set_integer(key_file, "chrome", "height", chrome->height);
    g_key_file_set_integer(key_file, "chrome", "touch_height", chrome->touch_height);
    g_key_file_set_integer(key_file, "chrome", "tab_count", chrome->tab_count);
    g_key_file_set_integer(key_file, "chrome", "active_tab", chrome->active);
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
        char *dir = g_path_get_dirname(chrome->render_state_path);
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        if (!g_file_set_contents(chrome->render_state_path, data, length, NULL))
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

    GKeyFile *key_file = g_key_file_new();
    g_key_file_set_string(key_file, "settings", "home_url", chrome->home_url ? chrome->home_url : default_home_url());
    g_key_file_set_boolean(key_file, "settings", "touch_debug", chrome->touch_debug);
    g_key_file_set_integer(key_file, "tabs", "active", chrome->active);
    g_key_file_set_integer(key_file, "tabs", "count", chrome->tab_count);
    g_key_file_set_integer(key_file, "tabs", "next_id", chrome->next_tab_id);

    for (int i = 0; i < chrome->tab_count; ++i) {
        BrowserTab *tab = &chrome->tabs[i];
        char group[32];
        snprintf(group, sizeof(group), "tab%d", i);
        g_key_file_set_integer(key_file, group, "id", tab->id);
        g_key_file_set_string(key_file, group, "url", tab->url);
        g_key_file_set_string(key_file, group, "title", tab->title);
        if (tab->back_count)
            g_key_file_set_string_list(key_file, group, "back", (const gchar * const *)tab->back, tab->back_count);
        if (tab->forward_count)
            g_key_file_set_string_list(key_file, group, "forward", (const gchar * const *)tab->forward, tab->forward_count);
    }

    gsize length = 0;
    gchar *data = g_key_file_to_data(key_file, &length, NULL);
    if (data) {
        char *dir = g_path_get_dirname(chrome->state_path);
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        if (!g_file_set_contents(chrome->state_path, data, length, NULL))
            g_warning("Failed to write chrome state: %s", chrome->state_path);
    }
    g_free(data);
    g_key_file_unref(key_file);
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

static void chrome_close_active_tab(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    if (chrome->tab_count <= 0)
        return;
    chrome_clear_tab(&chrome->tabs[chrome->active]);
    for (int i = chrome->active; i < chrome->tab_count - 1; ++i)
        chrome->tabs[i] = chrome->tabs[i + 1];
    memset(&chrome->tabs[chrome->tab_count - 1], 0, sizeof(BrowserTab));
    chrome->tab_count--;
    if (chrome->tab_count <= 0) {
        chrome->tab_count = 1;
        chrome->active = 0;
        chrome->tabs[0].id = chrome->next_tab_id++;
        chrome_set_tab_url(&chrome->tabs[0], chrome->home_url ? chrome->home_url : default_home_url());
    } else if (chrome->active >= chrome->tab_count)
        chrome->active = chrome->tab_count - 1;
    chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
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
    return g_string_free(out, FALSE);
}

static char *js_quote_string(const char *value)
{
    char *escaped = json_escape_string(value);
    char *quoted = g_strdup_printf("\"%s\"", escaped ? escaped : "");
    g_free(escaped);
    return quoted;
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
    if (strstr(value, "://") || g_str_has_prefix(value, "about:") || g_str_has_prefix(value, "file:"))
        return value;

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

static void keyboard_clear_active(AppState *state)
{
    if (!state)
        return;
    if (state->keyboard_response_source_id) {
        g_source_remove(state->keyboard_response_source_id);
        state->keyboard_response_source_id = 0;
    }
    if (state->keyboard_active_id) {
        char *request_name = g_strdup_printf("%s.json", state->keyboard_active_id);
        char *request_path = keyboard_path(state, "requests", request_name);
        if (request_path)
            g_unlink(request_path);
        g_free(request_path);
        g_free(request_name);
    }
    g_clear_pointer(&state->keyboard_active_id, g_free);
    g_clear_pointer(&state->keyboard_active_kind, g_free);
    state->keyboard_active_us = 0;
}

static void keyboard_insert_web_text(AppState *state, const char *text)
{
    if (!state || !state->web_view)
        return;
    char *quoted = js_quote_string(text ? text : "");
    char *script = g_strdup_printf(
        "(function(text){"
        "if(window.__haasKeyboardSetText)return window.__haasKeyboardSetText(text);"
        "var el=document.activeElement;"
        "function editable(n){return n&&(n.tagName==='INPUT'||n.tagName==='TEXTAREA'||n.isContentEditable);}"
        "if(!editable(el))return false;"
        "if(el.isContentEditable){el.textContent=text;}else{el.value=text;if(el.setSelectionRange){try{el.setSelectionRange(text.length,text.length);}catch(e){}}}"
        "function fire(n){var ev=document.createEvent('HTMLEvents');ev.initEvent(n,true,false);el.dispatchEvent(ev);}"
        "fire('input');fire('change');return true;"
        "})(%s)",
        quoted);
    g_print("Keyboard insert web text: bytes=%zu\n", strlen(text ? text : ""));
    webkit_web_view_evaluate_javascript(state->web_view, script, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(script);
    g_free(quoted);
}

static void keyboard_handle_response(AppState *state, gboolean confirmed, const char *text)
{
    if (!state || !state->keyboard_active_id || !state->keyboard_active_kind)
        return;

    g_print("Keyboard %s: id=%s kind=%s bytes=%zu\n",
            confirmed ? "confirmed" : "cancelled",
            state->keyboard_active_id,
            state->keyboard_active_kind,
            strlen(text ? text : ""));

    if (confirmed) {
        if (!g_strcmp0(state->keyboard_active_kind, "address")) {
            char *url = normalize_user_url(text);
            chrome_load_url(state, url, TRUE);
            g_free(url);
        } else if (!g_strcmp0(state->keyboard_active_kind, "home_url")) {
            char *url = normalize_user_url(text);
            g_free(state->chrome.home_url);
            state->chrome.home_url = url;
            state->chrome.panel = CHROME_PANEL_NONE;
            chrome_save_state(state);
            chrome_request_frame(state);
        } else if (!g_strcmp0(state->keyboard_active_kind, "web_input"))
            keyboard_insert_web_text(state, text);
    }

    keyboard_clear_active(state);
}

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
        keyboard_clear_active(state);
        return G_SOURCE_REMOVE;
    }

    char *ok_name = g_strdup_printf("%s.ok", state->keyboard_active_id);
    char *cancel_name = g_strdup_printf("%s.cancel", state->keyboard_active_id);
    char *ok_path = keyboard_path(state, "responses", ok_name);
    char *cancel_path = keyboard_path(state, "responses", cancel_name);
    g_free(ok_name);
    g_free(cancel_name);

    char *contents = NULL;
    gsize length = 0;
    if (ok_path && g_file_get_contents(ok_path, &contents, &length, NULL)) {
        g_unlink(ok_path);
        g_free(ok_path);
        if (cancel_path) {
            g_unlink(cancel_path);
            g_free(cancel_path);
        }
        state->keyboard_response_source_id = 0;
        keyboard_handle_response(state, TRUE, contents ? contents : "");
        g_free(contents);
        return G_SOURCE_REMOVE;
    }
    g_free(ok_path);

    if (cancel_path && g_file_test(cancel_path, G_FILE_TEST_EXISTS)) {
        g_unlink(cancel_path);
        g_free(cancel_path);
        state->keyboard_response_source_id = 0;
        keyboard_handle_response(state, FALSE, "");
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
    g_mkdir_with_parents(requests_dir, 0700);
    g_mkdir_with_parents(responses_dir, 0700);
    g_free(requests_dir);
    g_free(responses_dir);

    state->keyboard_next_id++;
    char *id = g_strdup_printf("%" G_GUINT64_FORMAT "_%" G_GINT64_FORMAT,
                               state->keyboard_next_id, g_get_monotonic_time());
    char *name = g_strdup_printf("%s.json", id);
    char *path = keyboard_path(state, "requests", name);
    g_free(name);

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

    gboolean ok = path && g_file_set_contents(path, payload, -1, NULL);
    if (ok) {
        state->keyboard_active_id = g_strdup(id);
        state->keyboard_active_kind = g_strdup(kind ? kind : "");
        state->keyboard_active_us = g_get_monotonic_time();
        state->keyboard_response_source_id = g_timeout_add(150, keyboard_response_tick, state);
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

static char *query_get_param(const char *message, const char *key)
{
    if (!message || !key)
        return NULL;
    char **pairs = g_strsplit(message, "&", -1);
    char *result = NULL;
    for (int i = 0; pairs && pairs[i]; ++i) {
        char *equals = strchr(pairs[i], '=');
        if (!equals)
            continue;
        *equals = 0;
        if (!strcmp(pairs[i], key)) {
            result = g_uri_unescape_string(equals + 1, NULL);
            break;
        }
    }
    g_strfreev(pairs);
    return result;
}

static int query_get_int(const char *message, const char *key, int fallback)
{
    char *value = query_get_param(message, key);
    if (!value)
        return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    int result = end != value ? (int)parsed : fallback;
    g_free(value);
    return result;
}

static gboolean query_get_bool(const char *message, const char *key, gboolean fallback)
{
    char *value = query_get_param(message, key);
    if (!value)
        return fallback;
    gboolean result = !g_ascii_strcasecmp(value, "1") || !g_ascii_strcasecmp(value, "true") ||
        !g_ascii_strcasecmp(value, "yes");
    g_free(value);
    return result;
}

static void on_keyboard_script_message(WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
    (void)manager;
    AppState *state = (AppState *)user_data;
    if (!state || !value)
        return;

    char *message = jsc_value_to_string(value);
    char *kind = query_get_param(message, "kind");
    if (g_strcmp0(kind, "web_input")) {
        g_free(kind);
        g_free(message);
        return;
    }

    char *text = query_get_param(message, "text");
    char *placeholder = query_get_param(message, "placeholder");
    char *input_type = query_get_param(message, "inputType");
    int maxlength = query_get_int(message, "maxlength", 100);
    gboolean multiline = query_get_bool(message, "multiLinesEditVisible", FALSE);
    gint64 pointer_gate_us = (gint64)env_double("WPE_KEYBOARD_POINTER_GATE_MS", 2000) * 1000;
    gint64 since_pointer_us = state->last_pointer_tap_us > 0 ? g_get_monotonic_time() - state->last_pointer_tap_us : G_MAXINT64;
    if (pointer_gate_us <= 0 || since_pointer_us <= pointer_gate_us) {
        keyboard_request(state, "web_input", text ? text : "", placeholder && placeholder[0] ? placeholder : "请输入内容",
                         input_type && input_type[0] ? input_type : "ZhCNPreferred",
                         maxlength, multiline);
    } else
        g_print("Keyboard web_input ignored: no recent pointer tap since_us=%" G_GINT64_FORMAT "\n", since_pointer_us);
    g_free(kind);
    g_free(text);
    g_free(placeholder);
    g_free(input_type);
    g_free(message);
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
    g_mkdir_with_parents(requests_dir, 0700);
    g_mkdir_with_parents(responses_dir, 0700);
    g_print("Keyboard bridge: dir=%s requests=%s responses=%s\n",
            state->keyboard_dir, requests_dir, responses_dir);
    g_free(requests_dir);
    g_free(responses_dir);
}

static void setup_keyboard_user_script(WebKitUserContentManager *manager, AppState *state)
{
    if (!manager || !state)
        return;
    g_signal_connect(manager, "script-message-received::haasKeyboard",
                     G_CALLBACK(on_keyboard_script_message), state);
    if (!webkit_user_content_manager_register_script_message_handler(manager, "haasKeyboard", NULL))
        g_warning("Keyboard script message handler already registered");

    const char *source =
        "(function(){"
        "if(window.__haasKeyboardInstalled)return;"
        "window.__haasKeyboardInstalled=true;"
        "window.__haasKeyboardTarget=null;"
        "window.__haasKeyboardLastAt=0;"
        "function editable(el){return !!(el&&((el.tagName==='INPUT'&&!/^(button|submit|reset|checkbox|radio|file|image|range|color)$/i.test(el.type||''))||el.tagName==='TEXTAREA'||el.isContentEditable)&&!el.disabled&&!el.readOnly);}"
        "function enc(v){return encodeURIComponent(v==null?'':String(v));}"
        "function valueOf(el){return el.isContentEditable?(el.innerText||el.textContent||''):(el.value||'');}"
        "function typeOf(el){var t=String(el.getAttribute('type')||'').toLowerCase();if(t==='number'||t==='tel')return 'Number';if(t==='email'||t==='url'||t==='password')return 'EnUSPreferred';return 'ZhCNPreferred';}"
        "function request(el){if(!editable(el)||!window.webkit||!window.webkit.messageHandlers||!window.webkit.messageHandlers.haasKeyboard)return;var now=Date.now();if(window.__haasKeyboardTarget===el&&now-window.__haasKeyboardLastAt<600)return;window.__haasKeyboardTarget=el;window.__haasKeyboardLastAt=now;var ml=(el.tagName==='TEXTAREA'||el.isContentEditable);var max=parseInt(el.getAttribute('maxlength')||'',10);if(!isFinite(max)||max<=0)max=ml?512:100;var ph=el.getAttribute('placeholder')||'请输入内容';var msg='kind=web_input&text='+enc(valueOf(el))+'&placeholder='+enc(ph)+'&inputType='+enc(typeOf(el))+'&maxlength='+max+'&multiLinesEditVisible='+(ml?'1':'0');window.webkit.messageHandlers.haasKeyboard.postMessage(msg);}"
        "document.addEventListener('click',function(e){if(e.isTrusted===false)return;var el=e.target;if(editable(el))setTimeout(function(){request(el);},0);},true);"
        "window.__haasKeyboardSetText=function(text){var el=window.__haasKeyboardTarget||document.activeElement;if(!editable(el))return false;try{el.focus();}catch(e){}if(el.isContentEditable){el.textContent=text;}else{el.value=text;if(el.setSelectionRange){try{el.setSelectionRange(String(text).length,String(text).length);}catch(e){}}}function fire(n){var ev=document.createEvent('HTMLEvents');ev.initEvent(n,true,false);el.dispatchEvent(ev);}fire('input');fire('change');return true;};"
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

static void chrome_select_panel_row(AppState *state, int row)
{
    BrowserChrome *chrome = &state->chrome;
    if (chrome->panel == CHROME_PANEL_ADDRESS) {
        if (row == 0)
            chrome_load_url(state, chrome->home_url ? chrome->home_url : default_home_url(), TRUE);
        else if (row == 1)
            chrome_load_url(state, "https://m.baidu.com/", TRUE);
        else if (row == 2)
            chrome_load_url(state, "https://www.bing.com/", TRUE);
        else if (row == 3)
            chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
    } else if (chrome->panel == CHROME_PANEL_TABS) {
        if (row >= 0 && row < chrome->tab_count)
            chrome_switch_tab(state, row);
        else if (row == chrome->tab_count)
            chrome_new_tab(state, chrome->home_url);
        else if (row == chrome->tab_count + 1)
            chrome_close_active_tab(state);
    } else if (chrome->panel == CHROME_PANEL_SETTINGS) {
        if (row == 0) {
            chrome->panel = CHROME_PANEL_NONE;
            chrome_save_state(state);
            keyboard_request(state, "home_url", chrome->home_url ? chrome->home_url : default_home_url(),
                             "设置主页 URL", "EnUSPreferred", 512, FALSE);
        } else if (row == 1) {
            chrome->touch_debug = !chrome->touch_debug;
            chrome_save_state(state);
        } else if (row == 2) {
            chrome_clear_cache();
            chrome->panel = CHROME_PANEL_NONE;
            chrome_save_state(state);
        } else if (row == 3) {
            g_print("About: Direct WPE DRM browser chrome\n");
        }
    }
    chrome_update_render_state(state);
    chrome_request_frame(state);
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
    if (chrome->panel != CHROME_PANEL_NONE && y >= panel_y + 30) {
        int row = (int)((y - panel_y - 30) / 28);
        if (row >= 0) {
            g_print("Chrome tap: panel=%s row=%d x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), row, x, y);
            chrome_select_panel_row(state, row);
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
    if (x < panel_width / 2) {
        BrowserTab *tab = chrome_active_tab(chrome);
        const char *current = tab && tab->url[0] ? tab->url : (chrome->home_url ? chrome->home_url : default_home_url());
        chrome->panel = CHROME_PANEL_NONE;
        chrome_set_visible(state, TRUE);
        chrome_save_state(state);
        chrome_request_frame(state);
        keyboard_request(state, "address", current, "输入网址或搜索", "EnUSPreferred", 512, FALSE);
        g_print("Chrome tap: address keyboard x=%.1f y=%.1f\n", x, y);
        return;
    }

    int button_x = panel_width / 2 + 8;
    int button_w = MAX(36, (panel_width - button_x - 8) / 6);
    int index = (int)((x - button_x) / button_w);
    if (index < 0 || index > 5) {
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
        chrome->panel = chrome->panel == CHROME_PANEL_TABS ? CHROME_PANEL_NONE : CHROME_PANEL_TABS;
        g_print("Chrome tap: tabs panel=%s x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), x, y);
        chrome_save_state(state);
    } else if (index == 3) {
        g_print("Chrome tap: new tab x=%.1f y=%.1f\n", x, y);
        chrome_new_tab(state, chrome->home_url);
    } else if (index == 4) {
        g_print("Chrome tap: %s x=%.1f y=%.1f\n", chrome->loading ? "stop" : "reload", x, y);
        if (chrome->loading)
            webkit_web_view_stop_loading(state->web_view);
        else if (chrome_active_tab(chrome))
            chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
    } else if (index == 5) {
        chrome->panel = chrome->panel == CHROME_PANEL_SETTINGS ? CHROME_PANEL_NONE : CHROME_PANEL_SETTINGS;
        g_print("Chrome tap: settings panel=%s x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), x, y);
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
    if (chrome->panel != CHROME_PANEL_NONE && y >= panel_y && y <= 240)
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
    chrome->touch_height = (int)env_double("WPE_CHROME_TOUCH_HEIGHT", 96);
    if (chrome->touch_height < chrome->height)
        chrome->touch_height = chrome->height;
    if (chrome->touch_height > 140)
        chrome->touch_height = 140;
    chrome->hide_down_px = env_double("WPE_CHROME_HIDE_DOWN_PX", 96);
    chrome->show_up_px = env_double("WPE_CHROME_SHOW_UP_PX", 180);
    chrome->show_up_min_velocity_px_s = env_double("WPE_CHROME_SHOW_UP_MIN_VELOCITY", 700);
    chrome->state_path = g_strdup(g_getenv("WPE_CHROME_STATE") && g_getenv("WPE_CHROME_STATE")[0] ? g_getenv("WPE_CHROME_STATE") : "/tmp/wpe-drm2-browser-state.ini");
    chrome->render_state_path = g_strdup(g_getenv("WPE_CHROME_RENDER_STATE") && g_getenv("WPE_CHROME_RENDER_STATE")[0] ? g_getenv("WPE_CHROME_RENDER_STATE") : "/tmp/wpe-drm2-chrome-state.ini");
    chrome->home_url = g_strdup(default_home_url());
    chrome->tab_count = 1;
    chrome->active = 0;
    chrome->next_tab_id = 2;
    chrome->tabs[0].id = 1;
    chrome_set_tab_url(&chrome->tabs[0], initial_url && initial_url[0] ? initial_url : chrome->home_url);
    g_print("Chrome config: enabled=%d height=%d touch_height=%d hide_down=%.1f show_up=%.1f show_min_velocity=%.1f\n",
            chrome->enabled, chrome->height, chrome->touch_height,
            chrome->hide_down_px, chrome->show_up_px, chrome->show_up_min_velocity_px_s);

    GKeyFile *key_file = g_key_file_new();
    if (g_key_file_load_from_file(key_file, chrome->state_path, G_KEY_FILE_NONE, NULL)) {
        gchar *home = g_key_file_get_string(key_file, "settings", "home_url", NULL);
        if (home && home[0]) {
            g_free(chrome->home_url);
            chrome->home_url = home;
            home = NULL;
        }
        g_free(home);
        chrome->touch_debug = g_key_file_get_boolean(key_file, "settings", "touch_debug", NULL);
        int count = g_key_file_get_integer(key_file, "tabs", "count", NULL);
        if (count > 0 && count <= MAX_BROWSER_TABS) {
            for (int i = 0; i < chrome->tab_count; ++i)
                chrome_clear_tab(&chrome->tabs[i]);
            chrome->tab_count = count;
            chrome->active = CLAMP(g_key_file_get_integer(key_file, "tabs", "active", NULL), 0, count - 1);
            chrome->next_tab_id = MAX(1, g_key_file_get_integer(key_file, "tabs", "next_id", NULL));
            for (int i = 0; i < count; ++i) {
                BrowserTab *tab = &chrome->tabs[i];
                char group[32];
                snprintf(group, sizeof(group), "tab%d", i);
                tab->id = g_key_file_get_integer(key_file, group, "id", NULL);
                if (!tab->id)
                    tab->id = i + 1;
                gchar *url = g_key_file_get_string(key_file, group, "url", NULL);
                chrome_set_tab_url(tab, url && url[0] ? url : chrome->home_url);
                g_free(url);
                gchar *title = g_key_file_get_string(key_file, group, "title", NULL);
                if (title)
                    g_strlcpy(tab->title, title, sizeof(tab->title));
                g_free(title);
                gsize list_len = 0;
                gchar **list = g_key_file_get_string_list(key_file, group, "back", &list_len, NULL);
                for (gsize j = 0; list && j < list_len && tab->back_count < MAX_BROWSER_HISTORY; ++j)
                    tab->back[tab->back_count++] = g_strdup(list[j]);
                g_strfreev(list);
                list_len = 0;
                list = g_key_file_get_string_list(key_file, group, "forward", &list_len, NULL);
                for (gsize j = 0; list && j < list_len && tab->forward_count < MAX_BROWSER_HISTORY; ++j)
                    tab->forward[tab->forward_count++] = g_strdup(list[j]);
                g_strfreev(list);
            }
        }
    }
    g_key_file_unref(key_file);
    chrome_update_render_state(state);
}

static void chrome_destroy(AppState *state)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    chrome_save_state(state);
    for (int i = 0; i < chrome->tab_count; ++i)
        chrome_clear_tab(&chrome->tabs[i]);
    g_free(chrome->state_path);
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

    char detail[96];
    snprintf(detail, sizeof(detail), "chrome_%s inset=%d panel=%dx%d", reason ? reason : "layout", top_inset, panel_width, panel_height);
    apply_viewport(state, panel_width, content_height, detail);
}

static gboolean viewport_file_tick(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (!state || !state->viewport_file || !state->viewport_file[0])
        return G_SOURCE_CONTINUE;

    char *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(state->viewport_file, &contents, &length, &error)) {
        if (error)
            g_error_free(error);
        return G_SOURCE_CONTINUE;
    }

    char *spec = g_strstrip(contents);
    if (!spec[0]) {
        g_free(contents);
        return G_SOURCE_CONTINUE;
    }

    if (state->viewport_spec && !strcmp(state->viewport_spec, spec)) {
        g_free(contents);
        return G_SOURCE_CONTINUE;
    }

    int width = 0;
    int height = 0;
    if (parse_viewport_string(spec, &width, &height)) {
        apply_viewport(state, width, height, state->viewport_file);
        g_free(state->viewport_spec);
        state->viewport_spec = g_strdup(spec);
    } else
        g_warning("Ignoring invalid viewport file content '%s' from %s", spec, state->viewport_file);

    g_free(contents);
    return G_SOURCE_CONTINUE;
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
    if (!state->send_touch_events)
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
    if (state->touch_scroll_invert_y)
        delta_y = -delta_y;
    if (state->panel_rotation == 90 || state->panel_rotation == 270) {
        delta_x = -delta_x;
        delta_y = -delta_y;
    }

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
            g_print("Touch down: raw=%d,%d mapped=%.1f,%.1f chrome=%d visible=%d panel=%s touch_height=%d\n",
                    slot->raw_x, slot->raw_y, slot->x, slot->y, slot->chrome_consumed,
                    state->chrome.visible, chrome_panel_name(state->chrome.panel), state->chrome.touch_height);
            if (!slot->chrome_consumed && !state->touch_scroll_fallback)
                send_touch_event(state, WPE_EVENT_TOUCH_DOWN, i, slot->x, slot->y, time_ms);
        } else if (slot->just_up) {
            if (!slot->chrome_consumed && !state->touch_scroll_fallback)
                send_touch_event(state, WPE_EVENT_TOUCH_UP, i, slot->last_x, slot->last_y, time_ms);
            double tap_limit = state->touch_tap_max_move > 0 ? state->touch_tap_max_move : 24;
            if (slot->chrome_consumed) {
                double chrome_tap_limit = env_double("WPE_CHROME_TAP_MAX_MOVE", 32);
                if (chrome_tap_limit > tap_limit)
                    tap_limit = chrome_tap_limit;
            }
            if (!slot->scrolling && slot->max_move_sq <= tap_limit * tap_limit) {
                if (slot->chrome_consumed)
                    chrome_handle_toolbar_tap(state, slot->last_x, slot->last_y);
                else
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
            slot->tracking_id = -1;
        } else if (slot->active && (slot->x != slot->last_x || slot->y != slot->last_y)) {
            if (!slot->chrome_consumed && !state->touch_scroll_fallback)
                send_touch_event(state, WPE_EVENT_TOUCH_MOVE, i, slot->x, slot->y, time_ms);
            double from_down_x = slot->x - slot->down_x;
            double from_down_y = slot->y - slot->down_y;
            double move_sq = from_down_x * from_down_x + from_down_y * from_down_y;
            if (move_sq > slot->max_move_sq)
                slot->max_move_sq = move_sq;
            double tap_limit = state->touch_tap_max_move > 0 ? state->touch_tap_max_move : 24;
            if (!slot->chrome_consumed) {
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
    if (!device || !device[0])
        device = "/dev/input/by-path/hyn_ts";

    int fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && strcmp(device, "/dev/input/event5"))
        fd = open("/dev/input/event5", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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
    state->send_touch_events = env_enabled("WPE_SEND_TOUCH_EVENTS", TRUE);
    state->synthesize_pointer_tap = env_enabled("WPE_SYNTHESIZE_POINTER_TAP", TRUE);
    state->touch_scroll_fallback = env_enabled("WPE_TOUCH_SCROLL_FALLBACK", FALSE);
    state->touch_native_scroll_fallback = env_enabled("WPE_TOUCH_NATIVE_SCROLL", TRUE);
    state->touch_js_scroll_fallback = env_enabled("WPE_TOUCH_JS_SCROLL", FALSE);
    state->touch_horizontal_scroll = env_enabled("WPE_TOUCH_HORIZONTAL_SCROLL", FALSE);
    state->touch_scroll_invert_y = env_enabled("WPE_TOUCH_SCROLL_INVERT_Y", TRUE);
    state->touch_scroll_scale = env_double("WPE_TOUCH_SCROLL_SCALE", 1.0);
    state->touch_scroll_max_step = env_double("WPE_TOUCH_SCROLL_MAX_STEP", 32);
    state->touch_scroll_pending_limit = env_double("WPE_TOUCH_SCROLL_PENDING_LIMIT", 48);
    state->touch_tap_max_move = env_double("WPE_TOUCH_TAP_MAX_MOVE", 24);
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
    if (tab && uri && uri[0] && g_strcmp0(tab->url, uri)) {
        if (!chrome->suppress_history && tab->url[0]) {
            history_push(tab->back, &tab->back_count, tab->url);
            history_clear(tab->forward, &tab->forward_count);
        }
        chrome_set_tab_url(tab, uri);
    }
    chrome->loading = load_event != WEBKIT_LOAD_FINISHED;
    if (load_event == WEBKIT_LOAD_FINISHED) {
        chrome->suppress_history = FALSE;
        const char *title = webkit_web_view_get_title(web_view);
        if (tab && title)
            g_strlcpy(tab->title, title, sizeof(tab->title));
    }
    chrome_save_state(state);
    chrome_request_frame(state);
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
        chrome_save_state(state);
        chrome_request_frame(state);
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

static void on_eval_finished(GObject *object, GAsyncResult *result, gpointer user_data) {
    (void)user_data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!value) {
        g_warning("Eval finished: failed: %s", error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return;
    }
    char *str = jsc_value_to_string(value);
    g_print("Eval finished: %s\n", str ? str : "(null)");
    g_free(str);
    g_object_unref(value);
}

static gboolean eval_tick(gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(user_data);
    static guint64 eval_count = 0;
    eval_count++;
    char *script = g_strdup_printf("(function(){var e=document.getElementById('txt'); var before='before title='+document.title+' text='+(e?e.textContent:'none')+' f='+typeof f+' body='+!!document.body; document.title='eval %llu'; if(e)e.textContent='eval %llu'; return before+' after=eval %llu';})()", (unsigned long long)eval_count, (unsigned long long)eval_count, (unsigned long long)eval_count);
    g_print("Eval request: %" G_GUINT64_FORMAT "\n", eval_count);
    webkit_web_view_evaluate_javascript(web_view, script, -1, NULL, NULL, NULL, on_eval_finished, NULL);
    g_free(script);
    return G_SOURCE_CONTINUE;
}

/* Signal handler for clean shutdown */
static void on_signal(int sig) {
    (void)sig;
    if (main_loop)
        g_main_loop_quit(main_loop);
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
    const char *default_uri = g_getenv("WPE_DEFAULT_URL");
    const char *uri = request->uri && request->uri[0] ? request->uri : (default_uri && default_uri[0] ? default_uri : "https://www.douyin.com/");
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
    (void)user_data;
    g_warning("Web process terminated: reason=%s(%d) uri=%s title=%s",
              termination_reason_name(reason), reason,
              webkit_web_view_get_uri(web_view) ? webkit_web_view_get_uri(web_view) : "(null)",
              webkit_web_view_get_title(web_view) ? webkit_web_view_get_title(web_view) : "(null)");
    ReloadRequest *request = g_new0(ReloadRequest, 1);
    request->web_view = WEBKIT_WEB_VIEW(g_object_ref(web_view));
    request->uri = g_strdup(webkit_web_view_get_uri(web_view));
    g_timeout_add(500, reload_after_web_process_crash, request);
}

/* Called each time a frame buffer has been rendered and scanned out */
static void on_buffer_rendered(WPEView *view, WPEBuffer *buffer, gpointer user_data) {
    (void)view; (void)user_data;
    frame_count++;
    if (!frame_stats_start_us)
        frame_stats_start_us = g_get_monotonic_time();

    if (frame_count == 1 || !(frame_count % 60)) {
        gint64 elapsed_us = g_get_monotonic_time() - frame_stats_start_us;
        if (elapsed_us <= 0)
            elapsed_us = 1;
        double fps = (double)frame_count * G_USEC_PER_SEC / elapsed_us;
        g_print("Frame rendered: %dx%d frame=%" G_GUINT64_FORMAT " fps=%.1f\n",
                wpe_buffer_get_width(buffer),
                wpe_buffer_get_height(buffer),
                frame_count,
                fps);
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
    WebKitWebContext *context = webkit_web_context_new();
    webkit_web_context_add_path_to_sandbox(context, "/userdisk", TRUE);
    webkit_web_context_add_path_to_sandbox(context, "/tmp", FALSE);
    g_print("WebKit context: 2022 GLib API sandbox_paths=/userdisk,/tmp\n");
    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_javascript_markup(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_enable_2d_canvas_acceleration(settings, FALSE);
    webkit_settings_set_enable_media(settings, TRUE);
    webkit_settings_set_enable_webaudio(settings, TRUE);
    webkit_settings_set_enable_mediasource(settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    webkit_settings_set_media_playback_allows_inline(settings, TRUE);
    g_object_set(settings, "enable-write-console-messages-to-stdout", TRUE, NULL);
    g_print("Settings: javascript=%d javascript_markup=%d file_access=%d webgl=%d canvas_accel=%d media=%d webaudio=%d mediasource=%d\n",
            webkit_settings_get_enable_javascript(settings),
            webkit_settings_get_enable_javascript_markup(settings),
            webkit_settings_get_allow_file_access_from_file_urls(settings),
            webkit_settings_get_enable_webgl(settings),
            webkit_settings_get_enable_2d_canvas_acceleration(settings),
            webkit_settings_get_enable_media(settings),
            webkit_settings_get_enable_webaudio(settings),
            webkit_settings_get_enable_mediasource(settings));
    WebKitUserContentManager *user_content_manager = webkit_user_content_manager_new();
    WebKitWebsitePolicies *policies = webkit_website_policies_new_with_policies(
        "autoplay", WEBKIT_AUTOPLAY_ALLOW,
        NULL);
    WebKitNetworkSession *network_session = webkit_network_session_get_default();
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

    g_signal_connect(web_view, "load-failed",
                     G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(web_view, "load-failed-with-tls-errors",
                     G_CALLBACK(on_load_failed_with_tls_errors), network_session);
    g_signal_connect(web_view, "web-process-terminated",
                     G_CALLBACK(on_web_process_terminated), NULL);

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
        state->touch_fd = -1;
        state->viewport_width = width;
        state->viewport_height = height;
        chrome_load_state(state, url);
        setup_keyboard_bridge(state);
        setup_keyboard_user_script(user_content_manager, state);
        g_signal_connect(web_view, "load-changed",
                         G_CALLBACK(on_load_changed), state);
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
        const char *viewport_file = g_getenv("WPE_VIEWPORT_FILE");
        if (env_enabled("WPE_ENABLE_VIEWPORT_FILE", FALSE) && viewport_file && viewport_file[0]) {
            state->viewport_file = g_strdup(viewport_file);
            viewport_file_tick(state);
            g_timeout_add(500, viewport_file_tick, state);
            g_print("Viewport hot-update file: %s\n", state->viewport_file);
        }
        setup_raw_touch(state);
        chrome_apply_layout(state, "startup");
        g_timeout_add_seconds(1, print_view_state, state);
        if (g_getenv("WPE_DRM_EVAL_TICK"))
            g_timeout_add_seconds(1, eval_tick, web_view);
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
    g_main_loop_run(main_loop);

    /* Cleanup */
    g_main_loop_unref(main_loop);
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
        g_free(state->viewport_file);
        g_free(state->viewport_spec);
        g_free(state);
    }
    g_object_unref(web_view);
    g_object_unref(context);
    g_object_unref(display);
    if (tls_exception_hosts) {
        g_hash_table_destroy(tls_exception_hosts);
        tls_exception_hosts = NULL;
    }

    return 0;
}
