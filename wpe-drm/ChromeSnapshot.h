/* SPDX-License-Identifier: MIT */
#pragma once
#include <glib.h>

#define WPE_CHROME_SNAPSHOT_VERSION 1
#define WPE_CHROME_SNAPSHOT_DATA_KEY "wpe-chrome-snapshot-v1"
typedef struct {
    guint32 version;
    guint64 sequence;
    GKeyFile *state;
} WPEChromeSnapshot;

static inline WPEChromeSnapshot *wpe_chrome_snapshot_new(GKeyFile *state, guint64 sequence)
{
    WPEChromeSnapshot *snapshot = g_new0(WPEChromeSnapshot, 1);
    snapshot->version = WPE_CHROME_SNAPSHOT_VERSION;
    snapshot->sequence = sequence;
    gsize length = 0;
    gchar *data = g_key_file_to_data(state, &length, NULL);
    snapshot->state = g_key_file_new();
    if (!data || !g_key_file_load_from_data(snapshot->state, data, length, G_KEY_FILE_NONE, NULL)) {
        g_free(data);
        g_key_file_unref(snapshot->state);
        g_free(snapshot);
        return NULL;
    }
    g_free(data);
    return snapshot;
}
static inline void wpe_chrome_snapshot_free(gpointer data)
{
    WPEChromeSnapshot *snapshot = (WPEChromeSnapshot *)data;
    if (!snapshot)
        return;
    g_key_file_unref(snapshot->state);
    g_free(snapshot);
}
