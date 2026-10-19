/* SPDX-License-Identifier: MIT */
#include "browser-chrome-publisher.h"
#include "ChromeSnapshot.h"
#include <string.h>
#include <sys/stat.h>

gboolean browser_chrome_publish(BrowserChromePublisher *publisher, GObject *view,
    GKeyFile *state, const char *diagnostic_path, gboolean structural,
    void (*notify)(GObject *))
{
    g_return_val_if_fail(publisher && G_IS_OBJECT(view) && state, FALSE);
    gsize length = 0;
    gchar *data = g_key_file_to_data(state, &length, NULL);
    if (!data)
        return FALSE;
    if (publisher->last_content && publisher->last_length == length
        && !memcmp(publisher->last_content, data, length)) {
        g_free(data);
        return FALSE;
    }
    g_free(publisher->last_content);
    publisher->last_content = data;
    publisher->last_length = length;
    if (!++publisher->sequence)
        ++publisher->sequence;
    WPEChromeSnapshot *snapshot = wpe_chrome_snapshot_new(state, publisher->sequence);
    if (!snapshot)
        return FALSE;
    g_object_set_data_full(view, WPE_CHROME_SNAPSHOT_DATA_KEY, snapshot, wpe_chrome_snapshot_free);
    if (notify)
        notify(view);
    gint64 now = g_get_monotonic_time();
    if (diagnostic_path && diagnostic_path[0]
        && (structural || now - publisher->last_diagnostic_us >= 250000)) {
        char *directory = g_path_get_dirname(diagnostic_path);
        g_mkdir_with_parents(directory, 0700);
        g_free(directory);
        if (g_file_set_contents(diagnostic_path, data, length, NULL)) {
            chmod(diagnostic_path, 0600);
            publisher->last_diagnostic_us = now;
        } else
            g_warning("Chrome diagnostic write failed");
    }
    return TRUE;
}

void browser_chrome_publisher_clear(BrowserChromePublisher *publisher, GObject *view)
{
    if (!publisher)
        return;
    if (view)
        g_object_set_data(view, WPE_CHROME_SNAPSHOT_DATA_KEY, NULL);
    g_clear_pointer(&publisher->last_content, g_free);
    publisher->last_length = 0;
}
