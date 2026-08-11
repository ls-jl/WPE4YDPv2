#include "browser-profile-store.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define STORE_ERROR browser_profile_store_error_quark()

struct BrowserProfileStore {
    sqlite3 *database;
    char *database_path;
    char *profiles_directory;
};

static GQuark browser_profile_store_error_quark(void)
{
    return g_quark_from_static_string("browser-profile-store-error");
}

static void set_sqlite_error(GError **error, sqlite3 *database, const char *operation, int result)
{
    g_set_error(error, STORE_ERROR, result, "%s failed: %s (%d)", operation,
                database ? sqlite3_errmsg(database) : sqlite3_errstr(result), result);
}

static gboolean execute_sql(sqlite3 *database, const char *sql, GError **error)
{
    char *message = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &message);
    if (result == SQLITE_OK)
        return TRUE;
    g_set_error(error, STORE_ERROR, result, "SQLite statement failed: %s; sql=%s",
                message ? message : sqlite3_errstr(result), sql);
    sqlite3_free(message);
    return FALSE;
}

static gboolean prepare(sqlite3 *database, const char *sql, sqlite3_stmt **statement, GError **error)
{
    int result = sqlite3_prepare_v2(database, sql, -1, statement, NULL);
    if (result == SQLITE_OK)
        return TRUE;
    set_sqlite_error(error, database, "sqlite3_prepare_v2", result);
    return FALSE;
}

static gboolean bind_text(sqlite3_stmt *statement, int index, const char *value, GError **error)
{
    int result = sqlite3_bind_text(statement, index, value ? value : "", -1, SQLITE_TRANSIENT);
    if (result == SQLITE_OK)
        return TRUE;
    set_sqlite_error(error, sqlite3_db_handle(statement), "sqlite3_bind_text", result);
    return FALSE;
}

static gboolean step_done(sqlite3_stmt *statement, GError **error)
{
    int result = sqlite3_step(statement);
    if (result == SQLITE_DONE)
        return TRUE;
    set_sqlite_error(error, sqlite3_db_handle(statement), "sqlite3_step", result);
    return FALSE;
}

static gboolean begin_transaction(sqlite3 *database, GError **error)
{
    return execute_sql(database, "BEGIN IMMEDIATE", error);
}

static gboolean commit_transaction(sqlite3 *database, GError **error)
{
    return execute_sql(database, "COMMIT", error);
}

static void rollback_transaction(sqlite3 *database)
{
    sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
}

static char *column_string(sqlite3_stmt *statement, int column)
{
    const unsigned char *value = sqlite3_column_text(statement, column);
    return g_strdup(value ? (const char *)value : "");
}

void browser_profile_clear(BrowserProfile *profile)
{
    if (!profile)
        return;
    g_free(profile->name);
    g_free(profile->home_url);
    g_free(profile->site_profile);
    g_free(profile->search_engine);
    g_free(profile->custom_search_template);
    g_free(profile->language);
    memset(profile, 0, sizeof(*profile));
}

static void browser_profile_free(BrowserProfile *profile)
{
    if (!profile)
        return;
    browser_profile_clear(profile);
    g_free(profile);
}

void browser_stored_tab_free(BrowserStoredTab *tab)
{
    if (!tab)
        return;
    g_free(tab->url);
    g_free(tab->title);
    g_clear_pointer(&tab->back, g_ptr_array_unref);
    g_clear_pointer(&tab->forward, g_ptr_array_unref);
    g_free(tab);
}

void browser_stored_page_free(BrowserStoredPage *page)
{
    if (!page)
        return;
    g_free(page->url);
    g_free(page->title);
    g_free(page);
}

static gboolean remove_tree(const char *path)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return TRUE;
    if (!g_file_test(path, G_FILE_TEST_IS_DIR))
        return g_unlink(path) == 0;

    GDir *directory = g_dir_open(path, 0, NULL);
    if (!directory)
        return FALSE;
    const char *name;
    gboolean success = TRUE;
    while ((name = g_dir_read_name(directory))) {
        char *child = g_build_filename(path, name, NULL);
        if (!remove_tree(child))
            success = FALSE;
        g_free(child);
    }
    g_dir_close(directory);
    if (g_rmdir(path) != 0 && errno != ENOENT)
        success = FALSE;
    return success;
}

static gboolean set_meta(BrowserProfileStore *store, const char *key, const char *value, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database,
                 "INSERT INTO meta(key,value) VALUES(?1,?2) "
                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value", &statement, error))
        return FALSE;
    gboolean success = bind_text(statement, 1, key, error)
        && bind_text(statement, 2, value, error)
        && step_done(statement, error);
    sqlite3_finalize(statement);
    return success;
}

static char *get_meta(BrowserProfileStore *store, const char *key)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database, "SELECT value FROM meta WHERE key=?1", -1,
                           &statement, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_TRANSIENT);
    char *value = sqlite3_step(statement) == SQLITE_ROW ? column_string(statement, 0) : NULL;
    sqlite3_finalize(statement);
    return value;
}

static int64_t get_meta_id(BrowserProfileStore *store, const char *key)
{
    char *value = get_meta(store, key);
    int64_t identifier = value ? g_ascii_strtoll(value, NULL, 10) : 0;
    g_free(value);
    return identifier;
}

static gboolean set_meta_id(BrowserProfileStore *store, const char *key, int64_t identifier,
                            GError **error)
{
    char value[32];
    g_snprintf(value, sizeof(value), "%" G_GINT64_FORMAT, identifier);
    return set_meta(store, key, value, error);
}

static gboolean initialize_schema(BrowserProfileStore *store, GError **error)
{
    static const char schema[] =
        "PRAGMA foreign_keys=ON;"
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA busy_timeout=3000;"
        "CREATE TABLE IF NOT EXISTS meta("
        " key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS profiles("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL UNIQUE,"
        " is_default INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL,"
        " last_used_at INTEGER NOT NULL);"
        "CREATE UNIQUE INDEX IF NOT EXISTS one_default_profile"
        " ON profiles(is_default) WHERE is_default=1;"
        "CREATE TABLE IF NOT EXISTS profile_settings("
        " profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,"
        " key TEXT NOT NULL, value TEXT NOT NULL,"
        " PRIMARY KEY(profile_id,key));"
        "CREATE TABLE IF NOT EXISTS tabs("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,"
        " logical_id INTEGER NOT NULL, position INTEGER NOT NULL,"
        " url TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', active INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(profile_id,position));"
        "CREATE TABLE IF NOT EXISTS tab_navigation("
        " tab_id INTEGER NOT NULL REFERENCES tabs(id) ON DELETE CASCADE,"
        " kind INTEGER NOT NULL, position INTEGER NOT NULL, url TEXT NOT NULL,"
        " PRIMARY KEY(tab_id,kind,position));"
        "CREATE TABLE IF NOT EXISTS visits("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,"
        " url TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', visited_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS visits_profile_time ON visits(profile_id,visited_at DESC,id DESC);"
        "CREATE TABLE IF NOT EXISTS bookmarks("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,"
        " url TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL,"
        " UNIQUE(profile_id,url));"
        "CREATE INDEX IF NOT EXISTS bookmarks_profile_time ON bookmarks(profile_id,created_at DESC,id DESC);"
        "PRAGMA user_version=1;";
    return execute_sql(store->database, schema, error);
}

static gboolean set_setting(BrowserProfileStore *store, int64_t profile_id,
                            const char *key, const char *value, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database,
                 "INSERT INTO profile_settings(profile_id,key,value) VALUES(?1,?2,?3) "
                 "ON CONFLICT(profile_id,key) DO UPDATE SET value=excluded.value",
                 &statement, error))
        return FALSE;
    sqlite3_bind_int64(statement, 1, profile_id);
    gboolean success = bind_text(statement, 2, key, error)
        && bind_text(statement, 3, value, error)
        && step_done(statement, error);
    sqlite3_finalize(statement);
    return success;
}

static char *get_setting(BrowserProfileStore *store, int64_t profile_id,
                         const char *key, const char *fallback)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database,
                           "SELECT value FROM profile_settings WHERE profile_id=?1 AND key=?2",
                           -1, &statement, NULL) != SQLITE_OK)
        return g_strdup(fallback);
    sqlite3_bind_int64(statement, 1, profile_id);
    sqlite3_bind_text(statement, 2, key, -1, SQLITE_TRANSIENT);
    char *value = sqlite3_step(statement) == SQLITE_ROW
        ? column_string(statement, 0) : g_strdup(fallback);
    sqlite3_finalize(statement);
    return value;
}

static gboolean setting_boolean(BrowserProfileStore *store, int64_t profile_id,
                                const char *key, gboolean fallback)
{
    char *value = get_setting(store, profile_id, key, fallback ? "1" : "0");
    gboolean result = value && (!g_ascii_strcasecmp(value, "1")
        || !g_ascii_strcasecmp(value, "true")
        || !g_ascii_strcasecmp(value, "yes")
        || !g_ascii_strcasecmp(value, "on"));
    g_free(value);
    return result;
}

static double setting_double(BrowserProfileStore *store, int64_t profile_id,
                             const char *key, double fallback,
                             double minimum, double maximum)
{
    char fallback_value[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(fallback_value, sizeof(fallback_value), fallback);
    char *value = get_setting(store, profile_id, key, fallback_value);
    char *end = NULL;
    double parsed = g_ascii_strtod(value, &end);
    if (!value[0] || end == value || *end || !isfinite(parsed))
        parsed = fallback;
    g_free(value);
    return CLAMP(parsed, minimum, maximum);
}

static guint setting_uint(BrowserProfileStore *store, int64_t profile_id,
                          const char *key, guint fallback,
                          guint minimum, guint maximum)
{
    char fallback_value[16];
    g_snprintf(fallback_value, sizeof(fallback_value), "%u", fallback);
    char *value = get_setting(store, profile_id, key, fallback_value);
    char *end = NULL;
    guint64 parsed = g_ascii_strtoull(value, &end, 10);
    if (!value[0] || end == value || *end)
        parsed = fallback;
    g_free(value);
    return (guint)CLAMP(parsed, minimum, maximum);
}

static gboolean set_default_settings(BrowserProfileStore *store, int64_t profile_id,
                                     GError **error)
{
    return set_setting(store, profile_id, "home_url", "https://m.baidu.com/", error)
        && set_setting(store, profile_id, "site_profile", "mobile", error)
        && set_setting(store, profile_id, "cookie_policy", "all", error)
        && set_setting(store, profile_id, "search_engine", "baidu", error)
        && set_setting(store, profile_id, "custom_search_template", "", error)
        && set_setting(store, profile_id, "language", "zh-CN", error)
        && set_setting(store, profile_id, "page_zoom", "1", error)
        && set_setting(store, profile_id, "default_font_size", "16", error)
        && set_setting(store, profile_id, "javascript_enabled", "1", error)
        && set_setting(store, profile_id, "autoplay_requires_gesture", "0", error)
        && set_setting(store, profile_id, "smooth_scrolling", "0", error)
        && set_setting(store, profile_id, "block_popups", "1", error)
        && set_setting(store, profile_id, "restore_tabs", "1", error)
        && set_setting(store, profile_id, "next_tab_id", "2", error);
}

static gboolean create_default_profile(BrowserProfileStore *store, int64_t *profile_id, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database,
                 "INSERT INTO profiles(name,is_default,created_at,last_used_at) "
                 "VALUES('DEFAULT',1,strftime('%s','now'),strftime('%s','now'))", &statement, error))
        return FALSE;
    gboolean success = step_done(statement, error);
    sqlite3_finalize(statement);
    if (!success)
        return FALSE;
    *profile_id = sqlite3_last_insert_rowid(store->database);
    if (!set_meta_id(store, "active_profile_id", *profile_id, error))
        return FALSE;
    return set_default_settings(store, *profile_id, error);
}

static gboolean database_has_profiles(BrowserProfileStore *store)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database, "SELECT 1 FROM profiles LIMIT 1", -1,
                           &statement, NULL) != SQLITE_OK)
        return FALSE;
    gboolean present = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return present;
}

static gboolean quick_check(sqlite3 *database)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "PRAGMA quick_check(1)", -1, &statement, NULL) != SQLITE_OK)
        return FALSE;
    gboolean valid = FALSE;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(statement, 0);
        valid = value && !strcmp(value, "ok");
    }
    sqlite3_finalize(statement);
    return valid;
}

static sqlite3 *open_database_with_recovery(const char *path, gboolean *fresh, GError **error)
{
    *fresh = !g_file_test(path, G_FILE_TEST_EXISTS);
    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(path, &database,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                 NULL);
    if (result != SQLITE_OK) {
        set_sqlite_error(error, database, "sqlite3_open_v2", result);
        sqlite3_close(database);
        return NULL;
    }
    if (*fresh || quick_check(database))
        return database;

    sqlite3_close(database);
    char *backup = g_strdup_printf("%s.corrupt-%" G_GINT64_FORMAT, path, (gint64)time(NULL));
    if (g_rename(path, backup) != 0) {
        g_set_error(error, STORE_ERROR, errno, "Corrupt database rename failed: %s", g_strerror(errno));
        g_free(backup);
        return NULL;
    }
    char *wal = g_strconcat(path, "-wal", NULL);
    char *shm = g_strconcat(path, "-shm", NULL);
    g_unlink(wal);
    g_unlink(shm);
    g_free(wal);
    g_free(shm);
    g_warning("Browser database was corrupt; preserved as %s", backup);
    g_free(backup);
    *fresh = TRUE;
    result = sqlite3_open_v2(path, &database,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (result != SQLITE_OK) {
        set_sqlite_error(error, database, "sqlite3_open_v2 recovery", result);
        sqlite3_close(database);
        return NULL;
    }
    return database;
}

static BrowserCookiePolicy cookie_policy_from_name(const char *name)
{
    if (!g_strcmp0(name, "no-third-party"))
        return BROWSER_COOKIE_NO_THIRD_PARTY;
    if (!g_strcmp0(name, "never"))
        return BROWSER_COOKIE_ACCEPT_NEVER;
    return BROWSER_COOKIE_ACCEPT_ALL;
}

const char *browser_cookie_policy_name(BrowserCookiePolicy policy)
{
    switch (policy) {
    case BROWSER_COOKIE_NO_THIRD_PARTY:
        return "no-third-party";
    case BROWSER_COOKIE_ACCEPT_NEVER:
        return "never";
    case BROWSER_COOKIE_ACCEPT_ALL:
    default:
        return "all";
    }
}

char *browser_profile_store_profile_root(BrowserProfileStore *store, int64_t profile_id)
{
    g_return_val_if_fail(store, NULL);
    return g_strdup_printf("%s/p%" G_GINT64_FORMAT, store->profiles_directory, profile_id);
}

static gboolean load_legacy_tabs(BrowserProfileStore *store, int64_t profile_id,
                                 GKeyFile *key_file, GError **error)
{
    int count = g_key_file_get_integer(key_file, "tabs", "count", NULL);
    int active = g_key_file_get_integer(key_file, "tabs", "active", NULL);
    guint next_id = (guint)g_key_file_get_integer(key_file, "tabs", "next_id", NULL);
    if (count < 1)
        return TRUE;
    if (count > 8)
        count = 8;

    GPtrArray *tabs = g_ptr_array_new_with_free_func((GDestroyNotify)browser_stored_tab_free);
    for (int index = 0; index < count; ++index) {
        char group[32];
        g_snprintf(group, sizeof(group), "tab%d", index);
        char *url = g_key_file_get_string(key_file, group, "url", NULL);
        if (!url || !url[0]) {
            g_free(url);
            continue;
        }
        BrowserStoredTab *tab = g_new0(BrowserStoredTab, 1);
        tab->logical_id = (guint)g_key_file_get_integer(key_file, group, "id", NULL);
        if (!tab->logical_id)
            tab->logical_id = (guint)index + 1;
        tab->url = url;
        tab->title = g_key_file_get_string(key_file, group, "title", NULL);
        if (!tab->title)
            tab->title = g_strdup("");
        tab->active = index == active;
        tab->back = g_ptr_array_new_with_free_func(g_free);
        tab->forward = g_ptr_array_new_with_free_func(g_free);

        gsize length = 0;
        char **items = g_key_file_get_string_list(key_file, group, "back", &length, NULL);
        for (gsize item = 0; items && item < length; ++item)
            g_ptr_array_add(tab->back, g_strdup(items[item]));
        g_strfreev(items);
        items = g_key_file_get_string_list(key_file, group, "forward", &length, NULL);
        for (gsize item = 0; items && item < length; ++item)
            g_ptr_array_add(tab->forward, g_strdup(items[item]));
        g_strfreev(items);
        g_ptr_array_add(tabs, tab);
    }
    if (!tabs->len) {
        g_ptr_array_unref(tabs);
        return TRUE;
    }
    if (next_id < 2)
        next_id = tabs->len + 1;
    gboolean success = browser_profile_store_save_tabs(store, profile_id, tabs, next_id, error);
    g_ptr_array_unref(tabs);
    return success;
}

static gboolean migrate_legacy_state(BrowserProfileStore *store, int64_t profile_id,
                                     const char *legacy_state_path,
                                     const char *legacy_runtime_path, GError **error)
{
    char *migrated = get_meta(store, "legacy_migrated");
    gboolean already_migrated = migrated && !g_strcmp0(migrated, "1");
    g_free(migrated);
    if (already_migrated)
        return TRUE;

    if (legacy_state_path && g_file_test(legacy_state_path, G_FILE_TEST_IS_REGULAR)) {
        GKeyFile *key_file = g_key_file_new();
        if (!g_key_file_load_from_file(key_file, legacy_state_path, G_KEY_FILE_NONE, error)) {
            g_key_file_unref(key_file);
            return FALSE;
        }
        char *home_url = g_key_file_get_string(key_file, "settings", "home_url", NULL);
        char *site_profile = g_key_file_get_string(key_file, "settings", "site_profile", NULL);
        gboolean success = (!home_url || set_setting(store, profile_id, "home_url", home_url, error))
            && (!site_profile || set_setting(store, profile_id, "site_profile", site_profile, error))
            && load_legacy_tabs(store, profile_id, key_file, error);
        g_free(home_url);
        g_free(site_profile);
        g_key_file_unref(key_file);
        if (!success)
            return FALSE;

        char *backup = g_strconcat(legacy_state_path, ".migrated.bak", NULL);
        g_unlink(backup);
        if (g_rename(legacy_state_path, backup) != 0) {
            g_set_error(error, STORE_ERROR, errno, "Legacy state backup failed: %s",
                        g_strerror(errno));
            g_free(backup);
            return FALSE;
        }
        g_print("Profile migration: state=%s backup=%s\n", legacy_state_path, backup);
        g_free(backup);
    }

    if (legacy_runtime_path && g_file_test(legacy_runtime_path, G_FILE_TEST_IS_DIR)) {
        char *root = browser_profile_store_profile_root(store, profile_id);
        char *runtime = g_build_filename(root, "runtime", NULL);
        if (!g_file_test(runtime, G_FILE_TEST_EXISTS)) {
            g_mkdir_with_parents(root, 0700);
            if (g_rename(legacy_runtime_path, runtime) != 0) {
                g_set_error(error, STORE_ERROR, errno, "Legacy runtime migration failed: %s",
                            g_strerror(errno));
                g_free(runtime);
                g_free(root);
                return FALSE;
            }
            g_print("Profile migration: runtime=%s destination=%s\n",
                    legacy_runtime_path, runtime);
        }
        g_free(runtime);
        g_free(root);
    }
    return set_meta(store, "legacy_migrated", "1", error);
}

static void cleanup_pending_delete(BrowserProfileStore *store)
{
    int64_t profile_id = get_meta_id(store, "pending_delete_profile_id");
    if (profile_id <= 0)
        return;
    char *root = browser_profile_store_profile_root(store, profile_id);
    if (!remove_tree(root))
        g_warning("Profile cleanup failed: id=%" G_GINT64_FORMAT " path=%s", profile_id, root);
    else {
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(store->database, "DELETE FROM meta WHERE key='pending_delete_profile_id'",
                               -1, &statement, NULL) == SQLITE_OK)
            sqlite3_step(statement);
        sqlite3_finalize(statement);
        g_print("Profile cleanup: deleted id=%" G_GINT64_FORMAT " path=%s\n", profile_id, root);
    }
    g_free(root);
}

BrowserProfileStore *browser_profile_store_open(const char *database_path,
                                                 const char *profiles_directory,
                                                 const char *legacy_state_path,
                                                 const char *legacy_runtime_path,
                                                 GError **error)
{
    g_return_val_if_fail(database_path && database_path[0], NULL);
    g_return_val_if_fail(profiles_directory && profiles_directory[0], NULL);

    char *database_directory = g_path_get_dirname(database_path);
    if (g_mkdir_with_parents(database_directory, 0700) != 0
        || g_mkdir_with_parents(profiles_directory, 0700) != 0) {
        g_set_error(error, STORE_ERROR, errno, "Profile directory creation failed: %s",
                    g_strerror(errno));
        g_free(database_directory);
        return NULL;
    }
    chmod(database_directory, 0700);
    chmod(profiles_directory, 0700);
    g_free(database_directory);

    gboolean fresh = FALSE;
    sqlite3 *database = open_database_with_recovery(database_path, &fresh, error);
    if (!database)
        return NULL;

    BrowserProfileStore *store = g_new0(BrowserProfileStore, 1);
    store->database = database;
    store->database_path = g_strdup(database_path);
    store->profiles_directory = g_strdup(profiles_directory);
    if (!initialize_schema(store, error)) {
        browser_profile_store_close(store);
        return NULL;
    }
    chmod(database_path, 0600);

    int64_t default_profile_id = 0;
    if (!database_has_profiles(store)) {
        if (!begin_transaction(database, error)
            || !create_default_profile(store, &default_profile_id, error)
            || !commit_transaction(database, error)) {
            rollback_transaction(database);
            browser_profile_store_close(store);
            return NULL;
        }
    } else {
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(database, "SELECT id FROM profiles WHERE is_default=1 LIMIT 1",
                               -1, &statement, NULL) == SQLITE_OK
            && sqlite3_step(statement) == SQLITE_ROW)
            default_profile_id = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
    }
    if (default_profile_id <= 0) {
        g_set_error(error, STORE_ERROR, SQLITE_CORRUPT, "Browser database has no default profile");
        browser_profile_store_close(store);
        return NULL;
    }
    if (!migrate_legacy_state(store, default_profile_id, legacy_state_path,
                              legacy_runtime_path, error)) {
        browser_profile_store_close(store);
        return NULL;
    }
    cleanup_pending_delete(store);
    g_print("Browser database: path=%s profiles=%s fresh=%d\n",
            database_path, profiles_directory, fresh);
    return store;
}

void browser_profile_store_close(BrowserProfileStore *store)
{
    if (!store)
        return;
    if (store->database)
        sqlite3_close(store->database);
    g_free(store->database_path);
    g_free(store->profiles_directory);
    g_free(store);
}

gboolean browser_profile_store_get_profile(BrowserProfileStore *store, int64_t profile_id,
                                           BrowserProfile *profile, GError **error)
{
    g_return_val_if_fail(store && profile, FALSE);
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database,
                 "SELECT id,name,is_default FROM profiles WHERE id=?1", &statement, error))
        return FALSE;
    sqlite3_bind_int64(statement, 1, profile_id);
    int result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        sqlite3_finalize(statement);
        if (result == SQLITE_DONE)
            g_set_error(error, STORE_ERROR, SQLITE_NOTFOUND,
                        "Profile not found: %" G_GINT64_FORMAT, profile_id);
        else
            set_sqlite_error(error, store->database, "read profile", result);
        return FALSE;
    }
    memset(profile, 0, sizeof(*profile));
    profile->id = sqlite3_column_int64(statement, 0);
    profile->name = column_string(statement, 1);
    profile->is_default = sqlite3_column_int(statement, 2) != 0;
    sqlite3_finalize(statement);
    profile->home_url = get_setting(store, profile_id, "home_url", "https://m.baidu.com/");
    profile->site_profile = get_setting(store, profile_id, "site_profile", "mobile");
    char *cookie_policy = get_setting(store, profile_id, "cookie_policy", "all");
    profile->cookie_policy = cookie_policy_from_name(cookie_policy);
    g_free(cookie_policy);
    profile->search_engine = get_setting(store, profile_id, "search_engine", "baidu");
    if (g_strcmp0(profile->search_engine, "baidu")
            && g_strcmp0(profile->search_engine, "bing")
            && g_strcmp0(profile->search_engine, "google")
            && g_strcmp0(profile->search_engine, "custom")) {
        g_free(profile->search_engine);
        profile->search_engine = g_strdup("baidu");
    }
    profile->custom_search_template = get_setting(store, profile_id,
                                                  "custom_search_template", "");
    profile->language = get_setting(store, profile_id, "language", "zh-CN");
    if (g_ascii_strcasecmp(profile->language, "zh-CN")
            && g_ascii_strcasecmp(profile->language, "en-US")) {
        g_free(profile->language);
        profile->language = g_strdup("zh-CN");
    }
    profile->page_zoom = setting_double(store, profile_id, "page_zoom", 1.0, 0.75, 1.25);
    profile->default_font_size = setting_uint(store, profile_id,
                                              "default_font_size", 16, 14, 20);
    profile->javascript_enabled = setting_boolean(store, profile_id,
                                                  "javascript_enabled", TRUE);
    profile->autoplay_requires_gesture = setting_boolean(store, profile_id,
                                                         "autoplay_requires_gesture", FALSE);
    profile->smooth_scrolling = setting_boolean(store, profile_id,
                                                "smooth_scrolling", FALSE);
    profile->block_popups = setting_boolean(store, profile_id, "block_popups", TRUE);
    profile->restore_tabs = setting_boolean(store, profile_id, "restore_tabs", TRUE);
    return TRUE;
}

gboolean browser_profile_store_get_active_profile(BrowserProfileStore *store,
                                                  BrowserProfile *profile, GError **error)
{
    int64_t profile_id = get_meta_id(store, "active_profile_id");
    if (profile_id <= 0) {
        sqlite3_stmt *statement = NULL;
        if (!prepare(store->database, "SELECT id FROM profiles WHERE is_default=1 LIMIT 1",
                     &statement, error))
            return FALSE;
        if (sqlite3_step(statement) == SQLITE_ROW)
            profile_id = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
    }
    return profile_id > 0 && browser_profile_store_get_profile(store, profile_id, profile, error);
}

GPtrArray *browser_profile_store_list_profiles(BrowserProfileStore *store, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database,
                 "SELECT id FROM profiles ORDER BY is_default DESC,last_used_at DESC,id", &statement, error))
        return NULL;
    GPtrArray *profiles = g_ptr_array_new_with_free_func((GDestroyNotify)browser_profile_free);
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        BrowserProfile *profile = g_new0(BrowserProfile, 1);
        if (!browser_profile_store_get_profile(store, sqlite3_column_int64(statement, 0),
                                               profile, error)) {
            browser_profile_free(profile);
            g_ptr_array_unref(profiles);
            sqlite3_finalize(statement);
            return NULL;
        }
        g_ptr_array_add(profiles, profile);
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        set_sqlite_error(error, store->database, "list profiles", result);
        g_ptr_array_unref(profiles);
        return NULL;
    }
    return profiles;
}

gboolean browser_profile_store_is_name_valid(const char *name, char **normalized_name,
                                             GError **error)
{
    char *normalized = g_strdup(name ? name : "");
    g_strstrip(normalized);
    if (!g_utf8_validate(normalized, -1, NULL)) {
        g_set_error(error, STORE_ERROR, EINVAL, "Profile name is not valid UTF-8");
        g_free(normalized);
        return FALSE;
    }
    glong length = g_utf8_strlen(normalized, -1);
    if (length < 1 || length > 20) {
        g_set_error(error, STORE_ERROR, EINVAL, "Profile name must contain 1-20 characters");
        g_free(normalized);
        return FALSE;
    }
    for (const char *cursor = normalized; *cursor; cursor = g_utf8_next_char(cursor)) {
        if (g_unichar_iscntrl(g_utf8_get_char(cursor))) {
            g_set_error(error, STORE_ERROR, EINVAL, "Profile name contains control characters");
            g_free(normalized);
            return FALSE;
        }
    }
    if (normalized_name)
        *normalized_name = normalized;
    else
        g_free(normalized);
    return TRUE;
}

static gboolean profile_name_exists(BrowserProfileStore *store, const char *name,
                                    int64_t excluding_id)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database,
                           "SELECT 1 FROM profiles WHERE name=?1 AND id<>?2 LIMIT 1",
                           -1, &statement, NULL) != SQLITE_OK)
        return TRUE;
    sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, excluding_id);
    gboolean exists = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return exists;
}

gboolean browser_profile_store_create_profile(BrowserProfileStore *store, const char *name,
                                              BrowserProfile *profile, GError **error)
{
    char *normalized = NULL;
    if (!browser_profile_store_is_name_valid(name, &normalized, error))
        return FALSE;
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, "SELECT count(*) FROM profiles", &statement, error)) {
        g_free(normalized);
        return FALSE;
    }
    int result = sqlite3_step(statement);
    int count = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : BROWSER_PROFILE_LIMIT;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW || count >= BROWSER_PROFILE_LIMIT) {
        g_set_error(error, STORE_ERROR, ENOSPC, "Profile limit reached (%d)", BROWSER_PROFILE_LIMIT);
        g_free(normalized);
        return FALSE;
    }
    if (profile_name_exists(store, normalized, 0)) {
        g_set_error(error, STORE_ERROR, EEXIST, "Profile name already exists: %s", normalized);
        g_free(normalized);
        return FALSE;
    }

    if (!begin_transaction(store->database, error)) {
        g_free(normalized);
        return FALSE;
    }
    if (!prepare(store->database,
                 "INSERT INTO profiles(name,is_default,created_at,last_used_at) "
                 "VALUES(?1,0,strftime('%s','now'),strftime('%s','now'))", &statement, error)) {
        rollback_transaction(store->database);
        g_free(normalized);
        return FALSE;
    }
    gboolean success = bind_text(statement, 1, normalized, error) && step_done(statement, error);
    sqlite3_finalize(statement);
    int64_t profile_id = sqlite3_last_insert_rowid(store->database);
    success = success
        && set_default_settings(store, profile_id, error)
        && commit_transaction(store->database, error);
    if (!success)
        rollback_transaction(store->database);
    g_free(normalized);
    if (!success)
        return FALSE;
    return !profile || browser_profile_store_get_profile(store, profile_id, profile, error);
}

gboolean browser_profile_store_rename_profile(BrowserProfileStore *store, int64_t profile_id,
                                              const char *name, GError **error)
{
    char *normalized = NULL;
    if (!browser_profile_store_is_name_valid(name, &normalized, error))
        return FALSE;
    if (profile_name_exists(store, normalized, profile_id)) {
        g_set_error(error, STORE_ERROR, EEXIST, "Profile name already exists: %s", normalized);
        g_free(normalized);
        return FALSE;
    }
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, "UPDATE profiles SET name=?1 WHERE id=?2", &statement, error)) {
        g_free(normalized);
        return FALSE;
    }
    gboolean success = bind_text(statement, 1, normalized, error);
    sqlite3_bind_int64(statement, 2, profile_id);
    success = success && step_done(statement, error);
    sqlite3_finalize(statement);
    if (success && !sqlite3_changes(store->database)) {
        g_set_error(error, STORE_ERROR, SQLITE_NOTFOUND, "Profile not found: %" G_GINT64_FORMAT,
                    profile_id);
        success = FALSE;
    }
    g_free(normalized);
    return success;
}

gboolean browser_profile_store_set_active_profile(BrowserProfileStore *store, int64_t profile_id,
                                                  GError **error)
{
    BrowserProfile profile = { 0 };
    if (!browser_profile_store_get_profile(store, profile_id, &profile, error))
        return FALSE;
    browser_profile_clear(&profile);
    if (!begin_transaction(store->database, error))
        return FALSE;
    sqlite3_stmt *statement = NULL;
    gboolean success = prepare(store->database,
        "UPDATE profiles SET last_used_at=strftime('%s','now') WHERE id=?1", &statement, error);
    if (success) {
        sqlite3_bind_int64(statement, 1, profile_id);
        success = step_done(statement, error);
    }
    sqlite3_finalize(statement);
    success = success && set_meta_id(store, "active_profile_id", profile_id, error)
        && commit_transaction(store->database, error);
    if (!success)
        rollback_transaction(store->database);
    return success;
}

gboolean browser_profile_store_prepare_delete_profile(BrowserProfileStore *store, int64_t profile_id,
                                                      int64_t *fallback_profile_id, GError **error)
{
    BrowserProfile target = { 0 };
    if (!browser_profile_store_get_profile(store, profile_id, &target, error))
        return FALSE;
    if (target.is_default) {
        g_set_error(error, STORE_ERROR, EPERM, "The default profile cannot be deleted");
        browser_profile_clear(&target);
        return FALSE;
    }
    browser_profile_clear(&target);

    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, "SELECT id FROM profiles WHERE is_default=1", &statement, error))
        return FALSE;
    int result = sqlite3_step(statement);
    int64_t fallback = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : 0;
    sqlite3_finalize(statement);
    if (fallback <= 0) {
        g_set_error(error, STORE_ERROR, SQLITE_CORRUPT, "Default profile is missing");
        return FALSE;
    }

    int64_t active = get_meta_id(store, "active_profile_id");
    if (!begin_transaction(store->database, error))
        return FALSE;
    gboolean success = TRUE;
    if (active == profile_id)
        success = set_meta_id(store, "active_profile_id", fallback, error);
    success = success && set_meta_id(store, "pending_delete_profile_id", profile_id, error);
    if (success && prepare(store->database, "DELETE FROM profiles WHERE id=?1", &statement, error)) {
        sqlite3_bind_int64(statement, 1, profile_id);
        success = step_done(statement, error);
    } else
        success = FALSE;
    sqlite3_finalize(statement);
    success = success && commit_transaction(store->database, error);
    if (!success)
        rollback_transaction(store->database);
    if (!success)
        return FALSE;

    if (active != profile_id)
        cleanup_pending_delete(store);
    if (fallback_profile_id)
        *fallback_profile_id = fallback;
    return TRUE;
}

gboolean browser_profile_store_set_home_url(BrowserProfileStore *store, int64_t profile_id,
                                            const char *home_url, GError **error)
{
    return set_setting(store, profile_id, "home_url", home_url, error);
}

gboolean browser_profile_store_set_site_profile(BrowserProfileStore *store, int64_t profile_id,
                                                const char *site_profile, GError **error)
{
    const char *value = !g_strcmp0(site_profile, "desktop") ? "desktop" : "mobile";
    return set_setting(store, profile_id, "site_profile", value, error);
}

gboolean browser_profile_store_set_cookie_policy(BrowserProfileStore *store, int64_t profile_id,
                                                 BrowserCookiePolicy policy, GError **error)
{
    return set_setting(store, profile_id, "cookie_policy", browser_cookie_policy_name(policy), error);
}

gboolean browser_profile_store_save_preferences(BrowserProfileStore *store,
                                                const BrowserProfile *profile,
                                                GError **error)
{
    g_return_val_if_fail(store && profile && profile->id > 0, FALSE);
    char zoom[G_ASCII_DTOSTR_BUF_SIZE];
    char font_size[16];
    g_ascii_dtostr(zoom, sizeof(zoom), CLAMP(profile->page_zoom, 0.75, 1.25));
    g_snprintf(font_size, sizeof(font_size), "%u",
               CLAMP(profile->default_font_size, 14, 20));

    if (!begin_transaction(store->database, error))
        return FALSE;
    gboolean success =
        set_setting(store, profile->id, "search_engine",
                    profile->search_engine ? profile->search_engine : "baidu", error)
        && set_setting(store, profile->id, "custom_search_template",
                       profile->custom_search_template ? profile->custom_search_template : "", error)
        && set_setting(store, profile->id, "language",
                       profile->language && !g_ascii_strcasecmp(profile->language, "en-US")
                         ? "en-US" : "zh-CN", error)
        && set_setting(store, profile->id, "page_zoom", zoom, error)
        && set_setting(store, profile->id, "default_font_size", font_size, error)
        && set_setting(store, profile->id, "javascript_enabled",
                       profile->javascript_enabled ? "1" : "0", error)
        && set_setting(store, profile->id, "autoplay_requires_gesture",
                       profile->autoplay_requires_gesture ? "1" : "0", error)
        && set_setting(store, profile->id, "smooth_scrolling",
                       profile->smooth_scrolling ? "1" : "0", error)
        && set_setting(store, profile->id, "block_popups",
                       profile->block_popups ? "1" : "0", error)
        && set_setting(store, profile->id, "restore_tabs",
                       profile->restore_tabs ? "1" : "0", error)
        && commit_transaction(store->database, error);
    if (!success)
        rollback_transaction(store->database);
    return success;
}

void browser_profile_store_get_global_settings(BrowserProfileStore *store,
                                               BrowserGlobalSettings *settings)
{
    g_return_if_fail(settings);
    memset(settings, 0, sizeof(*settings));
    g_strlcpy(settings->theme, "light", sizeof(settings->theme));
    settings->toolbar_auto_hide = TRUE;
    settings->toolbar_gesture_in_immersive = TRUE;
    settings->gpu_acceleration = TRUE;
    if (!store)
        return;

    char *theme = get_meta(store, "ui_theme");
    if (theme && !g_ascii_strcasecmp(theme, "dark"))
        g_strlcpy(settings->theme, "dark", sizeof(settings->theme));
    g_free(theme);
    char *auto_hide = get_meta(store, "toolbar_auto_hide");
    if (auto_hide) {
        settings->toolbar_auto_hide = !g_ascii_strcasecmp(auto_hide, "1")
            || !g_ascii_strcasecmp(auto_hide, "true")
            || !g_ascii_strcasecmp(auto_hide, "yes")
            || !g_ascii_strcasecmp(auto_hide, "on");
    }
    g_free(auto_hide);
    char *gesture_in_immersive = get_meta(store, "toolbar_gesture_in_immersive");
    if (gesture_in_immersive) {
        settings->toolbar_gesture_in_immersive =
            !g_ascii_strcasecmp(gesture_in_immersive, "1")
            || !g_ascii_strcasecmp(gesture_in_immersive, "true")
            || !g_ascii_strcasecmp(gesture_in_immersive, "yes")
            || !g_ascii_strcasecmp(gesture_in_immersive, "on");
    }
    g_free(gesture_in_immersive);
    char *gpu_acceleration = get_meta(store, "gpu_acceleration");
    if (gpu_acceleration) {
        settings->gpu_acceleration =
            !g_ascii_strcasecmp(gpu_acceleration, "1")
            || !g_ascii_strcasecmp(gpu_acceleration, "true")
            || !g_ascii_strcasecmp(gpu_acceleration, "yes")
            || !g_ascii_strcasecmp(gpu_acceleration, "on")
            || !g_ascii_strcasecmp(gpu_acceleration, "auto");
    }
    g_free(gpu_acceleration);
}

gboolean browser_profile_store_save_global_settings(BrowserProfileStore *store,
                                                    const BrowserGlobalSettings *settings,
                                                    GError **error)
{
    g_return_val_if_fail(store && settings, FALSE);
    return set_meta(store, "ui_theme",
                    !g_ascii_strcasecmp(settings->theme, "dark") ? "dark" : "light", error)
        && set_meta(store, "toolbar_auto_hide",
                    settings->toolbar_auto_hide ? "1" : "0", error)
        && set_meta(store, "toolbar_gesture_in_immersive",
                    settings->toolbar_gesture_in_immersive ? "1" : "0", error)
        && set_meta(store, "gpu_acceleration",
                    settings->gpu_acceleration ? "1" : "0", error);
}

static gboolean insert_navigation(sqlite3 *database, int64_t tab_id, int kind,
                                  GPtrArray *items, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(database,
                 "INSERT INTO tab_navigation(tab_id,kind,position,url) VALUES(?1,?2,?3,?4)",
                 &statement, error))
        return FALSE;
    gboolean success = TRUE;
    for (guint index = 0; success && items && index < items->len; ++index) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        sqlite3_bind_int64(statement, 1, tab_id);
        sqlite3_bind_int(statement, 2, kind);
        sqlite3_bind_int(statement, 3, (int)index);
        success = bind_text(statement, 4, g_ptr_array_index(items, index), error)
            && step_done(statement, error);
    }
    sqlite3_finalize(statement);
    return success;
}

gboolean browser_profile_store_save_tabs(BrowserProfileStore *store, int64_t profile_id,
                                         GPtrArray *tabs, guint next_tab_id, GError **error)
{
    if (!begin_transaction(store->database, error))
        return FALSE;
    sqlite3_stmt *statement = NULL;
    gboolean success = prepare(store->database, "DELETE FROM tabs WHERE profile_id=?1",
                               &statement, error);
    if (success) {
        sqlite3_bind_int64(statement, 1, profile_id);
        success = step_done(statement, error);
    }
    sqlite3_finalize(statement);

    for (guint index = 0; success && tabs && index < tabs->len; ++index) {
        BrowserStoredTab *tab = g_ptr_array_index(tabs, index);
        success = prepare(store->database,
            "INSERT INTO tabs(profile_id,logical_id,position,url,title,active) "
            "VALUES(?1,?2,?3,?4,?5,?6)", &statement, error);
        if (!success)
            break;
        sqlite3_bind_int64(statement, 1, profile_id);
        sqlite3_bind_int(statement, 2, tab->logical_id);
        sqlite3_bind_int(statement, 3, (int)index);
        success = bind_text(statement, 4, tab->url, error)
            && bind_text(statement, 5, tab->title, error);
        sqlite3_bind_int(statement, 6, tab->active ? 1 : 0);
        success = success && step_done(statement, error);
        sqlite3_finalize(statement);
        statement = NULL;
        if (success) {
            int64_t tab_id = sqlite3_last_insert_rowid(store->database);
            success = insert_navigation(store->database, tab_id, 0, tab->back, error)
                && insert_navigation(store->database, tab_id, 1, tab->forward, error);
        }
    }
    char next_value[32];
    g_snprintf(next_value, sizeof(next_value), "%u", next_tab_id);
    success = success && set_setting(store, profile_id, "next_tab_id", next_value, error)
        && commit_transaction(store->database, error);
    if (!success)
        rollback_transaction(store->database);
    return success;
}

static GPtrArray *load_navigation(sqlite3 *database, int64_t tab_id, int kind)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
                           "SELECT url FROM tab_navigation WHERE tab_id=?1 AND kind=?2 ORDER BY position",
                           -1, &statement, NULL) != SQLITE_OK)
        return items;
    sqlite3_bind_int64(statement, 1, tab_id);
    sqlite3_bind_int(statement, 2, kind);
    while (sqlite3_step(statement) == SQLITE_ROW)
        g_ptr_array_add(items, column_string(statement, 0));
    sqlite3_finalize(statement);
    return items;
}

GPtrArray *browser_profile_store_load_tabs(BrowserProfileStore *store, int64_t profile_id,
                                          guint *next_tab_id, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database,
                 "SELECT id,logical_id,url,title,active FROM tabs WHERE profile_id=?1 ORDER BY position",
                 &statement, error))
        return NULL;
    sqlite3_bind_int64(statement, 1, profile_id);
    GPtrArray *tabs = g_ptr_array_new_with_free_func((GDestroyNotify)browser_stored_tab_free);
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        BrowserStoredTab *tab = g_new0(BrowserStoredTab, 1);
        int64_t tab_id = sqlite3_column_int64(statement, 0);
        tab->logical_id = (guint)sqlite3_column_int(statement, 1);
        tab->url = column_string(statement, 2);
        tab->title = column_string(statement, 3);
        tab->active = sqlite3_column_int(statement, 4) != 0;
        tab->back = load_navigation(store->database, tab_id, 0);
        tab->forward = load_navigation(store->database, tab_id, 1);
        g_ptr_array_add(tabs, tab);
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        set_sqlite_error(error, store->database, "load tabs", result);
        g_ptr_array_unref(tabs);
        return NULL;
    }
    char *value = get_setting(store, profile_id, "next_tab_id", "2");
    if (next_tab_id)
        *next_tab_id = (guint)MAX(2, g_ascii_strtoull(value, NULL, 10));
    g_free(value);
    return tabs;
}

int64_t browser_profile_store_record_visit(BrowserProfileStore *store, int64_t profile_id,
                                           const char *url, const char *title, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database,
                 "INSERT INTO visits(profile_id,url,title,visited_at) "
                 "VALUES(?1,?2,?3,strftime('%s','now'))", &statement, error))
        return 0;
    sqlite3_bind_int64(statement, 1, profile_id);
    gboolean success = bind_text(statement, 2, url, error)
        && bind_text(statement, 3, title, error)
        && step_done(statement, error);
    sqlite3_finalize(statement);
    if (!success)
        return 0;
    int64_t visit_id = sqlite3_last_insert_rowid(store->database);
    if (!prepare(store->database,
        "DELETE FROM visits WHERE profile_id=?1 AND id NOT IN "
        "(SELECT id FROM visits WHERE profile_id=?1 ORDER BY visited_at DESC,id DESC LIMIT ?2)",
        &statement, error))
        return visit_id;
    sqlite3_bind_int64(statement, 1, profile_id);
    sqlite3_bind_int(statement, 2, BROWSER_HISTORY_LIMIT);
    success = step_done(statement, error);
    sqlite3_finalize(statement);
    return success ? visit_id : 0;
}

gboolean browser_profile_store_update_visit_title(BrowserProfileStore *store, int64_t visit_id,
                                                  const char *title, GError **error)
{
    if (visit_id <= 0)
        return TRUE;
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, "UPDATE visits SET title=?1 WHERE id=?2", &statement, error))
        return FALSE;
    gboolean success = bind_text(statement, 1, title, error);
    sqlite3_bind_int64(statement, 2, visit_id);
    success = success && step_done(statement, error);
    sqlite3_finalize(statement);
    return success;
}

static GPtrArray *list_pages(BrowserProfileStore *store, const char *sql, int64_t profile_id,
                            guint offset, guint limit, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, sql, &statement, error))
        return NULL;
    sqlite3_bind_int64(statement, 1, profile_id);
    sqlite3_bind_int(statement, 2, (int)limit);
    sqlite3_bind_int(statement, 3, (int)offset);
    GPtrArray *pages = g_ptr_array_new_with_free_func((GDestroyNotify)browser_stored_page_free);
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        BrowserStoredPage *page = g_new0(BrowserStoredPage, 1);
        page->id = sqlite3_column_int64(statement, 0);
        page->url = column_string(statement, 1);
        page->title = column_string(statement, 2);
        page->timestamp = sqlite3_column_int64(statement, 3);
        g_ptr_array_add(pages, page);
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        set_sqlite_error(error, store->database, "list pages", result);
        g_ptr_array_unref(pages);
        return NULL;
    }
    return pages;
}

GPtrArray *browser_profile_store_list_visits(BrowserProfileStore *store, int64_t profile_id,
                                            guint offset, guint limit, GError **error)
{
    return list_pages(store,
        "SELECT id,url,title,visited_at FROM visits WHERE profile_id=?1 "
        "ORDER BY visited_at DESC,id DESC LIMIT ?2 OFFSET ?3",
        profile_id, offset, limit, error);
}

gboolean browser_profile_store_delete_visit(BrowserProfileStore *store, int64_t profile_id,
                                            int64_t visit_id, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, "DELETE FROM visits WHERE profile_id=?1 AND id=?2",
                 &statement, error))
        return FALSE;
    sqlite3_bind_int64(statement, 1, profile_id);
    sqlite3_bind_int64(statement, 2, visit_id);
    gboolean success = step_done(statement, error);
    sqlite3_finalize(statement);
    return success;
}

gboolean browser_profile_store_clear_visits(BrowserProfileStore *store, int64_t profile_id,
                                            GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, "DELETE FROM visits WHERE profile_id=?1", &statement, error))
        return FALSE;
    sqlite3_bind_int64(statement, 1, profile_id);
    gboolean success = step_done(statement, error);
    sqlite3_finalize(statement);
    return success;
}

gboolean browser_profile_store_has_bookmark(BrowserProfileStore *store, int64_t profile_id,
                                            const char *url)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database,
                           "SELECT 1 FROM bookmarks WHERE profile_id=?1 AND url=?2 LIMIT 1",
                           -1, &statement, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_int64(statement, 1, profile_id);
    sqlite3_bind_text(statement, 2, url ? url : "", -1, SQLITE_TRANSIENT);
    gboolean found = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return found;
}

gboolean browser_profile_store_toggle_bookmark(BrowserProfileStore *store, int64_t profile_id,
                                               const char *url, const char *title,
                                               gboolean *added, GError **error)
{
    gboolean present = browser_profile_store_has_bookmark(store, profile_id, url);
    sqlite3_stmt *statement = NULL;
    const char *sql = present
        ? "DELETE FROM bookmarks WHERE profile_id=?1 AND url=?2"
        : "INSERT INTO bookmarks(profile_id,url,title,created_at) "
          "VALUES(?1,?2,?3,strftime('%s','now'))";
    if (!prepare(store->database, sql, &statement, error))
        return FALSE;
    sqlite3_bind_int64(statement, 1, profile_id);
    gboolean success = bind_text(statement, 2, url, error);
    if (!present)
        success = success && bind_text(statement, 3, title, error);
    success = success && step_done(statement, error);
    sqlite3_finalize(statement);
    if (success && added)
        *added = !present;
    return success;
}

GPtrArray *browser_profile_store_list_bookmarks(BrowserProfileStore *store, int64_t profile_id,
                                               guint offset, guint limit, GError **error)
{
    return list_pages(store,
        "SELECT id,url,title,created_at FROM bookmarks WHERE profile_id=?1 "
        "ORDER BY created_at DESC,id DESC LIMIT ?2 OFFSET ?3",
        profile_id, offset, limit, error);
}

gboolean browser_profile_store_delete_bookmark(BrowserProfileStore *store, int64_t profile_id,
                                               int64_t bookmark_id, GError **error)
{
    sqlite3_stmt *statement = NULL;
    if (!prepare(store->database, "DELETE FROM bookmarks WHERE profile_id=?1 AND id=?2",
                 &statement, error))
        return FALSE;
    sqlite3_bind_int64(statement, 1, profile_id);
    sqlite3_bind_int64(statement, 2, bookmark_id);
    gboolean success = step_done(statement, error);
    sqlite3_finalize(statement);
    return success;
}
