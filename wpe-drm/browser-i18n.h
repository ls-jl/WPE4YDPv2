#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    BROWSER_LANGUAGE_ZH_CN = 0,
    BROWSER_LANGUAGE_EN_US,
} BrowserLanguage;

BrowserLanguage browser_language_parse(const char *value);
const char *browser_language_code(BrowserLanguage language);
const char *browser_i18n(BrowserLanguage language, const char *message_id);

G_END_DECLS
