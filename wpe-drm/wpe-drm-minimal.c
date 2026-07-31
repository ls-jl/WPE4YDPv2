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
#include "ChromeMotionState.h"
#include "browser-chrome-model.h"
#include "browser-navigation.h"
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
#define CHROME_TABS_HEADER_HEIGHT 0
#define CHROME_TABS_FOOTER_HEIGHT 0
#define CHROME_TABS_ROW_HEIGHT 48
#define CHROME_TABS_CLOSE_HIT_WIDTH 52
#define CHROME_MENU_COLUMNS 3
#define CHROME_MENU_ROWS 2
#define CHROME_MENU_ITEMS_PER_PAGE (CHROME_MENU_COLUMNS * CHROME_MENU_ROWS)
#define CHROME_MENU_ITEM_COUNT 6
#define CHROME_LIST_HEADER_HEIGHT 32
#define CHROME_LIST_ROW_HEIGHT 48
#define CHROME_GPU_RESTART_CODE 73
#define CHROME_USER_EXIT_CODE 74

typedef enum {
    CHROME_PANEL_GESTURE_NONE = 0,
    CHROME_PANEL_GESTURE_TABS_VERTICAL,
    CHROME_PANEL_GESTURE_MENU_HORIZONTAL,
    CHROME_PANEL_GESTURE_LIST_VERTICAL,
} ChromePanelGesture;

typedef enum {
    GAME_GESTURE_NONE = 0,
    GAME_GESTURE_PENDING,
    GAME_GESTURE_TOUCH,
} GameGesture;

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
    ChromePanelGesture panel_gesture;
    double panel_start_offset;
    double panel_velocity;
    guint32 panel_last_time_ms;
    GameGesture game_gesture;
    guint game_hold_source_id;
    guint32 game_sequence_id;
    guint32 game_down_time_ms;
    gpointer game_state;
    gboolean game_video_mapped;
    int game_dom_x;
    int game_dom_y;
    int game_dom_width;
    int game_dom_height;
    int game_visible_x;
    int game_visible_y;
    int game_visible_width;
    int game_visible_height;
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
    ChromePanel dialog_parent;
    double scroll_accum;
    double scroll_velocity_peak;
    guint32 scroll_last_time_ms;
    int scroll_direction;
    gboolean suppress_history;
    guint layout_timer;
    char site_profile[16];
    BrowserCookiePolicy cookie_policy;
    char search_engine[16];
    char *custom_search_template;
    double page_zoom;
    guint default_font_size;
    gboolean javascript_enabled;
    gboolean autoplay_requires_gesture;
    gboolean smooth_scrolling;
    gboolean block_popups;
    gboolean restore_tabs;
    BrowserGlobalSettings global_settings;
    guint panel_page;
    gboolean panel_delete_mode;
    gboolean site_data_clearing;
    char site_data_status[64];
    GPtrArray *panel_profiles;
    GPtrArray *panel_pages;
    int64_t current_visit_id;
    double tabs_scroll_offset;
    guint menu_page;
    double menu_scroll_offset;
    guint menu_snap_source_id;
    double menu_snap_from;
    double menu_snap_to;
    gint64 menu_snap_start_us;
    double panel_scroll_offsets[CHROME_PANEL_COUNT];
    int pressed_row;
    int pressed_segment;
    int pressed_control;
    WPEChromeMotionState motion;
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
    guint32 next_touch_sequence;
    double game_drag_threshold;
    guint game_hold_delay_ms;
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
    gboolean cloud_autostart;
    gboolean cloud_launch_requested;
    gboolean page_fullscreen;
    gboolean chrome_visible_before_fullscreen;
    gboolean game_media_immersive;
    gboolean chrome_visible_before_game_media;
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
static void update_page_zoom_for_uri(AppState *state, const char *uri);

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

static const char *gpu_runtime_status_label(const BrowserGlobalSettings *settings)
{
    if (gpu_render_profile)
        return "ACTIVE";
    if (settings && !settings->gpu_acceleration)
        return "OFF";
    const char *status = g_getenv("WPE_GPU_STATUS");
    if (!g_strcmp0(status, "unavailable"))
        return "UNAVAILABLE";
    if (!g_strcmp0(status, "fallback"))
        return "CPU FALLBACK";
    return "CPU FALLBACK";
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

static int chrome_panel_line_count(const AppState *state, ChromePanel panel)
{
    const BrowserChrome *chrome = state ? &state->chrome : NULL;
    guint count = chrome_panel_default_line_count(panel);
    if (panel == CHROME_PANEL_PRIVACY && chrome && chrome->site_data_status[0])
        count++;
    return (int)count;
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

static void chrome_apply_web_preferences(AppState *state, gboolean reload)
{
    if (!state || !state->web_view)
        return;
    BrowserChrome *chrome = &state->chrome;
    WebKitSettings *settings = webkit_web_view_get_settings(state->web_view);
    if (settings) {
        webkit_settings_set_enable_javascript(settings, chrome->javascript_enabled);
        webkit_settings_set_default_font_size(settings, chrome->default_font_size);
        webkit_settings_set_media_playback_requires_user_gesture(
            settings, chrome->autoplay_requires_gesture);
        webkit_settings_set_enable_smooth_scrolling(settings, chrome->smooth_scrolling);
        webkit_settings_set_javascript_can_open_windows_automatically(
            settings, !chrome->block_popups);
    }
    chrome_apply_site_profile(state);
    update_page_zoom_for_uri(state, webkit_web_view_get_uri(state->web_view));
    g_print("Web preferences applied: js=%d autoplay_gesture=%d smooth=%d "
            "block_popups=%d font=%u zoom=%.2f reload=%d\n",
            chrome->javascript_enabled, chrome->autoplay_requires_gesture,
            chrome->smooth_scrolling, chrome->block_popups,
            chrome->default_font_size, chrome->page_zoom, reload);
    if (reload) {
        chrome->loading = TRUE;
        chrome->load_progress = 0;
        webkit_web_view_reload(state->web_view);
    }
}

static void chrome_apply_layout(AppState *state, const char *reason);
static void chrome_set_visible(AppState *state, gboolean visible);
static void chrome_update_render_state(AppState *state);
static void chrome_save_state(AppState *state);
static void chrome_cancel_menu_snap(BrowserChrome *chrome);
static void chrome_publish_motion(AppState *state, gboolean dragging);

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

typedef struct {
    int panel_x;
    int panel_y;
    int panel_width;
    int panel_height;
    int columns;
    int rows;
    int item_count;
    int page_count;
    double max_scroll;
} ChromeMenuGeometry;

typedef struct {
    int panel_x;
    int panel_y;
    int panel_width;
    int panel_height;
    int header_height;
    int row_height;
    int list_top;
    int list_height;
    int line_count;
    double max_scroll;
} ChromeListGeometry;

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
    int tabs_width = MAX(1, panel_width / 2);
    ChromeTabsGeometry geometry = {
        .panel_x = panel_width - tabs_width,
        .panel_y = chrome_height,
        .panel_width = tabs_width,
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

static ChromeMenuGeometry chrome_menu_geometry(AppState *state)
{
    int panel_width = state && state->panel_width > 0 ? state->panel_width : 960;
    int panel_height = state && state->panel_height > 0 ? state->panel_height : 266;
    int chrome_height = state ? state->chrome.height : 44;
    int menu_x = panel_width / 3;
    ChromeMenuGeometry geometry = {
        .panel_x = menu_x,
        .panel_y = chrome_height,
        .panel_width = panel_width - menu_x,
        .panel_height = MAX(0, panel_height - chrome_height),
        .columns = CHROME_MENU_COLUMNS,
        .rows = CHROME_MENU_ROWS,
        .item_count = CHROME_MENU_ITEM_COUNT,
        .page_count = (CHROME_MENU_ITEM_COUNT + CHROME_MENU_ITEMS_PER_PAGE - 1)
            / CHROME_MENU_ITEMS_PER_PAGE,
    };
    geometry.max_scroll = MAX(0, geometry.page_count - 1) * geometry.panel_width;
    return geometry;
}

static ChromeListGeometry chrome_list_geometry(AppState *state, ChromePanel panel)
{
    int panel_width = state && state->panel_width > 0 ? state->panel_width : 960;
    int panel_height = state && state->panel_height > 0 ? state->panel_height : 266;
    int chrome_height = state ? state->chrome.height : 44;
    ChromeListGeometry geometry = {
        .panel_x = 0,
        .panel_y = chrome_height,
        .panel_width = panel_width,
        .panel_height = MAX(0, panel_height - chrome_height),
        .header_height = CHROME_LIST_HEADER_HEIGHT,
        .row_height = CHROME_LIST_ROW_HEIGHT,
        .line_count = chrome_panel_line_count(state, panel),
    };
    geometry.list_top = geometry.panel_y + geometry.header_height;
    geometry.list_height = MAX(0, geometry.panel_height - geometry.header_height);
    geometry.max_scroll = MAX(0, geometry.line_count * geometry.row_height
                                  - geometry.list_height);
    return geometry;
}

static void chrome_clamp_list_scroll(AppState *state, ChromePanel panel)
{
    if (!state || !chrome_panel_is_internal_list(panel))
        return;
    ChromeListGeometry geometry = chrome_list_geometry(state, panel);
    state->chrome.panel_scroll_offsets[panel] =
        CLAMP(state->chrome.panel_scroll_offsets[panel], 0.0, geometry.max_scroll);
}

static void chrome_open_internal_panel(AppState *state, ChromePanel panel,
                                       gboolean reset_scroll)
{
    if (!state || !chrome_panel_is_internal_list(panel))
        return;
    BrowserChrome *chrome = &state->chrome;
    chrome_cancel_menu_snap(chrome);
    chrome->panel = panel;
    if (reset_scroll)
        chrome->panel_scroll_offsets[panel] = 0;
    chrome_clamp_list_scroll(state, panel);
    chrome_publish_motion(state, FALSE);
}

static void chrome_cancel_menu_snap(BrowserChrome *chrome)
{
    if (!chrome || !chrome->menu_snap_source_id)
        return;
    g_source_remove(chrome->menu_snap_source_id);
    chrome->menu_snap_source_id = 0;
}

static void chrome_clamp_menu_scroll(AppState *state)
{
    ChromeMenuGeometry geometry = chrome_menu_geometry(state);
    state->chrome.menu_scroll_offset = CLAMP(state->chrome.menu_scroll_offset,
                                              0.0, geometry.max_scroll);
}

static void chrome_publish_motion(AppState *state, gboolean dragging)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    WPEChromeMotionPanel panel = WPE_CHROME_MOTION_PANEL_NONE;
    double offset = 0;
    if (chrome->panel == CHROME_PANEL_TABS) {
        panel = WPE_CHROME_MOTION_PANEL_TABS;
        offset = chrome->tabs_scroll_offset;
    } else if (chrome->panel == CHROME_PANEL_MENU) {
        panel = WPE_CHROME_MOTION_PANEL_MENU;
        offset = chrome->menu_scroll_offset;
    } else if (chrome_panel_is_internal_list(chrome->panel)) {
        panel = WPE_CHROME_MOTION_PANEL_LIST;
        offset = chrome->panel_scroll_offsets[chrome->panel];
    }
    chrome->motion.version = WPE_CHROME_MOTION_VERSION;
    chrome->motion.panel = panel;
    chrome->motion.flags = dragging ? WPE_CHROME_MOTION_FLAG_DRAGGING : 0;
    chrome->motion.offset = offset;
    chrome->motion.pressed_row = chrome->pressed_row;
    chrome->motion.pressed_segment = chrome->pressed_segment;
    chrome->motion.pressed_control = chrome->pressed_control;
    chrome->motion.sequence++;
    if (!chrome->motion.sequence)
        chrome->motion.sequence = 1;
}

static void chrome_close_panel(AppState *state)
{
    if (!state)
        return;
    chrome_cancel_menu_snap(&state->chrome);
    state->chrome.panel = CHROME_PANEL_NONE;
    chrome_publish_motion(state, FALSE);
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
    chrome_cancel_menu_snap(&state->chrome);
    state->chrome.panel = CHROME_PANEL_TABS;
    chrome_set_visible(state, TRUE);
    chrome_position_active_tab(state);
    chrome_publish_motion(state, FALSE);
}

static void chrome_open_menu_panel(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    chrome_cancel_menu_snap(chrome);
    chrome->panel = CHROME_PANEL_MENU;
    chrome->panel_page = 0;
    chrome->panel_delete_mode = FALSE;
    chrome->menu_page = 0;
    chrome->menu_scroll_offset = 0;
    chrome_set_visible(state, TRUE);
    chrome_publish_motion(state, FALSE);
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

static void chrome_set_line_metadata(GKeyFile *key_file, int line,
                                     gboolean enabled, gboolean danger)
{
    char key[48];
    snprintf(key, sizeof(key), "line%d_enabled", line);
    g_key_file_set_boolean(key_file, "panel", key, enabled);
    snprintf(key, sizeof(key), "line%d_danger", line);
    g_key_file_set_boolean(key_file, "panel", key, danger);
}

static void chrome_set_line_style(GKeyFile *key_file, int line,
                                  const char *kind, const char *value,
                                  gboolean checked, const char *icon)
{
    char key[48];
    snprintf(key, sizeof(key), "line%d_kind", line);
    g_key_file_set_string(key_file, "panel", key, kind ? kind : "action");
    snprintf(key, sizeof(key), "line%d_value", line);
    g_key_file_set_string(key_file, "panel", key, value ? value : "");
    snprintf(key, sizeof(key), "line%d_checked", line);
    g_key_file_set_boolean(key_file, "panel", key, checked);
    snprintf(key, sizeof(key), "line%d_icon", line);
    g_key_file_set_string(key_file, "panel", key, icon ? icon : "");
}

static void chrome_set_segmented_line(GKeyFile *key_file, int line, int count,
                                      const char *const *labels,
                                      const gboolean *enabled,
                                      const gboolean *danger)
{
    char key[64];
    snprintf(key, sizeof(key), "line%d_segment_count", line);
    g_key_file_set_integer(key_file, "panel", key, count);
    for (int segment = 0; segment < count; ++segment) {
        snprintf(key, sizeof(key), "line%d_segment%d_label", line, segment);
        g_key_file_set_string(key_file, "panel", key, labels[segment]);
        snprintf(key, sizeof(key), "line%d_segment%d_enabled", line, segment);
        g_key_file_set_boolean(key_file, "panel", key, enabled[segment]);
        snprintf(key, sizeof(key), "line%d_segment%d_danger", line, segment);
        g_key_file_set_boolean(key_file, "panel", key, danger[segment]);
    }
}

static void chrome_write_panel_lines(GKeyFile *key_file, AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    BrowserTab *tab = chrome_active_tab(chrome);
    char line[128];
    int line_count = 0;
    int content_line_count = -1;

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
        g_key_file_set_string(key_file, "panel", "line3", "PROFILES");
        g_key_file_set_string(key_file, "panel", "line4", "PRIVACY");
        g_key_file_set_string(key_file, "panel", "line5", "SETTINGS");
        line_count = 6;
        static const char *item_ids[CHROME_MENU_ITEM_COUNT] = {
            "bookmark", "history", "bookmarks", "profiles", "privacy", "settings"
        };
        ChromeMenuGeometry geometry = chrome_menu_geometry(state);
        g_key_file_set_integer(key_file, "panel", "x", geometry.panel_x);
        g_key_file_set_integer(key_file, "panel", "y", geometry.panel_y);
        g_key_file_set_integer(key_file, "panel", "width", geometry.panel_width);
        g_key_file_set_integer(key_file, "panel", "height", geometry.panel_height);
        g_key_file_set_integer(key_file, "panel", "columns", geometry.columns);
        g_key_file_set_integer(key_file, "panel", "rows", geometry.rows);
        g_key_file_set_integer(key_file, "panel", "page_count", geometry.page_count);
        g_key_file_set_double(key_file, "panel", "scroll_offset", chrome->menu_scroll_offset);
        for (int index = 0; index < CHROME_MENU_ITEM_COUNT; ++index) {
            char key[32];
            snprintf(key, sizeof(key), "item%d_id", index);
            g_key_file_set_string(key_file, "panel", key, item_ids[index]);
            snprintf(key, sizeof(key), "item%d_enabled", index);
            g_key_file_set_boolean(key_file, "panel", key,
                                   index != 0 || !profile_runtime.guest);
        }
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
        g_key_file_set_string(key_file, "panel", "line0", "APPEARANCE");
        g_key_file_set_string(key_file, "panel", "line1", "WEB PAGE");
        g_key_file_set_string(key_file, "panel", "line2", "STARTUP & SEARCH");
        g_key_file_set_string(key_file, "panel", "line3", "PRIVACY & DATA");
        g_key_file_set_string(key_file, "panel", "line4", "ABOUT");
        line_count = 5;
    } else if (chrome->panel == CHROME_PANEL_APPEARANCE) {
        char zoom[16];
        g_snprintf(zoom, sizeof(zoom), "%d%%", (int)lround(chrome->page_zoom * 100));
        const char *font = chrome->default_font_size <= 14 ? "SMALL"
            : chrome->default_font_size >= 20 ? "LARGE" : "STANDARD";
        g_key_file_set_string(key_file, "panel", "line0", "THEME");
        g_key_file_set_string(key_file, "panel", "line1", "TOOLBAR AUTO-HIDE");
        g_key_file_set_string(key_file, "panel", "line2", "PAGE ZOOM");
        g_key_file_set_string(key_file, "panel", "line3", "DEFAULT FONT");
        g_key_file_set_string(key_file, "panel", "line4", "GPU ACCELERATION");
        line_count = 5;
        chrome_set_line_style(key_file, 0, "choice",
                              !g_ascii_strcasecmp(chrome->global_settings.theme, "dark")
                                ? "DARK" : "LIGHT", FALSE, "theme");
        chrome_set_line_style(key_file, 1, "toggle", "",
                              chrome->global_settings.toolbar_auto_hide, "toolbar");
        chrome_set_line_style(key_file, 2, "choice", zoom, FALSE, "zoom");
        chrome_set_line_style(key_file, 3, "choice", font, FALSE, "font");
        chrome_set_line_style(key_file, 4, "toggle",
                              gpu_runtime_status_label(&chrome->global_settings),
                              chrome->global_settings.gpu_acceleration, "renderer");
    } else if (chrome->panel == CHROME_PANEL_WEB) {
        g_key_file_set_string(key_file, "panel", "line0", "SITE MODE");
        g_key_file_set_string(key_file, "panel", "line1", "JAVASCRIPT");
        g_key_file_set_string(key_file, "panel", "line2", "AUTOPLAY");
        g_key_file_set_string(key_file, "panel", "line3", "SMOOTH SCROLL");
        g_key_file_set_string(key_file, "panel", "line4", "BLOCK POPUPS");
        line_count = 5;
        chrome_set_line_style(key_file, 0, "choice",
                              !g_strcmp0(normalize_site_profile(chrome->site_profile), "desktop")
                                ? "DESKTOP" : "MOBILE", FALSE, "desktop");
        chrome_set_line_style(key_file, 1, "toggle", "",
                              chrome->javascript_enabled, "javascript");
        chrome_set_line_style(key_file, 2, "choice",
                              chrome->autoplay_requires_gesture ? "USER ACTION" : "ALLOW",
                              FALSE, "autoplay");
        chrome_set_line_style(key_file, 3, "toggle", "",
                              chrome->smooth_scrolling, "scroll");
        chrome_set_line_style(key_file, 4, "toggle", "",
                              chrome->block_popups, "popup");
    } else if (chrome->panel == CHROME_PANEL_STARTUP) {
        const char *engine = !g_strcmp0(chrome->search_engine, "bing") ? "BING"
            : !g_strcmp0(chrome->search_engine, "google") ? "GOOGLE"
            : !g_strcmp0(chrome->search_engine, "custom") ? "CUSTOM" : "BAIDU";
        g_key_file_set_string(key_file, "panel", "line0", "SEARCH ENGINE");
        g_key_file_set_string(key_file, "panel", "line1", "CUSTOM SEARCH TEMPLATE");
        g_key_file_set_string(key_file, "panel", "line2", "HOME PAGE");
        g_key_file_set_string(key_file, "panel", "line3", "RESTORE TABS");
        line_count = 4;
        chrome_set_line_style(key_file, 0, "choice", engine, FALSE, "search");
        chrome_set_line_style(key_file, 1, "navigation",
                              browser_navigation_custom_search_template_valid(chrome->custom_search_template)
                                ? "SET" : "NOT SET", FALSE, "custom");
        chrome_set_line_style(key_file, 2, "navigation",
                              chrome->home_url ? chrome->home_url : default_home_url(),
                              FALSE, "home");
        chrome_set_line_style(key_file, 3, "toggle", "",
                              chrome->restore_tabs, "tabs");
    } else if (chrome->panel == CHROME_PANEL_SETTINGS_PRIVACY) {
        g_key_file_set_string(key_file, "panel", "line0", "COOKIE POLICY");
        g_key_file_set_string(key_file, "panel", "line1", "CLEAR COOKIES & SITE DATA");
        g_key_file_set_string(key_file, "panel", "line2", "CLEAR CACHE");
        g_key_file_set_string(key_file, "panel", "line3", "CLEAR HISTORY");
        line_count = 4;
        chrome_set_line_style(key_file, 0, "choice",
                              cookie_policy_label(chrome->cookie_policy),
                              FALSE, "privacy");
        chrome_set_line_style(key_file, 1, "action", "", FALSE, "clear");
        chrome_set_line_style(key_file, 2, "action", "", FALSE, "cache");
        chrome_set_line_style(key_file, 3, "action", "", FALSE, "history");
    } else if (chrome->panel == CHROME_PANEL_ABOUT) {
        char display[64];
        g_snprintf(display, sizeof(display), "%dx%d  R%d",
                   state->panel_width, state->panel_height, state->panel_rotation);
        g_key_file_set_string(key_file, "panel", "line0", "DIRECT WPE DRM BROWSER");
        g_key_file_set_string(key_file, "panel", "line1", "WEBKIT RUNTIME");
        g_key_file_set_string(key_file, "panel", "line2", "CURRENT PROFILE");
        g_key_file_set_string(key_file, "panel", "line3", "RENDERER");
        g_key_file_set_string(key_file, "panel", "line4", "DISPLAY");
        line_count = 5;
        chrome_set_line_style(key_file, 0, "info", "1.0", FALSE, "about");
        chrome_set_line_style(key_file, 1, "info", "2.53.3", FALSE, "webkit");
        chrome_set_line_style(key_file, 2, "info",
                              profile_runtime.profile.name
                                ? profile_runtime.profile.name : "DEFAULT",
                              FALSE, "profiles");
        chrome_set_line_style(key_file, 3, "info",
                              gpu_runtime_status_label(&chrome->global_settings),
                              FALSE, "renderer");
        chrome_set_line_style(key_file, 4, "info", display, FALSE, "display");
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
        content_line_count = line_count;
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

    if (chrome_panel_is_internal_list(chrome->panel)) {
        ChromeListGeometry geometry = chrome_list_geometry(state, chrome->panel);
        chrome_clamp_list_scroll(state, chrome->panel);
        g_key_file_set_integer(key_file, "panel", "x", geometry.panel_x);
        g_key_file_set_integer(key_file, "panel", "y", geometry.panel_y);
        g_key_file_set_integer(key_file, "panel", "width", geometry.panel_width);
        g_key_file_set_integer(key_file, "panel", "height", geometry.panel_height);
        g_key_file_set_integer(key_file, "panel", "header_height", geometry.header_height);
        g_key_file_set_integer(key_file, "panel", "row_height", geometry.row_height);
        g_key_file_set_double(key_file, "panel", "scroll_offset",
                              chrome->panel_scroll_offsets[chrome->panel]);
        g_key_file_set_double(key_file, "panel", "max_scroll", geometry.max_scroll);
        for (int index = 0; index < line_count; ++index)
            chrome_set_line_metadata(key_file, index, TRUE, FALSE);

        if (chrome->panel == CHROME_PANEL_SETTINGS) {
            static const char *icons[] = {
                "theme", "web", "search", "privacy", "about"
            };
            for (int index = 0; index < line_count; ++index)
                chrome_set_line_style(key_file, index, "navigation", "",
                                      FALSE, icons[index]);
        } else if (chrome->panel == CHROME_PANEL_SETTINGS_PRIVACY) {
            chrome_set_line_metadata(key_file, 1, TRUE, TRUE);
            chrome_set_line_metadata(key_file, 3, !profile_runtime.guest, TRUE);
        } else if (chrome->panel == CHROME_PANEL_ABOUT) {
            for (int index = 0; index < line_count; ++index)
                chrome_set_line_metadata(key_file, index, TRUE, FALSE);
        } else if (chrome->panel == CHROME_PANEL_PRIVACY) {
            chrome_set_line_metadata(key_file, 1, !chrome->site_data_clearing, TRUE);
        } else if (chrome->panel == CHROME_PANEL_CLEAR_SITE_DATA
                   || chrome->panel == CHROME_PANEL_PROFILE_DELETE
                   || chrome->panel == CHROME_PANEL_CLEAR_HISTORY) {
            chrome_set_line_metadata(key_file, 1, TRUE, TRUE);
        } else if (chrome->panel == CHROME_PANEL_PROFILE_MANAGE) {
            chrome_set_line_metadata(key_file, 1,
                                     !profile_runtime.guest
                                         && !profile_runtime.profile.is_default,
                                     TRUE);
        } else if (chrome->panel == CHROME_PANEL_PROFILES) {
            const char *row4_labels[] = { "GUEST", "NEW PROFILE" };
            const gboolean row4_enabled[] = { TRUE, TRUE };
            const gboolean row4_danger[] = { FALSE, FALSE };
            chrome_set_segmented_line(key_file, 4, 2, row4_labels,
                                      row4_enabled, row4_danger);
            gboolean has_previous = chrome->panel_page > 0;
            gboolean has_next = chrome->panel_profiles
                && (chrome->panel_page + 1) * 4 < chrome->panel_profiles->len;
            const char *row5_labels[] = { "PREV", "NEXT", "MANAGE ACTIVE" };
            const gboolean row5_enabled[] = {
                has_previous, has_next, !profile_runtime.guest
            };
            const gboolean row5_danger[] = { FALSE, FALSE, FALSE };
            chrome_set_segmented_line(key_file, 5, 3, row5_labels,
                                      row5_enabled, row5_danger);
        } else if (chrome->panel == CHROME_PANEL_HISTORY
                   || chrome->panel == CHROME_PANEL_BOOKMARKS) {
            for (int index = 0; index < 6; ++index)
                chrome_set_line_metadata(key_file, index,
                                         index < content_line_count,
                                         index < content_line_count
                                             && chrome->panel_delete_mode);
            gboolean has_previous = chrome->panel_page > 0;
            gboolean has_next = chrome->panel_pages
                && chrome->panel_pages->len == 6;
            const char *row6_labels[] = { "PREV PAGE", "NEXT PAGE" };
            const gboolean row6_enabled[] = { has_previous, has_next };
            const gboolean row6_danger[] = { FALSE, FALSE };
            chrome_set_segmented_line(key_file, 6, 2, row6_labels,
                                      row6_enabled, row6_danger);
            if (chrome->panel == CHROME_PANEL_HISTORY) {
                const char *row7_labels[] = {
                    chrome->panel_delete_mode ? "DONE DELETING" : "DELETE MODE",
                    "CLEAR HISTORY"
                };
                const gboolean row7_enabled[] = { TRUE, !profile_runtime.guest };
                const gboolean row7_danger[] = { chrome->panel_delete_mode, TRUE };
                chrome_set_segmented_line(key_file, 7, 2, row7_labels,
                                          row7_enabled, row7_danger);
            } else
                chrome_set_line_metadata(key_file, 7, TRUE,
                                         chrome->panel_delete_mode);
        }
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
    g_key_file_set_boolean(key_file, "chrome", "page_fullscreen", state->page_fullscreen);
    g_key_file_set_integer(key_file, "chrome", "height", chrome->height);
    g_key_file_set_integer(key_file, "chrome", "tab_count", chrome->tab_count);
    g_key_file_set_integer(key_file, "chrome", "active_tab", chrome->active);
    g_key_file_set_string(key_file, "chrome", "site_profile", normalize_site_profile(chrome->site_profile));
    g_key_file_set_string(key_file, "chrome", "profile",
                          profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT");
    g_key_file_set_boolean(key_file, "chrome", "guest_profile", profile_runtime.guest);
    g_key_file_set_string(key_file, "chrome", "theme",
                          !g_ascii_strcasecmp(chrome->global_settings.theme, "dark")
                            ? "dark" : "light");
    g_key_file_set_boolean(key_file, "chrome", "toolbar_auto_hide",
                           chrome->global_settings.toolbar_auto_hide);
    g_key_file_set_boolean(key_file, "chrome", "gpu_acceleration",
                           chrome->global_settings.gpu_acceleration);
    g_key_file_set_string(key_file, "chrome", "gpu_status",
                          gpu_runtime_status_label(&chrome->global_settings));
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

static gboolean chrome_menu_snap_tick(gpointer user_data)
{
    AppState *state = user_data;
    if (!state)
        return G_SOURCE_REMOVE;
    BrowserChrome *chrome = &state->chrome;
    if (chrome->panel != CHROME_PANEL_MENU) {
        chrome->menu_snap_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    double elapsed = (double)(g_get_monotonic_time() - chrome->menu_snap_start_us)
        / (120.0 * 1000.0);
    double t = CLAMP(elapsed, 0.0, 1.0);
    double eased = 1.0 - pow(1.0 - t, 3.0);
    chrome->menu_scroll_offset = chrome->menu_snap_from
        + (chrome->menu_snap_to - chrome->menu_snap_from) * eased;
    chrome_clamp_menu_scroll(state);
    chrome_publish_motion(state, t < 1.0);

    if (t < 1.0)
        return G_SOURCE_CONTINUE;

    ChromeMenuGeometry geometry = chrome_menu_geometry(state);
    chrome->menu_page = geometry.panel_width > 0
        ? (guint)llround(chrome->menu_scroll_offset / geometry.panel_width) : 0;
    chrome->menu_scroll_offset = chrome->menu_page * geometry.panel_width;
    chrome_publish_motion(state, FALSE);
    chrome->menu_snap_source_id = 0;
    chrome_update_render_state(state);
    return G_SOURCE_REMOVE;
}

static void chrome_snap_menu(AppState *state, double start_offset,
                             double drag_distance, double velocity)
{
    if (!state || state->chrome.panel != CHROME_PANEL_MENU)
        return;
    BrowserChrome *chrome = &state->chrome;
    ChromeMenuGeometry geometry = chrome_menu_geometry(state);
    chrome_clamp_menu_scroll(state);
    if (geometry.page_count <= 1 || geometry.panel_width <= 0) {
        chrome->menu_page = 0;
        chrome->menu_scroll_offset = 0;
        chrome_publish_motion(state, FALSE);
        chrome_update_render_state(state);
        return;
    }

    int current_page = CLAMP((int)llround(start_offset / geometry.panel_width),
                             0, geometry.page_count - 1);
    int target_page = current_page;
    double distance_threshold = MAX(48.0, geometry.panel_width * 0.2);
    if (drag_distance > distance_threshold || velocity > 600.0)
        target_page++;
    else if (drag_distance < -distance_threshold || velocity < -600.0)
        target_page--;
    target_page = CLAMP(target_page, 0, geometry.page_count - 1);

    chrome_cancel_menu_snap(chrome);
    chrome->menu_page = target_page;
    chrome->menu_snap_from = chrome->menu_scroll_offset;
    chrome->menu_snap_to = target_page * geometry.panel_width;
    chrome->menu_snap_start_us = g_get_monotonic_time();
    chrome->menu_snap_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, 16,
                                                      chrome_menu_snap_tick, state, NULL);
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
            && browser_profile_store_save_preferences(profile_runtime.store,
                &(BrowserProfile) {
                    .id = profile_runtime.profile.id,
                    .search_engine = chrome->search_engine,
                    .custom_search_template = chrome->custom_search_template,
                    .page_zoom = chrome->page_zoom,
                    .default_font_size = chrome->default_font_size,
                    .javascript_enabled = chrome->javascript_enabled,
                    .autoplay_requires_gesture = chrome->autoplay_requires_gesture,
                    .smooth_scrolling = chrome->smooth_scrolling,
                    .block_popups = chrome->block_popups,
                    .restore_tabs = chrome->restore_tabs,
                }, &error)
            && browser_profile_store_save_tabs(profile_runtime.store,
                profile_runtime.profile.id, tabs, chrome->next_tab_id, &error);
        if (!success)
            g_warning("Profile state save failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        g_ptr_array_unref(tabs);
    }
    if (profile_runtime.store) {
        GError *error = NULL;
        if (!browser_profile_store_save_global_settings(profile_runtime.store,
                &chrome->global_settings, &error))
            g_warning("Global UI settings save failed: %s",
                      error ? error->message : "unknown");
        g_clear_error(&error);
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
    if (state->page_fullscreen)
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
    chrome_close_panel(state);
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
        chrome_open_tabs_panel(state);
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

static char *search_url_for_query(const BrowserChrome *chrome, const char *query)
{
    return browser_navigation_search_url(
        chrome ? chrome->search_engine : NULL,
        chrome ? chrome->custom_search_template : NULL,
        query);
}

static char *normalize_user_url(AppState *state, const char *raw)
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

    char *scheme = browser_navigation_uri_scheme(value);
    if (scheme) {
        if (browser_navigation_scheme_allowed(scheme)) {
            g_free(scheme);
            return value;
        }
        if (!g_ascii_strcasecmp(scheme, "bilibili")) {
            char *web_url = browser_navigation_bilibili_web_url(value);
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
        char *url = search_url_for_query(state ? &state->chrome : NULL, value);
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
            char *url = normalize_user_url(state, final_text);
            if (url)
                chrome_load_url(state, url, TRUE);
            else
                g_warning("Ignoring blocked address input: bytes=%zu", strlen(final_text));
            g_free(url);
        } else if (!g_strcmp0(state->keyboard_active_kind, "home_url")) {
            char *url = normalize_user_url(state, final_text);
            if (url) {
                g_free(state->chrome.home_url);
                state->chrome.home_url = url;
                chrome_open_internal_panel(state, CHROME_PANEL_STARTUP, FALSE);
                chrome_save_state(state);
                chrome_request_frame(state);
            } else
                g_warning("Ignoring blocked home URL input: bytes=%zu", strlen(final_text));
        } else if (!g_strcmp0(state->keyboard_active_kind, "custom_search")) {
            char *candidate = g_strdup(final_text);
            g_strstrip(candidate);
            if (browser_navigation_custom_search_template_valid(candidate)) {
                g_free(state->chrome.custom_search_template);
                state->chrome.custom_search_template = candidate;
                g_strlcpy(state->chrome.search_engine, "custom",
                          sizeof(state->chrome.search_engine));
                chrome_open_internal_panel(state, CHROME_PANEL_STARTUP, FALSE);
                chrome_save_state(state);
                chrome_request_frame(state);
                g_print("Custom search template saved: %s\n",
                        state->chrome.custom_search_template);
            } else {
                g_warning("Rejected custom search template: HTTPS and exactly one %%s required");
                g_free(candidate);
            }
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
                chrome_open_internal_panel(state, CHROME_PANEL_PROFILE_MANAGE,
                                           FALSE);
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
    g_print("Site profile changed: %s\n", next);
    chrome_save_state(state);
    chrome_apply_web_preferences(state, TRUE);
    chrome_update_render_state(state);
    chrome_request_frame(state);
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

static void chrome_toggle_theme(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    g_strlcpy(chrome->global_settings.theme,
              !g_ascii_strcasecmp(chrome->global_settings.theme, "dark")
                ? "light" : "dark",
              sizeof(chrome->global_settings.theme));
    chrome_save_state(state);
    chrome_request_frame(state);
}

static void chrome_cycle_page_zoom(AppState *state)
{
    static const double values[] = { 0.75, 0.90, 1.00, 1.10, 1.25 };
    BrowserChrome *chrome = &state->chrome;
    guint closest = 0;
    double distance = G_MAXDOUBLE;
    for (guint index = 0; index < G_N_ELEMENTS(values); ++index) {
        double current = fabs(chrome->page_zoom - values[index]);
        if (current < distance) {
            closest = index;
            distance = current;
        }
    }
    chrome->page_zoom = values[(closest + 1) % G_N_ELEMENTS(values)];
    chrome_save_state(state);
    update_page_zoom_for_uri(state, webkit_web_view_get_uri(state->web_view));
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static void chrome_cycle_font_size(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    chrome->default_font_size = chrome->default_font_size <= 14 ? 16
        : chrome->default_font_size <= 16 ? 20 : 14;
    chrome_save_state(state);
    chrome_apply_web_preferences(state, TRUE);
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static void chrome_cycle_search_engine(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    if (!g_strcmp0(chrome->search_engine, "baidu"))
        g_strlcpy(chrome->search_engine, "bing", sizeof(chrome->search_engine));
    else if (!g_strcmp0(chrome->search_engine, "bing"))
        g_strlcpy(chrome->search_engine, "google", sizeof(chrome->search_engine));
    else if (!g_strcmp0(chrome->search_engine, "google")) {
        if (!browser_navigation_custom_search_template_valid(chrome->custom_search_template)) {
            keyboard_request(state, "custom_search", "",
                             "HTTPS 搜索模板，使用一个 %s", "EnUSPreferred", 512, FALSE);
            return;
        }
        g_strlcpy(chrome->search_engine, "custom", sizeof(chrome->search_engine));
    } else
        g_strlcpy(chrome->search_engine, "baidu", sizeof(chrome->search_engine));
    chrome_save_state(state);
    chrome_update_render_state(state);
    chrome_request_frame(state);
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

static gboolean chrome_write_gpu_mode(gboolean enabled)
{
    const char *path = g_getenv("WPE_GPU_MODE_FILE");
    char *fallback = NULL;
    if (!path || !path[0]) {
        const char *var_dir = g_getenv("WPE_VAR_DIR");
        fallback = g_build_filename(var_dir && var_dir[0] ? var_dir : "/tmp",
                                    "gpu-mode", NULL);
        path = fallback;
    }
    char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) && errno != EEXIST) {
        g_warning("GPU mode directory create failed: %s", directory);
        g_free(directory);
        g_free(fallback);
        return FALSE;
    }
    chmod(directory, 0700);
    g_free(directory);

    char *temporary = g_strdup_printf("%s.tmp.%ld", path, (long)getpid());
    int fd = g_open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const char *value = enabled ? "auto\n" : "off\n";
    gsize length = strlen(value);
    gsize written = 0;
    gboolean success = fd >= 0;
    while (success && written < length) {
        ssize_t count = write(fd, value + written, length - written);
        if (count > 0)
            written += count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            success = FALSE;
    }
    if (success)
        success = fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    if (success)
        success = g_rename(temporary, path) == 0;
    if (success)
        chmod(path, 0600);
    else {
        g_warning("GPU mode file write failed: %s errno=%d (%s)",
                  path, errno, strerror(errno));
        g_unlink(temporary);
    }
    g_free(temporary);
    g_free(fallback);
    return success;
}

static void chrome_request_gpu_restart(AppState *state, gboolean enabled)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    gboolean previous = chrome->global_settings.gpu_acceleration;
    chrome->global_settings.gpu_acceleration = enabled;
    if (!chrome_write_gpu_mode(enabled)) {
        chrome->global_settings.gpu_acceleration = previous;
        chrome_update_render_state(state);
        chrome_request_frame(state);
        return;
    }
    chrome_save_state(state);
    runtime_exit_code = CHROME_GPU_RESTART_CODE;
    g_print("GPU mode switch requested: enabled=%d mode=%s current=%s exit=%d\n",
            enabled, enabled ? "auto" : "off",
            gpu_render_profile ? "gpu" : "cpu", runtime_exit_code);
    if (main_loop)
        g_main_loop_quit(main_loop);
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
    chrome_open_internal_panel(state,
        state->chrome.dialog_parent == CHROME_PANEL_SETTINGS_PRIVACY
            ? CHROME_PANEL_SETTINGS_PRIVACY : CHROME_PANEL_PRIVACY,
        FALSE);
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
    chrome_open_internal_panel(state,
                               bookmarks ? CHROME_PANEL_BOOKMARKS
                                         : CHROME_PANEL_HISTORY,
                               TRUE);
    chrome->panel_page = 0;
    chrome->panel_delete_mode = FALSE;
    chrome_refresh_pages(chrome, bookmarks);
    chrome_update_render_state(state);
}

static void chrome_go_panel_back(AppState *state)
{
    BrowserChrome *chrome = &state->chrome;
    ChromePanel parent = (chrome->panel == CHROME_PANEL_CLEAR_SITE_DATA
            || chrome->panel == CHROME_PANEL_CLEAR_HISTORY)
        && chrome->dialog_parent != CHROME_PANEL_NONE
        ? chrome->dialog_parent : chrome_panel_parent(chrome->panel);
    if (parent == CHROME_PANEL_MENU) {
        chrome->panel_page = 0;
        chrome->panel_delete_mode = FALSE;
        chrome_open_menu_panel(state);
    } else if (chrome_panel_is_internal_list(parent))
        chrome_open_internal_panel(state, parent, FALSE);
    else
        chrome_close_panel(state);
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
            chrome_open_internal_panel(state, CHROME_PANEL_PROFILES, TRUE);
            chrome->panel_page = 0;
            chrome_refresh_profiles(chrome);
        } else if (row == 4) {
            chrome_open_internal_panel(state, CHROME_PANEL_PRIVACY, TRUE);
            chrome->site_data_status[0] = '\0';
        } else if (row == 5)
            chrome_open_internal_panel(state, CHROME_PANEL_SETTINGS, TRUE);
    } else if (chrome->panel == CHROME_PANEL_SETTINGS) {
        static const ChromePanel panels[] = {
            CHROME_PANEL_APPEARANCE,
            CHROME_PANEL_WEB,
            CHROME_PANEL_STARTUP,
            CHROME_PANEL_SETTINGS_PRIVACY,
            CHROME_PANEL_ABOUT,
        };
        if (row >= 0 && row < (int)G_N_ELEMENTS(panels))
            chrome_open_internal_panel(state, panels[row], TRUE);
    } else if (chrome->panel == CHROME_PANEL_APPEARANCE) {
        if (row == 0)
            chrome_toggle_theme(state);
        else if (row == 1) {
            chrome->global_settings.toolbar_auto_hide =
                !chrome->global_settings.toolbar_auto_hide;
            if (!chrome->global_settings.toolbar_auto_hide)
                chrome_set_visible(state, TRUE);
            chrome_save_state(state);
        } else if (row == 2)
            chrome_cycle_page_zoom(state);
        else if (row == 3)
            chrome_cycle_font_size(state);
        else if (row == 4)
            chrome_request_gpu_restart(
                state, !chrome->global_settings.gpu_acceleration);
    } else if (chrome->panel == CHROME_PANEL_WEB) {
        if (row == 0)
            chrome_toggle_site_profile(state);
        else if (row == 1) {
            chrome->javascript_enabled = !chrome->javascript_enabled;
            chrome_save_state(state);
            chrome_apply_web_preferences(state, TRUE);
        } else if (row == 2) {
            chrome->autoplay_requires_gesture = !chrome->autoplay_requires_gesture;
            chrome_save_state(state);
            chrome_apply_web_preferences(state, TRUE);
        } else if (row == 3) {
            chrome->smooth_scrolling = !chrome->smooth_scrolling;
            chrome_save_state(state);
            chrome_apply_web_preferences(state, TRUE);
        } else if (row == 4) {
            chrome->block_popups = !chrome->block_popups;
            chrome_save_state(state);
            chrome_apply_web_preferences(state, TRUE);
        }
    } else if (chrome->panel == CHROME_PANEL_STARTUP) {
        if (row == 0)
            chrome_cycle_search_engine(state);
        else if (row == 1)
            keyboard_request(state, "custom_search",
                             chrome->custom_search_template,
                             "HTTPS 搜索模板，使用一个 %s",
                             "EnUSPreferred", 512, FALSE);
        else if (row == 2)
            keyboard_request(state, "home_url",
                             chrome->home_url ? chrome->home_url : default_home_url(),
                             "设置主页 URL", "EnUSPreferred", 512, FALSE);
        else if (row == 3) {
            chrome->restore_tabs = !chrome->restore_tabs;
            chrome_save_state(state);
        }
    } else if (chrome->panel == CHROME_PANEL_SETTINGS_PRIVACY) {
        if (row == 0)
            chrome_cycle_cookie_policy(state);
        else if (row == 1 && !chrome->site_data_clearing) {
            chrome->dialog_parent = CHROME_PANEL_SETTINGS_PRIVACY;
            chrome_open_internal_panel(state, CHROME_PANEL_CLEAR_SITE_DATA, TRUE);
        } else if (row == 2) {
            chrome_clear_cache();
            g_strlcpy(chrome->site_data_status, "CACHE CLEARED",
                      sizeof(chrome->site_data_status));
            chrome_save_state(state);
        } else if (row == 3 && !profile_runtime.guest) {
            chrome->dialog_parent = CHROME_PANEL_SETTINGS_PRIVACY;
            chrome_open_internal_panel(state, CHROME_PANEL_CLEAR_HISTORY, TRUE);
        }
    } else if (chrome->panel == CHROME_PANEL_ABOUT) {
        g_print("About: Direct WPE DRM browser profile=%s guest=%d renderer=%s display=%dx%d rotation=%d\n",
                profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
                profile_runtime.guest, gpu_render_profile ? "mali-gpu" : "skia-cpu",
                state->panel_width, state->panel_height, state->panel_rotation);
    } else if (chrome->panel == CHROME_PANEL_PRIVACY) {
        if (row == 0)
            chrome_cycle_cookie_policy(state);
        else if (row == 1 && !chrome->site_data_clearing) {
            chrome->dialog_parent = CHROME_PANEL_PRIVACY;
            chrome_open_internal_panel(state, CHROME_PANEL_CLEAR_SITE_DATA, TRUE);
            chrome_update_render_state(state);
        }
    } else if (chrome->panel == CHROME_PANEL_CLEAR_SITE_DATA) {
        if (row == 0)
            chrome_open_internal_panel(state,
                chrome->dialog_parent == CHROME_PANEL_SETTINGS_PRIVACY
                    ? CHROME_PANEL_SETTINGS_PRIVACY : CHROME_PANEL_PRIVACY,
                FALSE);
        else if (row == 1)
            chrome_clear_site_data(state);
    } else if (chrome->panel == CHROME_PANEL_PROFILES) {
        guint offset = chrome->panel_page * 4;
        if (row >= 0 && row < 4 && chrome->panel_profiles
                && offset + row < chrome->panel_profiles->len) {
            BrowserProfile *profile = g_ptr_array_index(chrome->panel_profiles, offset + row);
            if (!profile_runtime.guest && profile->id == profile_runtime.profile.id) {
                chrome_open_internal_panel(state, CHROME_PANEL_PROFILE_MANAGE, TRUE);
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
            guint previous_page = chrome->panel_page;
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
                chrome_open_internal_panel(state, CHROME_PANEL_PROFILE_MANAGE, TRUE);
                chrome_update_render_state(state);
            }
            if (chrome->panel == CHROME_PANEL_PROFILES
                    && chrome->panel_page != previous_page) {
                chrome->panel_scroll_offsets[CHROME_PANEL_PROFILES] = 0;
                chrome_publish_motion(state, FALSE);
            }
        }
    } else if (chrome->panel == CHROME_PANEL_PROFILE_MANAGE) {
        if (row == 0 && !profile_runtime.guest)
            keyboard_request(state, "profile_rename", profile_runtime.profile.name,
                             "重命名 Profile", "ZhCNPreferred", 20, FALSE);
        else if (row == 1 && !profile_runtime.guest && !profile_runtime.profile.is_default) {
            chrome_open_internal_panel(state, CHROME_PANEL_PROFILE_DELETE, TRUE);
            chrome_update_render_state(state);
        }
    } else if (chrome->panel == CHROME_PANEL_PROFILE_DELETE) {
        if (row == 0) {
            chrome_open_internal_panel(state, CHROME_PANEL_PROFILE_MANAGE, FALSE);
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
                chrome_close_panel(state);
                chrome_load_url(state, url, TRUE);
                g_free(url);
            }
        } else if (row == 6) {
            if (x < panel_width / 2) {
                if (chrome->panel_page > 0)
                    chrome->panel_page--;
            } else if (chrome->panel_pages && chrome->panel_pages->len == 6)
                chrome->panel_page++;
            chrome->panel_scroll_offsets[chrome->panel] = 0;
            chrome_publish_motion(state, FALSE);
            chrome_refresh_pages(chrome, bookmarks);
        } else if (row == 7) {
            if (bookmarks || x < panel_width / 2)
                chrome->panel_delete_mode = !chrome->panel_delete_mode;
            else {
                chrome_open_internal_panel(state, CHROME_PANEL_CLEAR_HISTORY, TRUE);
                chrome_update_render_state(state);
            }
        }
    } else if (chrome->panel == CHROME_PANEL_CLEAR_HISTORY) {
        if (row == 0) {
            ChromePanel parent = chrome->dialog_parent == CHROME_PANEL_SETTINGS_PRIVACY
                ? CHROME_PANEL_SETTINGS_PRIVACY : CHROME_PANEL_HISTORY;
            chrome_open_internal_panel(state, parent, FALSE);
            if (parent == CHROME_PANEL_HISTORY)
                chrome_refresh_pages(chrome, FALSE);
        } else if (row == 1 && !profile_runtime.guest) {
            GError *error = NULL;
            if (!browser_profile_store_clear_visits(profile_runtime.store,
                    profile_runtime.profile.id, &error))
                g_warning("History clear failed: %s", error ? error->message : "unknown");
            g_clear_error(&error);
            ChromePanel parent = chrome->dialog_parent == CHROME_PANEL_SETTINGS_PRIVACY
                ? CHROME_PANEL_SETTINGS_PRIVACY : CHROME_PANEL_HISTORY;
            chrome_open_internal_panel(state, parent, FALSE);
            chrome->panel_page = 0;
            chrome->panel_scroll_offsets[parent] = 0;
            chrome_publish_motion(state, FALSE);
            if (parent == CHROME_PANEL_HISTORY)
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
        chrome_close_panel(state);
        chrome_save_state(state);
        chrome_request_frame(state);
        g_print("Chrome tabs: outside dismiss x=%.1f y=%.1f\n", x, y);
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
    chrome_close_panel(state);
    if (row == chrome->active) {
        chrome_save_state(state);
        chrome_request_frame(state);
    } else
        chrome_switch_tab(state, row);
    return TRUE;
}

static gboolean chrome_handle_menu_panel_tap(AppState *state, double x, double y)
{
    BrowserChrome *chrome = &state->chrome;
    if (chrome->panel != CHROME_PANEL_MENU)
        return FALSE;
    ChromeMenuGeometry geometry = chrome_menu_geometry(state);
    if (y < geometry.panel_y)
        return FALSE;

    if (x < geometry.panel_x || x >= geometry.panel_x + geometry.panel_width
            || y >= geometry.panel_y + geometry.panel_height) {
        chrome_close_panel(state);
        chrome_save_state(state);
        chrome_request_frame(state);
        g_print("Chrome menu: outside dismiss x=%.1f y=%.1f\n", x, y);
        return TRUE;
    }

    int dots_height = geometry.page_count > 1 ? 14 : 0;
    int grid_height = MAX(1, geometry.panel_height - dots_height);
    if (y >= geometry.panel_y + grid_height)
        return TRUE;
    double content_x = x - geometry.panel_x + chrome->menu_scroll_offset;
    int page = CLAMP((int)floor(content_x / geometry.panel_width),
                     0, geometry.page_count - 1);
    double page_x = content_x - page * geometry.panel_width;
    int column = CLAMP((int)floor(page_x * geometry.columns / geometry.panel_width),
                       0, geometry.columns - 1);
    int row = CLAMP((int)floor((y - geometry.panel_y) * geometry.rows / grid_height),
                    0, geometry.rows - 1);
    int item = page * CHROME_MENU_ITEMS_PER_PAGE + row * geometry.columns + column;
    if (item < 0 || item >= geometry.item_count)
        return TRUE;

    g_print("Chrome menu: select item=%d page=%d row=%d column=%d x=%.1f y=%.1f\n",
            item, page, row, column, x, y);
    chrome_select_panel_row(state, x, item);
    return TRUE;
}

static gboolean chrome_handle_list_panel_tap(AppState *state, double x, double y)
{
    BrowserChrome *chrome = &state->chrome;
    if (!chrome_panel_is_internal_list(chrome->panel))
        return FALSE;

    ChromeListGeometry geometry = chrome_list_geometry(state, chrome->panel);
    if (y < geometry.panel_y)
        return FALSE;
    if (x < geometry.panel_x || x >= geometry.panel_x + geometry.panel_width
            || y >= geometry.panel_y + geometry.panel_height)
        return TRUE;

    if (y < geometry.list_top) {
        if (x < geometry.panel_x + 72) {
            g_print("Chrome tap: panel back from=%s x=%.1f y=%.1f\n",
                    chrome_panel_name(chrome->panel), x, y);
            chrome_go_panel_back(state);
        }
        return TRUE;
    }

    double content_y = y - geometry.list_top
        + chrome->panel_scroll_offsets[chrome->panel];
    int row = (int)floor(content_y / geometry.row_height);
    if (row >= 0 && row < geometry.line_count) {
        g_print("Chrome tap: panel=%s row=%d x=%.1f y=%.1f offset=%.1f\n",
                chrome_panel_name(chrome->panel), row, x, y,
                chrome->panel_scroll_offsets[chrome->panel]);
        chrome_select_panel_row(state, x, row);
    }
    return TRUE;
}

static void chrome_request_user_exit(AppState *state)
{
    if (!state)
        return;
    chrome_save_state(state);
    chrome_update_render_state(state);
    runtime_exit_code = CHROME_USER_EXIT_CODE;
    g_print("Chrome user shutdown requested: exit_code=%d\n", runtime_exit_code);
    if (main_loop)
        g_main_loop_quit(main_loop);
}

static void chrome_handle_toolbar_tap(AppState *state, double x, double y)
{
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->enabled || state->page_fullscreen)
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
    if (chrome_handle_tabs_panel_tap(state, x, y))
        return;
    if (chrome_handle_menu_panel_tap(state, x, y))
        return;
    if (chrome_handle_list_panel_tap(state, x, y))
        return;

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
        chrome_close_panel(state);
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
            chrome_close_panel(state);
        else
            chrome_open_tabs_panel(state);
        g_print("Chrome tap: tabs panel=%s x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), x, y);
        chrome_save_state(state);
    } else if (index == 3) {
        if (chrome_panel_is_overflow_stack(chrome->panel)) {
            chrome_request_user_exit(state);
            return;
        } else if (chrome->panel == CHROME_PANEL_TABS) {
            if (chrome->tab_count >= MAX_BROWSER_TABS)
                g_print("Chrome tap: new tab disabled count=%d max=%d x=%.1f y=%.1f\n",
                        chrome->tab_count, MAX_BROWSER_TABS, x, y);
            else {
                g_print("Chrome tap: new tab count=%d x=%.1f y=%.1f\n",
                        chrome->tab_count, x, y);
                chrome_new_tab(state, chrome->home_url);
            }
        } else if (chrome->loading) {
            g_print("Chrome tap: stop x=%.1f y=%.1f\n", x, y);
            webkit_web_view_stop_loading(state->web_view);
        } else if (chrome_active_tab(chrome)) {
            g_print("Chrome tap: reload x=%.1f y=%.1f\n", x, y);
            chrome_load_url(state, chrome_active_tab(chrome)->url, FALSE);
        }
    } else if (index == 4) {
        if (chrome_panel_is_overflow_stack(chrome->panel))
            chrome_close_panel(state);
        else
            chrome_open_menu_panel(state);
        g_print("Chrome tap: menu panel=%s x=%.1f y=%.1f\n", chrome_panel_name(chrome->panel), x, y);
        chrome_save_state(state);
    }
    chrome_update_render_state(state);
    chrome_request_frame(state);
}

static gboolean chrome_touch_down_consumes(AppState *state, double x, double y)
{
    BrowserChrome *chrome = &state->chrome;
    if (!chrome->enabled || state->page_fullscreen)
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
    if (!chrome->enabled || state->page_fullscreen)
        return;
    if (!chrome->global_settings.toolbar_auto_hide) {
        if (!chrome->visible && chrome->panel == CHROME_PANEL_NONE) {
            chrome_set_visible(state, TRUE);
            chrome_request_frame(state);
        }
        return;
    }

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
    g_strlcpy(chrome->search_engine,
              profile_runtime.profile.search_engine
                ? profile_runtime.profile.search_engine : "baidu",
              sizeof(chrome->search_engine));
    chrome->custom_search_template = g_strdup(
        profile_runtime.profile.custom_search_template
            ? profile_runtime.profile.custom_search_template : "");
    chrome->page_zoom = CLAMP(profile_runtime.profile.page_zoom, 0.75, 1.25);
    chrome->default_font_size = CLAMP(profile_runtime.profile.default_font_size, 14, 20);
    chrome->javascript_enabled = profile_runtime.profile.javascript_enabled;
    chrome->autoplay_requires_gesture = profile_runtime.profile.autoplay_requires_gesture;
    chrome->smooth_scrolling = profile_runtime.profile.smooth_scrolling;
    chrome->block_popups = profile_runtime.profile.block_popups;
    chrome->restore_tabs = profile_runtime.profile.restore_tabs;
    browser_profile_store_get_global_settings(profile_runtime.store,
                                              &chrome->global_settings);
    chrome->tab_count = 1;
    chrome->active = 0;
    chrome->next_tab_id = 2;
    chrome->tabs[0].id = 1;
    chrome_set_tab_url(&chrome->tabs[0], initial_url && initial_url[0] ? initial_url : chrome->home_url);
    chrome->motion.version = WPE_CHROME_MOTION_VERSION;
    chrome->motion.sequence = 1;
    chrome->motion.panel = WPE_CHROME_MOTION_PANEL_NONE;
    chrome->motion.flags = 0;
    chrome->motion.offset = 0;
    chrome->pressed_row = -1;
    chrome->pressed_segment = -1;
    chrome->pressed_control = -1;
    chrome->motion.pressed_row = -1;
    chrome->motion.pressed_segment = -1;
    chrome->motion.pressed_control = -1;
    if (state->view)
        g_object_set_data(G_OBJECT(state->view), WPE_CHROME_MOTION_DATA_KEY,
                          &chrome->motion);
    g_print("Chrome config: enabled=%d height=%d hide_down=%.1f show_up=%.1f show_min_velocity=%.1f\n",
            chrome->enabled, chrome->height,
            chrome->hide_down_px, chrome->show_up_px, chrome->show_up_min_velocity_px_s);

    if (!profile_runtime.guest && profile_runtime.store && chrome->restore_tabs) {
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
    g_print("Chrome profile state: name=%s guest=%d home=%s site_profile=%s cookie_policy=%s "
            "search=%s zoom=%.2f font=%u js=%d autoplay_gesture=%d smooth=%d popups_blocked=%d "
            "restore_tabs=%d theme=%s toolbar_auto_hide=%d gpu_acceleration=%d gpu_status=%s tabs=%d active=%d\n",
            profile_runtime.profile.name ? profile_runtime.profile.name : "DEFAULT",
            profile_runtime.guest, chrome->home_url,
            chrome->site_profile, browser_cookie_policy_name(chrome->cookie_policy),
            chrome->search_engine, chrome->page_zoom, chrome->default_font_size,
            chrome->javascript_enabled, chrome->autoplay_requires_gesture,
            chrome->smooth_scrolling, chrome->block_popups, chrome->restore_tabs,
            chrome->global_settings.theme, chrome->global_settings.toolbar_auto_hide,
            chrome->global_settings.gpu_acceleration,
            gpu_runtime_status_label(&chrome->global_settings),
            chrome->tab_count, chrome->active);
    chrome_update_render_state(state);
}

static void chrome_destroy(AppState *state)
{
    if (!state)
        return;
    BrowserChrome *chrome = &state->chrome;
    chrome_cancel_menu_snap(chrome);
    if (state->view)
        g_object_set_data(G_OBJECT(state->view), WPE_CHROME_MOTION_DATA_KEY, NULL);
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
    g_free(chrome->custom_search_template);
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
        if (state->slots[i].game_hold_source_id)
            g_source_remove(state->slots[i].game_hold_source_id);
        memset(&state->slots[i], 0, sizeof(state->slots[i]));
        state->slots[i].tracking_id = -1;
        state->slots[i].raw_x = -1;
        state->slots[i].raw_y = -1;
        state->slots[i].game_state = state;
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

    slot->x = clamp_double(tx, 0, 1) * MAX(0, width - 1);
    slot->y = clamp_double(ty, 0, 1) * MAX(0, height - 1);
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

static gboolean transform_game_video_point(AppState *state, double *x, double *y)
{
    if (!state || !x || !y || !state->game_input_active || !state->view)
        return FALSE;

    const char *value = g_object_get_data(G_OBJECT(state->view), "wpe-video-overlay-input-geometry");
    if (!value || !value[0])
        return FALSE;

    int version = 0;
    char fit[16] = { 0 };
    int dom_x = 0;
    int dom_y = 0;
    int dom_w = 0;
    int dom_h = 0;
    int visible_x = 0;
    int visible_y = 0;
    int visible_w = 0;
    int visible_h = 0;
    if (sscanf(value, "%d,%15[^,],%d,%d,%d,%d,%d,%d,%d,%d",
            &version, fit, &dom_x, &dom_y, &dom_w, &dom_h,
            &visible_x, &visible_y, &visible_w, &visible_h) != 10
        || version != 1 || g_strcmp0(fit, "contain")
        || dom_w <= 0 || dom_h <= 0 || visible_w <= 0 || visible_h <= 0)
        return FALSE;

    double visible_max_x = visible_x + visible_w - 1;
    double visible_max_y = visible_y + visible_h - 1;
    if (*x < visible_x || *x > visible_max_x
        || *y < visible_y || *y > visible_max_y)
        return FALSE;

    double normalized_x = (*x - visible_x) / MAX(1.0, (double)visible_w - 1.0);
    double normalized_y = (*y - visible_y) / MAX(1.0, (double)visible_h - 1.0);
    normalized_x = clamp_double(normalized_x, 0, 1);
    normalized_y = clamp_double(normalized_y, 0, 1);
    *x = dom_x + normalized_x * MAX(0, dom_w - 1);
    *y = dom_y + normalized_y * MAX(0, dom_h - 1);
    return TRUE;
}

static void snapshot_game_video_mapping(AppState *state, TouchSlot *slot)
{
    slot->game_video_mapped = FALSE;
    if (!state || !slot || !state->game_input_active || !state->view)
        return;

    const char *value = g_object_get_data(G_OBJECT(state->view),
                                          "wpe-video-overlay-input-geometry");
    int version = 0;
    char fit[16] = { 0 };
    if (!value || sscanf(value, "%d,%15[^,],%d,%d,%d,%d,%d,%d,%d,%d",
            &version, fit,
            &slot->game_dom_x, &slot->game_dom_y,
            &slot->game_dom_width, &slot->game_dom_height,
            &slot->game_visible_x, &slot->game_visible_y,
            &slot->game_visible_width, &slot->game_visible_height) != 10
        || version != 1 || g_strcmp0(fit, "contain")
        || slot->game_dom_width <= 0 || slot->game_dom_height <= 0
        || slot->game_visible_width <= 0 || slot->game_visible_height <= 0)
        return;

    double visible_max_x = slot->game_visible_x + slot->game_visible_width - 1;
    double visible_max_y = slot->game_visible_y + slot->game_visible_height - 1;
    slot->game_video_mapped =
        slot->x >= slot->game_visible_x && slot->x <= visible_max_x
        && slot->y >= slot->game_visible_y && slot->y <= visible_max_y;
}

static void map_game_slot_point(AppState *state, const TouchSlot *slot,
                                double screen_x, double screen_y,
                                double *view_x, double *view_y,
                                gboolean *video_mapped)
{
    gboolean mapped = state && slot && state->game_input_active
        && slot->game_video_mapped;
    double x = screen_x;
    double y = screen_y;
    if (mapped) {
        double normalized_x = (screen_x - slot->game_visible_x)
            / MAX(1.0, (double)slot->game_visible_width - 1.0);
        double normalized_y = (screen_y - slot->game_visible_y)
            / MAX(1.0, (double)slot->game_visible_height - 1.0);
        normalized_x = clamp_double(normalized_x, 0, 1);
        normalized_y = clamp_double(normalized_y, 0, 1);
        x = slot->game_dom_x
            + normalized_x * MAX(0, slot->game_dom_width - 1);
        y = slot->game_dom_y
            + normalized_y * MAX(0, slot->game_dom_height - 1);
    } else
        y = web_event_y(state, screen_y);
    if (view_x)
        *view_x = x;
    if (view_y)
        *view_y = y;
    if (video_mapped)
        *video_mapped = mapped;
}

static gboolean uses_native_touch_events(AppState *state)
{
    if (!state)
        return FALSE;
    return (state->game_input_active || !state->touch_scroll_fallback)
        && (state->send_touch_events || state->game_input_active);
}

static void send_touch_event(AppState *state, WPEEventType type, guint32 sequence_id, double x, double y, guint32 time_ms)
{
    if (!state->send_touch_events && !state->game_input_active)
        return;

    double screen_x = x;
    double screen_y = y;
    gboolean video_mapped = transform_game_video_point(state, &x, &y);
    double view_y = video_mapped ? y : web_event_y(state, y);
    WPEEvent *event = wpe_event_touch_new(type, state->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time_ms, 0, sequence_id, x, view_y);
    wpe_view_event(state->view, event);
    wpe_event_unref(event);

    state->touch_event_count++;
    if (type == WPE_EVENT_TOUCH_DOWN || type == WPE_EVENT_TOUCH_UP
        || state->touch_event_count <= 8 || !(state->touch_event_count % 80))
        g_print("Touch event: type=%d seq=%u x=%.1f y=%.1f screen=%.1f,%.1f video_mapped=%d count=%" G_GUINT64_FORMAT "\n",
                type, sequence_id, x, view_y, screen_x, screen_y, video_mapped, state->touch_event_count);
}

static void send_game_touch_event(AppState *state, WPEEventType type,
                                  TouchSlot *slot, double screen_x,
                                  double screen_y, guint32 time_ms)
{
    if (!state || !slot)
        return;
    double x = 0;
    double y = 0;
    gboolean video_mapped = FALSE;
    map_game_slot_point(state, slot, screen_x, screen_y, &x, &y,
                        &video_mapped);
    WPEEvent *event = wpe_event_touch_new(type, state->view,
        WPE_INPUT_SOURCE_TOUCHSCREEN, time_ms, 0,
        slot->game_sequence_id, x, y);
    wpe_view_event(state->view, event);
    wpe_event_unref(event);
    state->touch_event_count++;
    if (type == WPE_EVENT_TOUCH_DOWN || type == WPE_EVENT_TOUCH_UP
        || state->touch_event_count <= 8 || !(state->touch_event_count % 80))
        g_print("Game touch: type=%d seq=%u x=%.1f y=%.1f screen=%.1f,%.1f frozen_video_map=%d count=%" G_GUINT64_FORMAT "\n",
                type, slot->game_sequence_id, x, y, screen_x, screen_y,
                video_mapped, state->touch_event_count);
}

static void dispatch_pointer_tap(AppState *state, double x, double view_y,
                                 double screen_x, double screen_y,
                                 gboolean video_mapped, guint32 time_ms)
{
    state->last_pointer_tap_us = g_get_monotonic_time();
    wpe_view_focus_in(state->view);

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

    g_print("Pointer tap: x=%.1f y=%.1f screen=%.1f,%.1f video_mapped=%d\n",
            x, view_y, screen_x, screen_y, video_mapped);
}

static void send_pointer_tap(AppState *state, double x, double y, guint32 time_ms)
{
    if (!state->synthesize_pointer_tap)
        return;

    double screen_x = x;
    double screen_y = y;
    gboolean video_mapped = transform_game_video_point(state, &x, &y);
    double view_y = video_mapped ? y : web_event_y(state, y);
    dispatch_pointer_tap(state, x, view_y, screen_x, screen_y,
                         video_mapped, time_ms);
}

static void send_game_pointer_tap(AppState *state, TouchSlot *slot,
                                  double screen_x, double screen_y,
                                  guint32 time_ms)
{
    if (!state->synthesize_pointer_tap)
        return;
    double x = 0;
    double y = 0;
    gboolean video_mapped = FALSE;
    map_game_slot_point(state, slot, screen_x, screen_y, &x, &y,
                        &video_mapped);
    dispatch_pointer_tap(state, x, y, screen_x, screen_y,
                         video_mapped, time_ms);
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

static void game_activate_touch_slot(AppState *state, TouchSlot *slot,
                                     const char *reason, guint32 time_ms)
{
    if (!state || !slot || slot->game_gesture != GAME_GESTURE_PENDING
        || slot->chrome_consumed)
        return;
    if (slot->game_hold_source_id) {
        g_source_remove(slot->game_hold_source_id);
        slot->game_hold_source_id = 0;
    }
    slot->game_gesture = GAME_GESTURE_TOUCH;
    send_game_touch_event(state, WPE_EVENT_TOUCH_DOWN, slot,
                          slot->down_x, slot->down_y,
                          slot->game_down_time_ms);
    g_print("Game gesture: gesture=%s seq=%u reason=%s frozen_video_map=%d down=%.1f,%.1f\n",
            !g_strcmp0(reason, "multitouch") ? "multitouch" : "drag-touch",
            slot->game_sequence_id, reason ? reason : "unknown",
            slot->game_video_mapped, slot->down_x, slot->down_y);
    (void)time_ms;
}

static gboolean game_hold_timeout_cb(gpointer user_data)
{
    TouchSlot *slot = (TouchSlot *)user_data;
    if (!slot)
        return G_SOURCE_REMOVE;
    slot->game_hold_source_id = 0;
    AppState *state = (AppState *)slot->game_state;
    if (state && state->game_input_active && slot->active
        && slot->game_gesture == GAME_GESTURE_PENDING)
        game_activate_touch_slot(state, slot, "hold",
                                 slot->game_down_time_ms
                                     + state->game_hold_delay_ms);
    return G_SOURCE_REMOVE;
}

static void game_flush_multitouch(AppState *state, guint32 time_ms)
{
    guint active_contacts = 0;
    for (int i = 0; i < MAX_TOUCH_SLOTS; ++i) {
        TouchSlot *slot = &state->slots[i];
        if (slot->active && !slot->chrome_consumed
            && slot->game_gesture != GAME_GESTURE_NONE)
            active_contacts++;
    }
    if (active_contacts < 2)
        return;
    for (int i = 0; i < MAX_TOUCH_SLOTS; ++i) {
        TouchSlot *slot = &state->slots[i];
        if (slot->active && !slot->chrome_consumed
            && slot->game_gesture == GAME_GESTURE_PENDING)
            game_activate_touch_slot(state, slot, "multitouch", time_ms);
    }
}

static void game_reset_touch_slot(TouchSlot *slot)
{
    if (slot->game_hold_source_id) {
        g_source_remove(slot->game_hold_source_id);
        slot->game_hold_source_id = 0;
    }
    slot->game_gesture = GAME_GESTURE_NONE;
    slot->game_video_mapped = FALSE;
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
        gboolean native_touch = uses_native_touch_events(state);

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
            slot->panel_gesture = CHROME_PANEL_GESTURE_NONE;
            slot->panel_start_offset = 0;
            slot->panel_velocity = 0;
            slot->panel_last_time_ms = time_ms;
            state->chrome.pressed_row = -1;
            state->chrome.pressed_segment = -1;
            state->chrome.pressed_control = -1;
            if (slot->chrome_consumed && state->chrome.panel == CHROME_PANEL_TABS) {
                ChromeTabsGeometry geometry = chrome_tabs_geometry(state);
                if (slot->x >= geometry.panel_x
                    && slot->x < geometry.panel_x + geometry.panel_width
                    && slot->y >= geometry.list_top
                    && slot->y < geometry.footer_top) {
                    slot->panel_gesture = CHROME_PANEL_GESTURE_TABS_VERTICAL;
                    slot->panel_start_offset = state->chrome.tabs_scroll_offset;
                    state->chrome.pressed_row = (int)floor(
                        (slot->y - geometry.list_top
                         + state->chrome.tabs_scroll_offset) / geometry.row_height);
                }
            } else if (slot->chrome_consumed && state->chrome.panel == CHROME_PANEL_MENU) {
                ChromeMenuGeometry geometry = chrome_menu_geometry(state);
                if (slot->x >= geometry.panel_x
                    && slot->x < geometry.panel_x + geometry.panel_width
                    && slot->y >= geometry.panel_y
                    && slot->y < geometry.panel_y + geometry.panel_height) {
                    slot->panel_gesture = CHROME_PANEL_GESTURE_MENU_HORIZONTAL;
                    slot->panel_start_offset = state->chrome.menu_scroll_offset;
                    int page_x = (int)floor(slot->x - geometry.panel_x
                        + state->chrome.menu_scroll_offset);
                    int page = page_x / MAX(1, geometry.panel_width);
                    int local_x = page_x % MAX(1, geometry.panel_width);
                    int column = CLAMP(local_x * geometry.columns
                        / MAX(1, geometry.panel_width), 0, geometry.columns - 1);
                    int row = CLAMP((int)(slot->y - geometry.panel_y)
                        * geometry.rows / MAX(1, geometry.panel_height),
                        0, geometry.rows - 1);
                    state->chrome.pressed_row = page
                        * CHROME_MENU_ITEMS_PER_PAGE + row * geometry.columns + column;
                    chrome_cancel_menu_snap(&state->chrome);
                }
            } else if (slot->chrome_consumed
                       && chrome_panel_is_internal_list(state->chrome.panel)) {
                ChromeListGeometry geometry =
                    chrome_list_geometry(state, state->chrome.panel);
                if (slot->x >= geometry.panel_x
                    && slot->x < geometry.panel_x + geometry.panel_width
                    && slot->y >= geometry.list_top
                    && slot->y < geometry.list_top + geometry.list_height) {
                    slot->panel_gesture = CHROME_PANEL_GESTURE_LIST_VERTICAL;
                    slot->panel_start_offset =
                        state->chrome.panel_scroll_offsets[state->chrome.panel];
                    state->chrome.pressed_row = (int)floor(
                        (slot->y - geometry.list_top
                         + state->chrome.panel_scroll_offsets[state->chrome.panel])
                        / geometry.row_height);
                }
            } else if (slot->chrome_consumed) {
                int panel_width = state->panel_width > 0 ? state->panel_width : 960;
                int button_x = chrome_toolbar_controls_x(panel_width);
                if (slot->x < chrome_address_right(panel_width))
                    state->chrome.pressed_control = -2;
                else if (slot->x >= button_x)
                    state->chrome.pressed_control = CLAMP(
                        (int)((slot->x - button_x)
                            / chrome_toolbar_legacy_button_width(panel_width)),
                        0, CHROME_TOOLBAR_BUTTON_COUNT - 1);
            }
            if (slot->chrome_consumed)
                chrome_publish_motion(state, FALSE);
            g_print("Touch down: raw=%d,%d mapped=%.1f,%.1f chrome=%d visible=%d panel=%s\n",
                    slot->raw_x, slot->raw_y, slot->x, slot->y, slot->chrome_consumed,
                    state->chrome.visible, chrome_panel_name(state->chrome.panel));
            if (!slot->chrome_consumed && state->game_input_active) {
                state->next_touch_sequence++;
                if (!state->next_touch_sequence)
                    state->next_touch_sequence++;
                slot->game_sequence_id = state->next_touch_sequence;
                slot->game_down_time_ms = time_ms;
                slot->game_gesture = GAME_GESTURE_PENDING;
                snapshot_game_video_mapping(state, slot);
                slot->game_hold_source_id = g_timeout_add(
                    MAX(1, state->game_hold_delay_ms),
                    game_hold_timeout_cb, slot);
                game_flush_multitouch(state, time_ms);
            } else if (!slot->chrome_consumed && native_touch)
                send_touch_event(state, WPE_EVENT_TOUCH_DOWN, i, slot->x, slot->y, time_ms);
        } else if (slot->just_up) {
            gboolean handled_game_gesture =
                !slot->chrome_consumed
                && slot->game_gesture != GAME_GESTURE_NONE;
            if (handled_game_gesture) {
                if (slot->game_hold_source_id) {
                    g_source_remove(slot->game_hold_source_id);
                    slot->game_hold_source_id = 0;
                }
                if (slot->game_gesture == GAME_GESTURE_TOUCH) {
                    send_game_touch_event(state, WPE_EVENT_TOUCH_UP, slot,
                                          slot->last_x, slot->last_y,
                                          time_ms);
                    g_print("Game gesture end: gesture=touch seq=%u move=%.1f frozen_video_map=%d\n",
                            slot->game_sequence_id, sqrt(slot->max_move_sq),
                            slot->game_video_mapped);
                } else {
                    send_game_pointer_tap(state, slot, slot->last_x,
                                          slot->last_y, time_ms);
                    g_print("Game gesture end: gesture=tap-pointer seq=%u duration=%u move=%.1f frozen_video_map=%d\n",
                            slot->game_sequence_id,
                            time_ms - slot->game_down_time_ms,
                            sqrt(slot->max_move_sq),
                            slot->game_video_mapped);
                }
                game_reset_touch_slot(slot);
            } else if (!slot->chrome_consumed && native_touch)
                send_touch_event(state, WPE_EVENT_TOUCH_UP, i, slot->last_x,
                                 slot->last_y, time_ms);
            double tap_limit = state->touch_tap_max_move > 0 ? state->touch_tap_max_move : 24;
            if (slot->chrome_consumed) {
                double chrome_tap_limit = env_double("WPE_CHROME_TAP_MAX_MOVE", 32);
                if (chrome_tap_limit > tap_limit)
                    tap_limit = chrome_tap_limit;
            }
            if (handled_game_gesture) {
                /* The game gesture has already emitted exactly one protocol. */
            } else if (slot->chrome_consumed && slot->panel_gesture != CHROME_PANEL_GESTURE_NONE
                    && slot->scrolling) {
                if (slot->panel_gesture == CHROME_PANEL_GESTURE_TABS_VERTICAL) {
                    chrome_clamp_tabs_scroll(state);
                    chrome_publish_motion(state, FALSE);
                    chrome_update_render_state(state);
                } else if (slot->panel_gesture == CHROME_PANEL_GESTURE_LIST_VERTICAL) {
                    chrome_clamp_list_scroll(state, state->chrome.panel);
                    chrome_publish_motion(state, FALSE);
                    chrome_update_render_state(state);
                } else {
                    double drag_distance = state->chrome.menu_scroll_offset
                        - slot->panel_start_offset;
                    chrome_snap_menu(state, slot->panel_start_offset,
                                     drag_distance, slot->panel_velocity);
                }
            } else if (!slot->scrolling && slot->max_move_sq <= tap_limit * tap_limit) {
                if (slot->chrome_consumed)
                    chrome_handle_toolbar_tap(state, slot->last_x, slot->last_y);
                else if (!native_touch)
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
            slot->panel_gesture = CHROME_PANEL_GESTURE_NONE;
            slot->panel_velocity = 0;
            slot->tracking_id = -1;
            state->chrome.pressed_row = -1;
            state->chrome.pressed_segment = -1;
            state->chrome.pressed_control = -1;
            chrome_publish_motion(state, FALSE);
        } else if (slot->active && (slot->x != slot->last_x || slot->y != slot->last_y)) {
            double from_down_x = slot->x - slot->down_x;
            double from_down_y = slot->y - slot->down_y;
            double move_sq = from_down_x * from_down_x + from_down_y * from_down_y;
            if (move_sq > slot->max_move_sq)
                slot->max_move_sq = move_sq;
            double tap_limit = state->touch_tap_max_move > 0 ? state->touch_tap_max_move : 24;
            if (!slot->chrome_consumed
                && slot->game_gesture != GAME_GESTURE_NONE) {
                double threshold = state->game_drag_threshold > 0
                    ? state->game_drag_threshold : 8;
                if (slot->game_gesture == GAME_GESTURE_PENDING
                    && slot->max_move_sq > threshold * threshold)
                    game_activate_touch_slot(state, slot, "movement", time_ms);
                if (slot->game_gesture == GAME_GESTURE_TOUCH)
                    send_game_touch_event(state, WPE_EVENT_TOUCH_MOVE, slot,
                                          slot->x, slot->y, time_ms);
            } else {
                if (!slot->chrome_consumed && native_touch)
                    send_touch_event(state, WPE_EVENT_TOUCH_MOVE, i,
                                     slot->x, slot->y, time_ms);
            }
            if (slot->chrome_consumed && slot->panel_gesture != CHROME_PANEL_GESTURE_NONE) {
                double panel_drag_threshold = env_double("WPE_CHROME_PANEL_DRAG_PX", 8);
                double delta_x = slot->x - slot->last_x;
                double delta_y = slot->y - slot->last_y;
                gboolean vertical_gesture =
                    slot->panel_gesture == CHROME_PANEL_GESTURE_TABS_VERTICAL
                    || slot->panel_gesture == CHROME_PANEL_GESTURE_LIST_VERTICAL;
                gboolean direction_matches = vertical_gesture
                    ? fabs(from_down_y) >= fabs(from_down_x)
                    : fabs(from_down_x) >= fabs(from_down_y);
                if (direction_matches
                    && (slot->scrolling
                        || slot->max_move_sq > panel_drag_threshold * panel_drag_threshold)) {
                    slot->scrolling = TRUE;
                    state->chrome.pressed_row = -1;
                    state->chrome.pressed_segment = -1;
                    state->chrome.pressed_control = -1;
                    guint32 elapsed_ms = time_ms - slot->panel_last_time_ms;
                    if (slot->panel_gesture == CHROME_PANEL_GESTURE_TABS_VERTICAL) {
                        state->chrome.tabs_scroll_offset -= delta_y;
                        chrome_clamp_tabs_scroll(state);
                    } else if (slot->panel_gesture
                               == CHROME_PANEL_GESTURE_LIST_VERTICAL) {
                        ChromePanel panel = state->chrome.panel;
                        state->chrome.panel_scroll_offsets[panel] -= delta_y;
                        chrome_clamp_list_scroll(state, panel);
                    } else {
                        state->chrome.menu_scroll_offset -= delta_x;
                        chrome_clamp_menu_scroll(state);
                        if (elapsed_ms)
                            slot->panel_velocity = -delta_x * 1000.0 / elapsed_ms;
                    }
                    chrome_publish_motion(state, TRUE);
                }
                slot->panel_last_time_ms = time_ms;
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
    /* 默认值以 run.sh 为唯一事实来源; 此处仅覆盖绕过 run.sh 的直接启动。
     * 正常交互必须走 WPE 原生 touch event，不能只依赖鼠标轻点和滚动回退。 */
    state->send_touch_events = env_enabled("WPE_SEND_TOUCH_EVENTS", TRUE);
    state->synthesize_pointer_tap = env_enabled("WPE_SYNTHESIZE_POINTER_TAP", TRUE);
    state->game_drag_threshold = env_double("WPE_GAME_GESTURE_DRAG_PX", 8);
    state->game_hold_delay_ms =
        (guint)MAX(1, env_double("WPE_GAME_GESTURE_HOLD_MS", 80));
    state->touch_scroll_fallback = env_enabled("WPE_TOUCH_SCROLL_FALLBACK", FALSE);
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
    g_print("Raw touch: device=%s fd=%d panel=%dx%d panel_rotation=%d touch_rotation=%d touch_offset=%d,%d x_code=%d range=%d..%d active_x=%s%d..%d y_code=%d range=%d..%d active_y=%s%d..%d swap=%d inv_x=%d inv_y=%d send_touch=%d pointer_tap=%d game_drag=%.1f game_hold=%ums scroll_fallback=%d native_scroll=%d js_scroll=%d hscroll=%d scroll_invert_y=%d scroll_scale=%.2f max_step=%.1f pending_limit=%.1f tap_max=%.1f interval=%dms stop_delay=%dms\n",
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
            state->game_drag_threshold, state->game_hold_delay_ms,
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
        profile_runtime.profile.search_engine = g_strdup("baidu");
        profile_runtime.profile.custom_search_template = g_strdup("");
        profile_runtime.profile.page_zoom = 1.0;
        profile_runtime.profile.default_font_size = 16;
        profile_runtime.profile.javascript_enabled = TRUE;
        profile_runtime.profile.autoplay_requires_gesture = FALSE;
        profile_runtime.profile.smooth_scrolling = FALSE;
        profile_runtime.profile.block_popups = TRUE;
        profile_runtime.profile.restore_tabs = TRUE;
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

static void update_page_zoom_for_uri(AppState *state, const char *uri)
{
    if (!state || !state->web_view)
        return;

    double zoom = state->chrome.page_zoom > 0
        ? state->chrome.page_zoom : 1.0;
    if (uri_uses_game_input(uri)) {
        int height = state->panel_height > 0 ? state->panel_height : state->viewport_height;
        double default_zoom = height > 0 && height <= 300 ? 0.5 : 1.0;
        zoom = env_double("WPE_CLOUD_GAME_ZOOM", default_zoom);
        zoom = clamp_double(zoom, 0.25, 2.0);
    }

    double current_zoom = webkit_web_view_get_zoom_level(state->web_view);
    if (fabs(current_zoom - zoom) < 0.001)
        return;

    webkit_web_view_set_zoom_level(state->web_view, zoom);
    g_print("Page zoom: %.2f -> %.2f uri=%s\n",
            current_zoom, zoom, uri ? uri : "(null)");
}

static void set_game_input_active(AppState *state, gboolean game_active,
                                  const char *reason, const char *uri)
{
    if (!state)
        return;
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
    if (!game_active) {
        guint32 time_ms = (guint32)(g_get_monotonic_time() / 1000);
        for (int i = 0; i < MAX_TOUCH_SLOTS; ++i) {
            TouchSlot *slot = &state->slots[i];
            if (slot->game_gesture == GAME_GESTURE_TOUCH && slot->active)
                send_game_touch_event(state, WPE_EVENT_TOUCH_UP, slot,
                                      slot->last_x, slot->last_y, time_ms);
            game_reset_touch_slot(slot);
        }
    }
    state->game_input_active = game_active;

    if (env_enabled("WPE_GAME_MEDIA_IMMERSIVE", TRUE)) {
        BrowserChrome *chrome = &state->chrome;
        if (game_active && !state->game_media_immersive) {
            state->game_media_immersive = TRUE;
            state->chrome_visible_before_game_media = chrome->visible;
            chrome->panel = CHROME_PANEL_NONE;
            chrome_set_visible(state, FALSE);
            chrome_update_render_state(state);
            chrome_apply_layout(state, "game_media_enter");
            chrome_request_frame(state);
            g_print("Game media immersive entered: chrome_hidden=1 previous_visible=%d\n",
                    state->chrome_visible_before_game_media);
        } else if (!game_active && state->game_media_immersive) {
            state->game_media_immersive = FALSE;
            if (!state->page_fullscreen)
                chrome_set_visible(state, state->chrome_visible_before_game_media);
            chrome_update_render_state(state);
            chrome_apply_layout(state, "game_media_leave");
            chrome_request_frame(state);
            g_print("Game media immersive left: chrome_visible=%d page_fullscreen=%d\n",
                    chrome->visible, state->page_fullscreen);
        }
    }

    g_print("Input profile: configured=%s active=%s reason=%s uri=%s\n",
            normalize_input_profile(state->input_profile),
            game_active ? "game" : "browser",
            reason ? reason : "unknown",
            uri ? uri : "(null)");
}

static void update_input_profile_for_uri(AppState *state, const char *uri)
{
    if (!state)
        return;
    const char *profile = normalize_input_profile(state->input_profile);
    /*
     * Auto mode must preserve browser click semantics on cloud-game landing,
     * reward and queue pages. The page-side media observer switches to direct
     * touch only after a real video begins playback.
     */
    set_game_input_active(state, !g_strcmp0(profile, "game"),
                          "navigation", uri);
}

static gboolean on_game_input_script_message_with_reply(
    WebKitUserContentManager *manager, JSCValue *value,
    WebKitScriptMessageReply *reply, gpointer user_data)
{
    (void)manager;
    AppState *state = (AppState *)user_data;
    if (!state || !value || !reply)
        return FALSE;

    char *message = jsc_value_to_string(value);
    GHashTable *params = query_parse_params(message);
    gboolean active = params_get_bool(params, "active", FALSE);
    const char *reason = params_get_string(params, "reason",
                                            active ? "video_playing" : "video_stopped");
    const char *uri = state->web_view ? webkit_web_view_get_uri(state->web_view) : NULL;
    const char *profile = normalize_input_profile(state->input_profile);

    if (!g_strcmp0(profile, "auto") && uri_uses_game_input(uri)) {
        if (state->web_view) {
            if (active) {
                double current_zoom = webkit_web_view_get_zoom_level(state->web_view);
                if (fabs(current_zoom - 1.0) >= 0.001) {
                    webkit_web_view_set_zoom_level(state->web_view, 1.0);
                    g_print("Page zoom: %.2f -> 1.00 reason=game_media uri=%s\n",
                            current_zoom, uri ? uri : "(null)");
                }
            } else
                update_page_zoom_for_uri(state, uri);
        }
        set_game_input_active(state, active, reason, uri);
    } else
        g_print("Input profile media signal ignored: configured=%s active=%d reason=%s uri=%s\n",
                profile, active, reason, uri ? uri : "(null)");

    keyboard_reply_message(value, reply, "op=ok");
    g_hash_table_unref(params);
    g_free(message);
    return TRUE;
}

static gboolean on_cloud_launch_script_message_with_reply(
    WebKitUserContentManager *manager, JSCValue *value,
    WebKitScriptMessageReply *reply, gpointer user_data)
{
    (void)manager;
    AppState *state = (AppState *)user_data;
    if (!state || !value || !reply)
        return FALSE;

    char *message = jsc_value_to_string(value);
    GHashTable *params = query_parse_params(message);
    const char *op = params_get_string(params, "op", "");
    const char *x_text = params_get_string(params, "x", "");
    const char *y_text = params_get_string(params, "y", "");
    const char *top_text = params_get_string(params, "top", "0");
    const char *host = params_get_string(params, "host", "");
    const char *tag = params_get_string(params, "tag", "");
    const char *trusted = params_get_string(params, "trusted", "");
    char *x_end = NULL;
    char *y_end = NULL;
    double css_x = g_ascii_strtod(x_text, &x_end);
    double css_y = g_ascii_strtod(y_text, &y_end);
    const char *uri = state->web_view ? webkit_web_view_get_uri(state->web_view) : NULL;

    if (!g_strcmp0(op, "probe")) {
        g_print("Cloud launch probe: host=%s top=%s\n", host, top_text);
    } else if (!g_strcmp0(op, "capabilities")) {
        const char *peer_connection = params_get_string(params, "pc", "0");
        const char *data_channel = params_get_string(params, "dc", "0");
        const char *create_data_channel = params_get_string(params, "create", "0");
        const char *sctp = params_get_string(params, "sctp", "0");
        g_print("Cloud WebRTC capabilities: host=%s top=%s pc=%s dc=%s create_data_channel=%s sctp=%s\n",
                host, top_text, peer_connection, data_channel, create_data_channel, sctp);
    } else if (!g_strcmp0(op, "sdp-application")) {
        const char *phase = params_get_string(params, "phase", "(none)");
        const char *max_message_size = params_get_string(params, "max", "(none)");
        const char *sctp_map = params_get_string(params, "sctpmap", "0");
        const char *sctp_port = params_get_string(params, "sctpport", "(none)");
        g_print("Cloud JS SDP application: phase=%s host=%s top=%s sctp_port=%s max_message_size=%s sctpmap=%s\n",
                phase, host, top_text, sctp_port, max_message_size, sctp_map);
    } else if (!g_strcmp0(op, "sdp-media")) {
        const char *phase = params_get_string(params, "phase", "(none)");
        const char *index = params_get_string(params, "index", "(none)");
        const char *media = params_get_string(params, "media", "(none)");
        const char *proto = params_get_string(params, "proto", "(none)");
        const char *mid = params_get_string(params, "mid", "(none)");
        const char *direction = params_get_string(params, "direction", "(none)");
        const char *payloads = params_get_string(params, "payloads", "(none)");
        const char *extmaps = params_get_string(params, "extmaps", "0");
        const char *mixed = params_get_string(params, "mixed", "0");
        const char *rtcp_mux = params_get_string(params, "rtcp_mux", "0");
        const char *rtcp_rsize = params_get_string(params, "rtcp_rsize", "0");
        g_print("Cloud JS SDP media: phase=%s index=%s media=%s proto=%s mid=%s direction=%s payloads=%s extmaps=%s mixed=%s rtcp_mux=%s rtcp_rsize=%s\n",
                phase, index, media, proto, mid, direction, payloads,
                extmaps, mixed, rtcp_mux, rtcp_rsize);
    } else if (!g_strcmp0(op, "sdp-codec")) {
        const char *phase = params_get_string(params, "phase", "(none)");
        const char *index = params_get_string(params, "index", "(none)");
        const char *payload = params_get_string(params, "pt", "(none)");
        const char *rtpmap = params_get_string(params, "rtpmap", "(none)");
        const char *fmtp = params_get_string(params, "fmtp", "(none)");
        const char *feedback = params_get_string(params, "fb", "(none)");
        g_print("Cloud JS SDP codec: phase=%s index=%s pt=%s rtpmap=%s fmtp=%s rtcp_fb=%s\n",
                phase, index, payload, rtpmap, fmtp, feedback);
    } else if (!g_strcmp0(op, "rtc-state")) {
        const char *reason = params_get_string(params, "reason", "(none)");
        const char *connection = params_get_string(params, "connection", "(none)");
        const char *ice = params_get_string(params, "ice", "(none)");
        const char *signaling = params_get_string(params, "signaling", "(none)");
        const char *gathering = params_get_string(params, "gathering", "(none)");
        const char *dtls = params_get_string(params, "dtls", "(none)");
        g_print("Cloud JS RTC state: reason=%s connection=%s ice=%s signaling=%s gathering=%s dtls=%s\n",
                reason, connection, ice, signaling, gathering, dtls);
    } else if (!g_strcmp0(op, "click")) {
        /* Keep the trace structural: page text, URLs and event payloads may be private. */
        g_print("Cloud page click: host=%s top=%s tag=%s trusted=%s\n",
                host, top_text, tag, trusted);
    } else if (g_strcmp0(op, "target") || x_end == x_text || y_end == y_text || !uri_uses_game_input(uri)) {
        g_print("Cloud launch target ignored: op=%s uri=%s\n", op, uri ? uri : "(null)");
    } else if (g_strcmp0(top_text, "1")) {
        g_print("Cloud launch target observed in child frame: host=%s css=%.1f,%.1f\n",
            host, css_x, css_y);
    } else if (state->cloud_launch_requested) {
        g_print("Cloud launch target ignored: already requested css=%.1f,%.1f\n", css_x, css_y);
    } else if (!state->cloud_autostart) {
        g_print("Cloud launch target observed: css=%.1f,%.1f auto=off\n", css_x, css_y);
    } else {
        double zoom = webkit_web_view_get_zoom_level(state->web_view);
        double screen_x = css_x * zoom;
        double screen_y = css_y * zoom + chrome_page_top_inset(state);
        if (screen_x < 0 || screen_x >= state->viewport_width || screen_y < 0 || screen_y >= state->viewport_height) {
            g_warning("Cloud launch target rejected: css=%.1f,%.1f zoom=%.2f screen=%.1f,%.1f viewport=%dx%d",
                css_x, css_y, zoom, screen_x, screen_y, state->viewport_width, state->viewport_height);
        } else {
            state->cloud_launch_requested = TRUE;
            g_print("Cloud launch native tap: css=%.1f,%.1f zoom=%.2f screen=%.1f,%.1f\n",
                css_x, css_y, zoom, screen_x, screen_y);
            send_pointer_tap(state, screen_x, screen_y,
                (guint32)(g_get_monotonic_time() / 1000));
        }
    }

    keyboard_reply_message(value, reply, "op=ok");
    g_hash_table_unref(params);
    g_free(message);
    return TRUE;
}

static void setup_cloud_autostart_user_script(WebKitUserContentManager *manager,
                                               AppState *state)
{
    if (!manager || !state)
        return;

    g_signal_connect(manager, "script-message-with-reply-received::haasCloudLaunch",
                     G_CALLBACK(on_cloud_launch_script_message_with_reply), state);
    if (!webkit_user_content_manager_register_script_message_handler_with_reply(
            manager, "haasCloudLaunch", NULL))
        g_warning("Cloud launch script message handler already registered");

    const char *source =
        "(function(){"
        "if(window.__haasCloudLaunchInstalled)return;"
        "window.__haasCloudLaunchInstalled=true;"
        "if(!/(?:^|\\.)(?:mihoyo\\.com|mihoyocg\\.com)$/.test(location.hostname))return;"
        "var sent=false,attempts=0;"
        "function normalized(s){return (s||'').replace(/\\s+/g,'').trim();}"
        "function post(s){try{Promise.resolve(window.webkit.messageHandlers.haasCloudLaunch.postMessage(s)).catch(function(){});}catch(e){}}"
        "post('op=probe&host='+encodeURIComponent(location.hostname)+'&top='+(window.top===window?'1':'0'));"
        "try{"
        " var pc=typeof window.RTCPeerConnection==='function';"
        " var dc=typeof window.RTCDataChannel!=='undefined';"
        " var create=pc&&typeof window.RTCPeerConnection.prototype.createDataChannel==='function';"
        " var sctp=pc&&'sctp' in window.RTCPeerConnection.prototype;"
        " post('op=capabilities&pc='+(pc?'1':'0')+'&dc='+(dc?'1':'0')"
        "  +'&create='+(create?'1':'0')+'&sctp='+(sctp?'1':'0')"
        "  +'&host='+encodeURIComponent(location.hostname)+'&top='+(window.top===window?'1':'0'));"
        " function reportSdp(phase,desc){try{"
        "  var text=desc&&typeof desc.sdp==='string'?desc.sdp:'';"
        "  var sections=text.split(/(?=^m=)/m);"
        "  sections.forEach(function(section,index){"
        "   if(!/^m=/m.test(section))return;"
        "   var lines=section.split(/\\r?\\n/).filter(Boolean);"
        "   var m=(lines[0]||'').trim().split(/\\s+/);"
        "   var media=m[0]&&m[0].slice(2)||'none',proto=m[2]||'none';"
        "   var payloads=m.slice(3),mid='none',direction='sendrecv',extmaps=0;"
        "   var mixed=false,rtcpMux=false,rtcpRsize=false;"
        "   lines.forEach(function(line){"
        "    var match;"
        "    if((match=line.match(/^a=mid:(.+)$/)))mid=match[1];"
        "    else if(/^a=(sendonly|recvonly|inactive|sendrecv)$/.test(line))direction=line.slice(2);"
        "    else if(/^a=extmap:/.test(line))extmaps++;"
        "    else if(line==='a=extmap-allow-mixed')mixed=true;"
        "    else if(line==='a=rtcp-mux')rtcpMux=true;"
        "    else if(line==='a=rtcp-rsize')rtcpRsize=true;"
        "   });"
        "   post('op=sdp-media&phase='+encodeURIComponent(phase)"
        "    +'&index='+index+'&media='+encodeURIComponent(media)"
        "    +'&proto='+encodeURIComponent(proto)+'&mid='+encodeURIComponent(mid)"
        "    +'&direction='+encodeURIComponent(direction)"
        "    +'&payloads='+encodeURIComponent(payloads.join(','))"
        "    +'&extmaps='+extmaps+'&mixed='+(mixed?'1':'0')"
        "    +'&rtcp_mux='+(rtcpMux?'1':'0')+'&rtcp_rsize='+(rtcpRsize?'1':'0'));"
        "   payloads.forEach(function(pt){"
        "    var escaped=String(pt).replace(/[.*+?^${}()|[\\]\\\\]/g,'\\\\$&');"
        "    var rtp=(section.match(new RegExp('(?:^|\\\\r?\\\\n)a=rtpmap:'+escaped+'[ \\\\t]+([^\\\\r\\\\n]+)','m'))||[])[1]||'none';"
        "    var fmtp=(section.match(new RegExp('(?:^|\\\\r?\\\\n)a=fmtp:'+escaped+'[ \\\\t]+([^\\\\r\\\\n]+)','m'))||[])[1]||'none';"
        "    var fb=[];"
        "    section.replace(new RegExp('(?:^|\\\\r?\\\\n)a=rtcp-fb:'+escaped+'[ \\\\t]+([^\\\\r\\\\n]+)','gm'),function(_,value){fb.push(value);return _;});"
        "    post('op=sdp-codec&phase='+encodeURIComponent(phase)"
        "     +'&index='+index+'&pt='+encodeURIComponent(pt)"
        "     +'&rtpmap='+encodeURIComponent(rtp)+'&fmtp='+encodeURIComponent(fmtp)"
        "     +'&fb='+encodeURIComponent(fb.join(',')));"
        "   });"
        "  });"
        "  var app=(text.match(/(?:^|\\r?\\n)m=application[^]*?(?=\\r?\\nm=|$)/)||[''])[0];"
        "  if(!app)return;"
        "  var port=(app.match(/(?:^|\\r?\\n)a=sctp-port:(\\d+)/)||[])[1]||'none';"
        "  var max=(app.match(/(?:^|\\r?\\n)a=max-message-size:(\\d+)/)||[])[1]||'none';"
        "  var legacy=/(?:^|\\r?\\n)a=sctpmap:/m.test(app);"
        "  post('op=sdp-application&phase='+encodeURIComponent(phase)"
        "   +'&sctpport='+encodeURIComponent(port)+'&max='+encodeURIComponent(max)"
        "   +'&sctpmap='+(legacy?'1':'0')+'&host='+encodeURIComponent(location.hostname)"
        "   +'&top='+(window.top===window?'1':'0'));"
        " }catch(e){}}"
        " function observePc(peer){try{"
        "  if(!peer||peer.__haasRtcStateObserved)return;"
        "  peer.__haasRtcStateObserved=true;"
        "  var last='';"
        "  function reportState(reason){try{"
        "   var dtls=peer.sctp&&peer.sctp.transport?peer.sctp.transport.state:'none';"
        "   var snapshot=[peer.connectionState,peer.iceConnectionState,peer.signalingState,"
        "    peer.iceGatheringState,dtls].join('|');"
        "   if(reason==='poll'&&snapshot===last)return; last=snapshot;"
        "   post('op=rtc-state&reason='+encodeURIComponent(reason)"
        "    +'&connection='+encodeURIComponent(peer.connectionState||'none')"
        "    +'&ice='+encodeURIComponent(peer.iceConnectionState||'none')"
        "    +'&signaling='+encodeURIComponent(peer.signalingState||'none')"
        "    +'&gathering='+encodeURIComponent(peer.iceGatheringState||'none')"
        "    +'&dtls='+encodeURIComponent(dtls));"
        "  }catch(e){}}"
        "  ['connectionstatechange','iceconnectionstatechange','signalingstatechange',"
        "   'icegatheringstatechange'].forEach(function(name){"
        "    peer.addEventListener(name,function(){reportState(name);});"
        "  });"
        "  var ticks=0,timer=setInterval(function(){"
        "   reportState('poll'); if(++ticks>=120)clearInterval(timer);"
        "  },250);"
        "  reportState('attach');"
        " }catch(e){}}"
        " if(pc){"
        "  var proto=window.RTCPeerConnection.prototype;"
        "  if(!proto.__haasSdpWrapped){"
        "   proto.__haasSdpWrapped=true;"
        "   var setRemote=proto.setRemoteDescription;"
        "   proto.setRemoteDescription=function(desc){observePc(this);reportSdp('remote',desc);return setRemote.apply(this,arguments);};"
        "   var setLocal=proto.setLocalDescription;"
        "   proto.setLocalDescription=function(desc){observePc(this);reportSdp('local',desc);return setLocal.apply(this,arguments);};"
        "  }"
        " }"
        "}catch(e){}"
        "document.addEventListener('click',function(e){"
        " var t=e.target||{},tag=String(t.tagName||'').slice(0,24);"
        " post('op=click&tag='+encodeURIComponent(tag)+'&trusted='+(e.isTrusted?'1':'0')"
        "  +'&host='+encodeURIComponent(location.hostname)+'&top='+(window.top===window?'1':'0'));"
        "},true);"
        "function report(el,r){"
        " if(sent)return; sent=true;"
        " var payload='op=target&x='+encodeURIComponent(String(r.left+r.width/2))"
        "+'&y='+encodeURIComponent(String(r.top+r.height/2))"
        "+'&tag='+encodeURIComponent(el.tagName||'')"
        "+'&host='+encodeURIComponent(location.hostname)"
        "+'&top='+(window.top===window?'1':'0');"
        " post(payload);"
        "}"
        "function find(){"
        " attempts++; var nodes=document.querySelectorAll('button,[role=button],[role=link],a,div');"
        " for(var i=0;i<nodes.length&&i<1200;i++){var el=nodes[i];"
        "  if(normalized(el.innerText||el.textContent)!=='进入游戏')continue;"
        "  if(el.disabled||el.getAttribute('aria-disabled')==='true')continue;"
        "  var r=el.getBoundingClientRect();"
        "  if(r.width>=48&&r.height>=16&&r.right>0&&r.bottom>0&&r.left<innerWidth&&r.top<innerHeight){report(el,r);return;}"
        " }"
        " if(attempts>=80)clearInterval(window.__haasCloudLaunchTimer);"
        "}"
        "window.__haasCloudLaunchTimer=setInterval(find,250);find();"
        "})();";
    WebKitUserScript *script = webkit_user_script_new(source,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        NULL,
        NULL);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
    g_print("Cloud launch observer installed: auto=%d\n", state->cloud_autostart);
}

static void setup_game_input_user_script(WebKitUserContentManager *manager,
                                         AppState *state)
{
    if (!manager || !state)
        return;

    g_signal_connect(manager, "script-message-with-reply-received::haasGameInput",
                     G_CALLBACK(on_game_input_script_message_with_reply), state);
    if (!webkit_user_content_manager_register_script_message_handler_with_reply(
            manager, "haasGameInput", NULL))
        g_warning("Game input script message handler already registered");

    const char *source =
        "(function(){"
        "if(window.__haasGameInputInstalled)return;"
        "window.__haasGameInputInstalled=true;"
        "window.__haasGameInputActive=false;"
        "window.__haasGameInputTimer=0;"
        "function post(active,reason){"
        " if(window.__haasGameInputActive===active)return;"
        " window.__haasGameInputActive=active;"
        " try{Promise.resolve(window.webkit.messageHandlers.haasGameInput.postMessage("
        "  'active='+(active?'1':'0')+'&reason='+encodeURIComponent(reason)"
        " )).catch(function(){});}catch(e){}"
        "}"
        "function playing(){"
        " var list=document.getElementsByTagName('video');"
        " for(var i=0;i<list.length;i++){var v=list[i];"
        "  if(!v.paused&&!v.ended&&v.readyState>=2&&v.videoWidth>0&&v.videoHeight>0)return true;"
        " }"
        " return false;"
        "}"
        "function scan(reason){"
        " clearTimeout(window.__haasGameInputTimer);"
        " if(playing()){post(true,reason||'video_playing');return;}"
        " if(window.__haasGameInputActive)window.__haasGameInputTimer=setTimeout(function(){"
        "  if(!playing())post(false,'video_stopped');"
        " },1500);"
        "}"
        "document.addEventListener('play',function(e){"
        " if(e.target&&e.target.tagName==='VIDEO')scan('video_playing');"
        "},true);"
        "document.addEventListener('playing',function(e){"
        " if(e.target&&e.target.tagName==='VIDEO')scan('video_playing');"
        "},true);"
        "document.addEventListener('pause',function(e){"
        " if(e.target&&e.target.tagName==='VIDEO')scan('video_paused');"
        "},true);"
        "document.addEventListener('ended',function(e){"
        " if(e.target&&e.target.tagName==='VIDEO')scan('video_ended');"
        "},true);"
        "setInterval(function(){scan('video_poll');},1000);"
        "})();";
    WebKitUserScript *script = webkit_user_script_new(source,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        NULL,
        NULL);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
    g_print("Game input media observer installed\n");
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
    if (load_event == WEBKIT_LOAD_STARTED || load_event == WEBKIT_LOAD_REDIRECTED || load_event == WEBKIT_LOAD_COMMITTED) {
        update_input_profile_for_uri(state, uri);
        update_page_zoom_for_uri(state, uri);
    }
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
        char *scheme = browser_navigation_uri_scheme(uri);
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

static gboolean on_enter_fullscreen(WebKitWebView *web_view, gpointer user_data)
{
    (void)web_view;
    AppState *state = user_data;
    if (!state || state->page_fullscreen)
        return FALSE;

    BrowserChrome *chrome = &state->chrome;
    state->page_fullscreen = TRUE;
    state->chrome_visible_before_fullscreen = chrome->visible;
    chrome->panel = CHROME_PANEL_NONE;
    chrome->visible = FALSE;
    chrome->transition_us = 0;
    chrome->scroll_accum = 0;
    chrome->scroll_velocity_peak = 0;
    chrome->scroll_direction = 0;
    chrome_update_render_state(state);
    chrome_apply_layout(state, "page_fullscreen_enter");
    chrome_request_frame(state);
    g_print("Page fullscreen entered: chrome_hidden=1 previous_visible=%d\n",
            state->chrome_visible_before_fullscreen);
    return FALSE;
}

static gboolean on_leave_fullscreen(WebKitWebView *web_view, gpointer user_data)
{
    (void)web_view;
    AppState *state = user_data;
    if (!state || !state->page_fullscreen)
        return FALSE;

    state->page_fullscreen = FALSE;
    state->chrome.panel = CHROME_PANEL_NONE;
    if (state->game_media_immersive) {
        state->game_media_immersive = FALSE;
        state->chrome_visible_before_game_media = TRUE;
        chrome_set_visible(state, TRUE);
    } else
        chrome_set_visible(state, state->chrome_visible_before_fullscreen);
    chrome_update_render_state(state);
    chrome_apply_layout(state, "page_fullscreen_leave");
    chrome_request_frame(state);
    g_print("Page fullscreen left: chrome_visible=%d\n", state->chrome.visible);
    return FALSE;
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
    g_print("View state: size=%dx%d top=%dx%d visible=%d mapped=%d focus=%d fullscreen=%d frame=%" G_GUINT64_FORMAT " touch=%" G_GUINT64_FORMAT " scroll=%" G_GUINT64_FORMAT " title=%s uri=%s\n",
            wpe_view_get_width(view),
            wpe_view_get_height(view),
            top_width, top_height,
            wpe_view_get_visible(view),
            wpe_view_get_mapped(view),
            wpe_view_get_has_focus(view),
            state->page_fullscreen,
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
    char *scheme = browser_navigation_uri_scheme(uri);
    if (!scheme)
        return FALSE;

    if (browser_navigation_scheme_allowed(scheme)) {
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
        char *web_url = browser_navigation_bilibili_web_url(uri);
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
    /*
     * MiniApp 自身在部分 1GB SKU 上固定占用约 340MB，其中约 185MB 是 card1
     * framebuffer 映射。WebProcess 不能再按“独占整机”预算内存，否则内核会
     * 先杀 MiniApp 生命周期宿主。低内存设备给 WebProcess 340MB，并在到达
     * kill 阈值前主动回收；更大内存设备仍按物理内存比例扩展。
     */
    guint memory_limit_mb = 340;
    guint64 total_mb = 0;
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0 && si.totalram > 0) {
            total_mb = (guint64)si.totalram * si.mem_unit / (1024 * 1024);
            if (total_mb > 1536)
                memory_limit_mb = (guint)MIN(total_mb * 45 / 100, 768);
        }
    }
    const char *memory_limit_override = g_getenv("WPE_WEB_PROCESS_MEMORY_LIMIT_MB");
    if (memory_limit_override && memory_limit_override[0]) {
        char *end = NULL;
        guint64 parsed = g_ascii_strtoull(memory_limit_override, &end, 10);
        if (end != memory_limit_override && !*end && parsed >= 192 && parsed <= 2048)
            memory_limit_mb = (guint)parsed;
        else
            g_warning("Ignoring invalid WPE_WEB_PROCESS_MEMORY_LIMIT_MB=%s", memory_limit_override);
    }
    WebKitMemoryPressureSettings *memory_pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(memory_pressure, memory_limit_mb);
    /* setter 断言要求 conservative < strict < kill，必须先抬高 strict 再设 conservative */
    webkit_memory_pressure_settings_set_strict_threshold(memory_pressure, 0.65);
    webkit_memory_pressure_settings_set_conservative_threshold(memory_pressure, 0.45);
    webkit_memory_pressure_settings_set_kill_threshold(memory_pressure, 0.90);
    WebKitWebContext *context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
        "memory-pressure-settings", memory_pressure,
        NULL);
    WebKitCacheModel cache_model = total_mb && total_mb <= 1536
        ? WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER
        : WEBKIT_CACHE_MODEL_WEB_BROWSER;
    const char *cache_model_name = g_getenv("WPE_WEBKIT_CACHE_MODEL");
    if (cache_model_name && !g_ascii_strcasecmp(cache_model_name, "document-viewer"))
        cache_model = WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER;
    else if (cache_model_name && !g_ascii_strcasecmp(cache_model_name, "document-browser"))
        cache_model = WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER;
    else if (cache_model_name && !g_ascii_strcasecmp(cache_model_name, "web-browser"))
        cache_model = WEBKIT_CACHE_MODEL_WEB_BROWSER;
    webkit_web_context_set_cache_model(context, cache_model);
    webkit_web_context_add_path_to_sandbox(context, "/userdisk", TRUE);
    webkit_web_context_add_path_to_sandbox(context, "/tmp", FALSE);
    const char *cache_model_log = cache_model == WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER ? "document-viewer"
        : cache_model == WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER ? "document-browser" : "web-browser";
    g_print("WebKit context: 2022 GLib API sandbox_paths=/userdisk,/tmp cache_model=%s total_ram=%" G_GUINT64_FORMAT "MB mem_limit=%uMB kill=0.90\n",
        cache_model_log, total_mb, memory_limit_mb);
    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings,
                                          profile_runtime.profile.javascript_enabled);
    webkit_settings_set_enable_javascript_markup(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, gpu_render_profile);
    webkit_settings_set_enable_2d_canvas_acceleration(settings, gpu_render_profile);
    webkit_settings_set_enable_media(settings, TRUE);
    webkit_settings_set_enable_webaudio(settings, TRUE);
    webkit_settings_set_enable_mediasource(settings, TRUE);
    webkit_settings_set_enable_fullscreen(settings, TRUE);
    gboolean webrtc_enabled = env_enabled("WPE_WEBRTC", TRUE);
    webkit_settings_set_enable_media_stream(settings, webrtc_enabled);
    webkit_settings_set_enable_webrtc(settings, webrtc_enabled);
    webkit_settings_set_media_playback_requires_user_gesture(
        settings, profile_runtime.profile.autoplay_requires_gesture);
    webkit_settings_set_media_playback_allows_inline(settings, TRUE);
    webkit_settings_set_default_font_size(
        settings, CLAMP(profile_runtime.profile.default_font_size, 14, 20));
    webkit_settings_set_enable_smooth_scrolling(
        settings, profile_runtime.profile.smooth_scrolling);
    webkit_settings_set_javascript_can_open_windows_automatically(
        settings, !profile_runtime.profile.block_popups);
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
    webkit_network_session_set_memory_pressure_settings(memory_pressure);
    webkit_memory_pressure_settings_free(memory_pressure);
    WebKitNetworkSession *network_session = profile_network_session_new();
    if (!network_session) {
        g_printerr("Failed to create isolated WebKit network session\n");
        g_object_unref(user_content_manager);
        g_object_unref(settings);
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
        "display", display,
        NULL);
    g_object_unref(settings);

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
        state->cloud_autostart = env_enabled("WPE_CLOUD_AUTOSTART", FALSE);
        state->viewport_width = width;
        state->viewport_height = height;
        chrome_load_state(state, url);
        g_strlcpy(state->input_profile, normalize_input_profile(g_getenv("WPE_INPUT_PROFILE")), sizeof(state->input_profile));
        const char *initial_input_uri = chrome_active_tab(&state->chrome) && chrome_active_tab(&state->chrome)->url[0]
            ? chrome_active_tab(&state->chrome)->url : url;
        state->game_input_active = !g_strcmp0(state->input_profile, "game");
        g_print("Input profile: configured=%s active=%s reason=initial uri=%s\n",
                state->input_profile, state->game_input_active ? "game" : "browser",
                initial_input_uri ? initial_input_uri : "(null)");
        chrome_apply_web_preferences(state, FALSE);
        setup_keyboard_bridge(state);
        setup_keyboard_user_script(user_content_manager, state);
        setup_cloud_autostart_user_script(user_content_manager, state);
        setup_game_input_user_script(user_content_manager, state);
        g_signal_connect(web_view, "load-changed",
                         G_CALLBACK(on_load_changed), state);
        g_signal_connect(web_view, "notify::estimated-load-progress",
                         G_CALLBACK(on_estimated_load_progress_changed), state);
        g_signal_connect(web_view, "notify::title",
                         G_CALLBACK(on_title_changed), state);
        g_signal_connect(web_view, "enter-fullscreen",
                         G_CALLBACK(on_enter_fullscreen), state);
        g_signal_connect(web_view, "leave-fullscreen",
                         G_CALLBACK(on_leave_fullscreen), state);

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
    gboolean force_startup_url = env_enabled("WPE_START_URL_OVERRIDE", FALSE);
    if (!force_startup_url && state && chrome_active_tab(&state->chrome) && chrome_active_tab(&state->chrome)->url[0])
        startup_url = chrome_active_tab(&state->chrome)->url;
    if (force_startup_url)
        g_print("Startup URL override: %s\n", startup_url);
    if (state)
        update_page_zoom_for_uri(state, startup_url);
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
        for (int i = 0; i < MAX_TOUCH_SLOTS; ++i)
            game_reset_touch_slot(&state->slots[i]);
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
