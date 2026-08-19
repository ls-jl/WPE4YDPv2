#pragma once

#include <glib.h>

G_BEGIN_DECLS

char *browser_navigation_uri_scheme(const char *uri);
gboolean browser_navigation_scheme_allowed(const char *scheme);
char *browser_navigation_bilibili_web_url(const char *uri);
gboolean browser_navigation_custom_search_template_valid(const char *value);
char *browser_navigation_search_url(const char *engine,
                                    const char *custom_template,
                                    const char *query);

G_END_DECLS
