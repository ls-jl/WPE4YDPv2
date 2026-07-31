#pragma once

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

#define BROWSER_PROFILE_LIMIT 8
#define BROWSER_HISTORY_LIMIT 1000

typedef enum {
    BROWSER_COOKIE_ACCEPT_ALL = 0,
    BROWSER_COOKIE_NO_THIRD_PARTY,
    BROWSER_COOKIE_ACCEPT_NEVER
} BrowserCookiePolicy;

typedef struct {
    int64_t id;
    char *name;
    gboolean is_default;
    char *home_url;
    char *site_profile;
    BrowserCookiePolicy cookie_policy;
    char *search_engine;
    char *custom_search_template;
    double page_zoom;
    guint default_font_size;
    gboolean javascript_enabled;
    gboolean autoplay_requires_gesture;
    gboolean smooth_scrolling;
    gboolean block_popups;
    gboolean restore_tabs;
} BrowserProfile;

typedef struct {
    char theme[8];
    gboolean toolbar_auto_hide;
    gboolean gpu_acceleration;
} BrowserGlobalSettings;

typedef struct {
    guint logical_id;
    char *url;
    char *title;
    gboolean active;
    GPtrArray *back;
    GPtrArray *forward;
} BrowserStoredTab;

typedef struct {
    int64_t id;
    char *url;
    char *title;
    int64_t timestamp;
} BrowserStoredPage;

typedef struct BrowserProfileStore BrowserProfileStore;

BrowserProfileStore *browser_profile_store_open(const char *database_path,
                                                 const char *profiles_directory,
                                                 const char *legacy_state_path,
                                                 const char *legacy_runtime_path,
                                                 GError **error);
void browser_profile_store_close(BrowserProfileStore *store);

void browser_profile_clear(BrowserProfile *profile);
void browser_stored_tab_free(BrowserStoredTab *tab);
void browser_stored_page_free(BrowserStoredPage *page);

gboolean browser_profile_store_get_profile(BrowserProfileStore *store, int64_t profile_id,
                                           BrowserProfile *profile, GError **error);
gboolean browser_profile_store_get_active_profile(BrowserProfileStore *store,
                                                  BrowserProfile *profile, GError **error);
GPtrArray *browser_profile_store_list_profiles(BrowserProfileStore *store, GError **error);
gboolean browser_profile_store_create_profile(BrowserProfileStore *store, const char *name,
                                              BrowserProfile *profile, GError **error);
gboolean browser_profile_store_rename_profile(BrowserProfileStore *store, int64_t profile_id,
                                              const char *name, GError **error);
gboolean browser_profile_store_set_active_profile(BrowserProfileStore *store, int64_t profile_id,
                                                  GError **error);
gboolean browser_profile_store_prepare_delete_profile(BrowserProfileStore *store, int64_t profile_id,
                                                      int64_t *fallback_profile_id, GError **error);
gboolean browser_profile_store_is_name_valid(const char *name, char **normalized_name,
                                             GError **error);

gboolean browser_profile_store_set_home_url(BrowserProfileStore *store, int64_t profile_id,
                                            const char *home_url, GError **error);
gboolean browser_profile_store_set_site_profile(BrowserProfileStore *store, int64_t profile_id,
                                                const char *site_profile, GError **error);
gboolean browser_profile_store_set_cookie_policy(BrowserProfileStore *store, int64_t profile_id,
                                                 BrowserCookiePolicy policy, GError **error);
gboolean browser_profile_store_save_preferences(BrowserProfileStore *store,
                                                const BrowserProfile *profile,
                                                GError **error);
void browser_profile_store_get_global_settings(BrowserProfileStore *store,
                                               BrowserGlobalSettings *settings);
gboolean browser_profile_store_save_global_settings(BrowserProfileStore *store,
                                                    const BrowserGlobalSettings *settings,
                                                    GError **error);
const char *browser_cookie_policy_name(BrowserCookiePolicy policy);

GPtrArray *browser_profile_store_load_tabs(BrowserProfileStore *store, int64_t profile_id,
                                          guint *next_tab_id, GError **error);
gboolean browser_profile_store_save_tabs(BrowserProfileStore *store, int64_t profile_id,
                                         GPtrArray *tabs, guint next_tab_id, GError **error);

int64_t browser_profile_store_record_visit(BrowserProfileStore *store, int64_t profile_id,
                                           const char *url, const char *title, GError **error);
gboolean browser_profile_store_update_visit_title(BrowserProfileStore *store, int64_t visit_id,
                                                  const char *title, GError **error);
GPtrArray *browser_profile_store_list_visits(BrowserProfileStore *store, int64_t profile_id,
                                            guint offset, guint limit, GError **error);
gboolean browser_profile_store_delete_visit(BrowserProfileStore *store, int64_t profile_id,
                                            int64_t visit_id, GError **error);
gboolean browser_profile_store_clear_visits(BrowserProfileStore *store, int64_t profile_id,
                                            GError **error);

gboolean browser_profile_store_has_bookmark(BrowserProfileStore *store, int64_t profile_id,
                                            const char *url);
gboolean browser_profile_store_toggle_bookmark(BrowserProfileStore *store, int64_t profile_id,
                                               const char *url, const char *title,
                                               gboolean *added, GError **error);
GPtrArray *browser_profile_store_list_bookmarks(BrowserProfileStore *store, int64_t profile_id,
                                               guint offset, guint limit, GError **error);
gboolean browser_profile_store_delete_bookmark(BrowserProfileStore *store, int64_t profile_id,
                                               int64_t bookmark_id, GError **error);

char *browser_profile_store_profile_root(BrowserProfileStore *store, int64_t profile_id);

G_END_DECLS
