#include "browser-navigation.h"

#include <string.h>

char *browser_navigation_uri_scheme(const char *uri)
{
    if (!uri || !g_ascii_isalpha(uri[0]))
        return NULL;

    const char *position = uri + 1;
    while (*position) {
        if (*position == ':')
            return g_ascii_strdown(uri, position - uri);
        if (*position == '/' || *position == '?' || *position == '#'
                || g_ascii_isspace(*position))
            return NULL;
        if (!(g_ascii_isalnum(*position) || *position == '+'
                || *position == '-' || *position == '.'))
            return NULL;
        position++;
    }
    return NULL;
}

gboolean browser_navigation_scheme_allowed(const char *scheme)
{
    if (!scheme || !scheme[0])
        return TRUE;
    return !g_ascii_strcasecmp(scheme, "http")
        || !g_ascii_strcasecmp(scheme, "https")
        || !g_ascii_strcasecmp(scheme, "about")
        || !g_ascii_strcasecmp(scheme, "file");
}

static gboolean is_bvid_char(char character)
{
    return g_ascii_isalnum(character);
}

char *browser_navigation_bilibili_web_url(const char *uri)
{
    if (!uri || !uri[0])
        return NULL;

    const char *bvid = NULL;
    for (const char *position = uri; *position; ++position) {
        if (position[0] == 'B' && position[1] == 'V'
                && is_bvid_char(position[2])) {
            bvid = position;
            break;
        }
    }
    if (bvid) {
        const char *end = bvid;
        while (*end && is_bvid_char(*end) && end - bvid < 32)
            end++;
        if (end - bvid >= 4) {
            char *identifier = g_strndup(bvid, end - bvid);
            char *url = g_strdup_printf("https://www.bilibili.com/video/%s/",
                                        identifier);
            g_free(identifier);
            return url;
        }
    }

    const char *prefix = "bilibili://video/";
    if (!g_ascii_strncasecmp(uri, prefix, strlen(prefix))) {
        const char *identifier = uri + strlen(prefix);
        while (*identifier && !g_ascii_isdigit(*identifier))
            identifier++;
        const char *end = identifier;
        while (*end && g_ascii_isdigit(*end))
            end++;
        if (end > identifier) {
            char *avid = g_strndup(identifier, end - identifier);
            char *url = g_strdup_printf("https://www.bilibili.com/video/av%s/",
                                        avid);
            g_free(avid);
            return url;
        }
    }
    return NULL;
}

gboolean browser_navigation_custom_search_template_valid(const char *value)
{
    if (!value || !g_str_has_prefix(value, "https://"))
        return FALSE;
    const char *placeholder = strstr(value, "%s");
    if (!placeholder || strstr(placeholder + 2, "%s")
            || strpbrk(value, "\r\n\t"))
        return FALSE;

    char *candidate = g_strdup_printf("%.*squery%s",
        (int)(placeholder - value), value, placeholder + 2);
    GError *error = NULL;
    GUri *uri = g_uri_parse(candidate, G_URI_FLAGS_NONE, &error);
    gboolean valid = uri && g_uri_get_scheme(uri)
        && !g_ascii_strcasecmp(g_uri_get_scheme(uri), "https")
        && g_uri_get_host(uri) && g_uri_get_host(uri)[0];
    if (uri)
        g_uri_unref(uri);
    g_clear_error(&error);
    g_free(candidate);
    return valid;
}

char *browser_navigation_search_url(const char *engine,
                                    const char *custom_template,
                                    const char *query)
{
    const char *format = "https://m.baidu.com/s?word=%s";
    if (!g_strcmp0(engine, "bing"))
        format = "https://www.bing.com/search?q=%s";
    else if (!g_strcmp0(engine, "google"))
        format = "https://www.google.com/search?q=%s";
    else if (!g_strcmp0(engine, "custom")
            && browser_navigation_custom_search_template_valid(custom_template))
        format = custom_template;

    const char *placeholder = strstr(format, "%s");
    char *escaped = g_uri_escape_string(query ? query : "", NULL, FALSE);
    char *url = g_strdup_printf("%.*s%s%s",
        (int)(placeholder - format), format, escaped ? escaped : "",
        placeholder + 2);
    g_free(escaped);
    return url;
}
