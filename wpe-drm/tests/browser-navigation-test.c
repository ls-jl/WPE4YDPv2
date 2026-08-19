#include "../browser-navigation.h"

static void test_navigation(void)
{
    char *scheme = browser_navigation_uri_scheme("BaIdUbOxApP://open");
    g_assert_cmpstr(scheme, ==, "baiduboxapp");
    g_assert_false(browser_navigation_scheme_allowed(scheme));
    g_free(scheme);

    g_assert_true(browser_navigation_scheme_allowed("https"));
    g_assert_true(browser_navigation_scheme_allowed("file"));
    g_assert_false(browser_navigation_scheme_allowed("intent"));

    char *bilibili = browser_navigation_bilibili_web_url(
        "bilibili://video/av170001");
    g_assert_cmpstr(bilibili, ==,
                    "https://www.bilibili.com/video/av170001/");
    g_free(bilibili);

    g_assert_true(browser_navigation_custom_search_template_valid(
        "https://search.example/?q=%s"));
    g_assert_false(browser_navigation_custom_search_template_valid(
        "http://search.example/?q=%s"));
    g_assert_false(browser_navigation_custom_search_template_valid(
        "https://search.example/?q=%s&again=%s"));

    char *search = browser_navigation_search_url("bing", NULL, "WPE 浏览器");
    g_assert_true(g_str_has_prefix(search, "https://www.bing.com/search?q="));
    g_assert_nonnull(strstr(search, "WPE%20"));
    g_assert_nonnull(strstr(search, "%E6"));
    g_free(search);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/browser/navigation/policy", test_navigation);
    return g_test_run();
}
