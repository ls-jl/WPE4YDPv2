#include "../browser-profile-store.h"

#include <glib/gstdio.h>

static void remove_tree(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_unlink(path);
        return;
    }
    GDir *directory = g_dir_open(path, 0, NULL);
    const char *name;
    while (directory && (name = g_dir_read_name(directory))) {
        char *child = g_build_filename(path, name, NULL);
        remove_tree(child);
        g_free(child);
    }
    if (directory)
        g_dir_close(directory);
    g_rmdir(path);
}

static BrowserStoredTab *new_tab(const char *url)
{
    BrowserStoredTab *tab = g_new0(BrowserStoredTab, 1);
    tab->logical_id = 1;
    tab->url = g_strdup(url);
    tab->title = g_strdup("Initial");
    tab->active = TRUE;
    tab->back = g_ptr_array_new_with_free_func(g_free);
    tab->forward = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tab->back, g_strdup("https://example.com/back"));
    return tab;
}

static void test_profile_store(void)
{
    GError *error = NULL;
    char *root = g_dir_make_tmp("profile-store-test-XXXXXX", &error);
    g_assert_no_error(error);
    char *database = g_build_filename(root, "browser.sqlite3", NULL);
    char *profiles = g_build_filename(root, "profiles", NULL);
    char *legacy = g_build_filename(root, "browser-state.ini", NULL);
    char *legacy_runtime = g_build_filename(root, "runtime-tmp", NULL);
    g_mkdir_with_parents(legacy_runtime, 0700);
    g_file_set_contents(legacy,
        "[settings]\nhome_url=https://legacy.example/\nsite_profile=desktop\n"
        "[tabs]\ncount=1\nactive=0\nnext_id=3\n"
        "[tab0]\nid=2\nurl=https://legacy.example/page\ntitle=Legacy\n",
        -1, &error);
    g_assert_no_error(error);

    BrowserProfileStore *store = browser_profile_store_open(database, profiles, legacy,
                                                            legacy_runtime, &error);
    g_assert_no_error(error);
    g_assert_nonnull(store);
    BrowserProfile active = { 0 };
    g_assert_true(browser_profile_store_get_active_profile(store, &active, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(active.home_url, ==, "https://legacy.example/");
    g_assert_cmpstr(active.site_profile, ==, "desktop");

    guint next_id = 0;
    GPtrArray *loaded = browser_profile_store_load_tabs(store, active.id, &next_id, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(loaded->len, ==, 1);
    g_assert_cmpuint(next_id, ==, 3);
    g_ptr_array_unref(loaded);

    BrowserProfile second = { 0 };
    g_assert_true(browser_profile_store_create_profile(store, " 测试用户 ", &second, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(second.name, ==, "测试用户");
    int64_t second_id = second.id;
    BrowserProfile duplicate = { 0 };
    g_assert_false(browser_profile_store_create_profile(store, "测试用户", &duplicate, &error));
    g_assert_error(error, g_quark_from_static_string("browser-profile-store-error"), EEXIST);
    g_clear_error(&error);
    g_assert_true(browser_profile_store_rename_profile(store, second_id, " 第二用户 ", &error));
    g_assert_no_error(error);
    g_assert_true(browser_profile_store_set_cookie_policy(store, second.id,
        BROWSER_COOKIE_NO_THIRD_PARTY, &error));
    g_assert_no_error(error);

    GPtrArray *tabs = g_ptr_array_new_with_free_func((GDestroyNotify)browser_stored_tab_free);
    g_ptr_array_add(tabs, new_tab("https://example.com/"));
    g_assert_true(browser_profile_store_save_tabs(store, second.id, tabs, 2, &error));
    g_assert_no_error(error);
    g_ptr_array_unref(tabs);

    int64_t visit = browser_profile_store_record_visit(store, second.id,
        "https://example.com/", "Example", &error);
    g_assert_no_error(error);
    g_assert_cmpint(visit, >, 0);
    gboolean added = FALSE;
    g_assert_true(browser_profile_store_toggle_bookmark(store, second.id,
        "https://example.com/", "Example", &added, &error));
    g_assert_no_error(error);
    g_assert_true(added);
    g_assert_true(browser_profile_store_has_bookmark(store, second.id,
        "https://example.com/"));
    GPtrArray *visits = browser_profile_store_list_visits(store, second.id, 0, 6, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(visits->len, ==, 1);
    g_ptr_array_unref(visits);
    GPtrArray *bookmarks = browser_profile_store_list_bookmarks(store, second.id, 0, 6, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(bookmarks->len, ==, 1);
    g_ptr_array_unref(bookmarks);
    g_assert_true(browser_profile_store_set_active_profile(store, second.id, &error));
    g_assert_no_error(error);

    browser_profile_clear(&second);
    browser_profile_clear(&active);
    browser_profile_store_close(store);

    store = browser_profile_store_open(database, profiles, legacy, legacy_runtime, &error);
    g_assert_no_error(error);
    g_assert_nonnull(store);
    GPtrArray *all = browser_profile_store_list_profiles(store, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(all->len, ==, 2);
    g_ptr_array_unref(all);

    BrowserProfile restored = { 0 };
    g_assert_true(browser_profile_store_get_active_profile(store, &restored, &error));
    g_assert_no_error(error);
    g_assert_cmpint(restored.id, ==, second_id);
    g_assert_cmpstr(restored.name, ==, "第二用户");
    g_assert_cmpint(restored.cookie_policy, ==, BROWSER_COOKIE_NO_THIRD_PARTY);
    browser_profile_clear(&restored);

    int64_t fallback_id = 0;
    g_assert_true(browser_profile_store_prepare_delete_profile(store, second_id,
                                                              &fallback_id, &error));
    g_assert_no_error(error);
    g_assert_cmpint(fallback_id, ==, 1);
    browser_profile_store_close(store);

    store = browser_profile_store_open(database, profiles, legacy, legacy_runtime, &error);
    g_assert_no_error(error);
    g_assert_nonnull(store);
    g_assert_true(browser_profile_store_get_active_profile(store, &restored, &error));
    g_assert_no_error(error);
    g_assert_true(restored.is_default);
    browser_profile_clear(&restored);
    all = browser_profile_store_list_profiles(store, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(all->len, ==, 1);
    g_ptr_array_unref(all);

    for (int index = 0; index < 7; ++index) {
        char name[16];
        g_snprintf(name, sizeof(name), "PROFILE-%d", index + 2);
        BrowserProfile created = { 0 };
        g_assert_true(browser_profile_store_create_profile(store, name, &created, &error));
        g_assert_no_error(error);
        browser_profile_clear(&created);
    }
    g_assert_false(browser_profile_store_create_profile(store, "PROFILE-9", NULL, &error));
    g_assert_error(error, g_quark_from_static_string("browser-profile-store-error"), ENOSPC);
    g_clear_error(&error);

    for (int index = 0; index < 1005; ++index) {
        char url[64];
        g_snprintf(url, sizeof(url), "https://history.example/%d", index);
        g_assert_cmpint(browser_profile_store_record_visit(store, 1, url, "History", &error), >, 0);
        g_assert_no_error(error);
    }
    visits = browser_profile_store_list_visits(store, 1, 0, 1001, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(visits->len, ==, 1000);
    g_assert_cmpstr(((BrowserStoredPage *)g_ptr_array_index(visits, 0))->url,
                    ==, "https://history.example/1004");
    g_assert_cmpstr(((BrowserStoredPage *)g_ptr_array_index(visits, 999))->url,
                    ==, "https://history.example/5");
    g_ptr_array_unref(visits);
    browser_profile_store_close(store);

    remove_tree(root);
    g_free(database);
    g_free(profiles);
    g_free(legacy);
    g_free(legacy_runtime);
    g_free(root);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/browser/profile-store", test_profile_store);
    return g_test_run();
}
