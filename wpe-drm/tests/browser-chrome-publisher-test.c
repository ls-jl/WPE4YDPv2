#include "browser-chrome-publisher.h"
#include "ChromeSnapshot.h"
#include <assert.h>

static unsigned notifications;
static void notify(GObject *object) { assert(object); notifications++; }

int main(void)
{
    GObject *first = g_object_new(G_TYPE_OBJECT, NULL);
    GObject *second = g_object_new(G_TYPE_OBJECT, NULL);
    GKeyFile *one = g_key_file_new();
    BrowserChromePublisher publisher = { 0 };
    BrowserChromePublisher other = { 0 };
    g_key_file_set_string(one, "chrome", "panel", "tabs");
    assert(browser_chrome_publish(&publisher, first, one, NULL, TRUE, notify));
    assert(!browser_chrome_publish(&publisher, first, one, NULL, TRUE, notify));
    assert(notifications == 1);
    const WPEChromeSnapshot *snapshot = g_object_get_data(first, WPE_CHROME_SNAPSHOT_DATA_KEY);
    assert(snapshot->version == WPE_CHROME_SNAPSHOT_VERSION && snapshot->sequence == 1);
    g_key_file_set_string(one, "chrome", "panel", "settings");
    gchar *original = g_key_file_get_string(snapshot->state, "chrome", "panel", NULL);
    assert(!g_strcmp0(original, "tabs"));
    g_free(original);
    g_key_file_set_string(one, "chrome", "panel", "tabs");
    assert(!g_object_get_data(second, WPE_CHROME_SNAPSHOT_DATA_KEY));
    GKeyFile *next = g_key_file_new();
    g_key_file_set_string(next, "chrome", "panel", "menu");
    g_key_file_set_integer(next, "chrome", "height", 80);
    assert(browser_chrome_publish(&publisher, first, next, NULL, TRUE, notify));
    assert(publisher.sequence == 2);
    assert(browser_chrome_publish(&other, second, one, NULL, TRUE, notify));
    snapshot = g_object_get_data(second, WPE_CHROME_SNAPSHOT_DATA_KEY);
    gchar *panel = g_key_file_get_string(snapshot->state, "chrome", "panel", NULL);
    assert(!g_strcmp0(panel, "tabs"));
    g_free(panel);
    g_key_file_unref(one);
    g_key_file_unref(next);
    browser_chrome_publisher_clear(&publisher, first);
    assert(!g_object_get_data(first, WPE_CHROME_SNAPSHOT_DATA_KEY));
    assert(g_object_get_data(second, WPE_CHROME_SNAPSHOT_DATA_KEY));
    browser_chrome_publisher_clear(&other, second);
    g_object_unref(first);
    g_object_unref(second);
    g_print("Chrome publisher tests passed: deduplication, lifetime and view isolation\n");
    return 0;
}
