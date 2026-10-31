/* SPDX-License-Identifier: MIT */
#pragma once
#include <glib-object.h>

typedef struct {
    guint64 sequence;
    gchar *last_content;
    gsize last_length;
    gint64 last_diagnostic_us;
} BrowserChromePublisher;

gboolean browser_chrome_publish(BrowserChromePublisher *, GObject *view,
    GKeyFile *state, const char *diagnostic_path, gboolean structural,
    void (*notify)(GObject *));
void browser_chrome_publisher_clear(BrowserChromePublisher *, GObject *view);
