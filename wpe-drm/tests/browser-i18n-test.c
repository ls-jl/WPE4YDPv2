#include "../browser-i18n.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    assert(browser_language_parse(NULL) == BROWSER_LANGUAGE_ZH_CN);
    assert(browser_language_parse("zh-CN") == BROWSER_LANGUAGE_ZH_CN);
    assert(browser_language_parse("en-US") == BROWSER_LANGUAGE_EN_US);
    assert(!strcmp(browser_language_code(BROWSER_LANGUAGE_ZH_CN), "zh-CN"));
    assert(!strcmp(browser_language_code(BROWSER_LANGUAGE_EN_US), "en-US"));
    assert(!strcmp(browser_i18n(BROWSER_LANGUAGE_ZH_CN, "settings"), "设置"));
    assert(!strcmp(browser_i18n(BROWSER_LANGUAGE_EN_US, "settings"), "SETTINGS"));
    assert(!strcmp(browser_i18n(BROWSER_LANGUAGE_EN_US, "unknown"), "unknown"));
    return 0;
}
