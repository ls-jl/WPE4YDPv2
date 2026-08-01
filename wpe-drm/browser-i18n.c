#include "browser-i18n.h"

#include <string.h>

typedef struct {
    const char *id;
    const char *zh_cn;
    const char *en_us;
} BrowserTranslation;

static const BrowserTranslation translations[] = {
    { "add_bookmark", "添加书签", "ADD BOOKMARK" },
    { "remove_bookmark", "移除书签", "REMOVE BOOKMARK" },
    { "history", "历史记录", "HISTORY" },
    { "bookmarks", "书签", "BOOKMARKS" },
    { "profiles", "用户配置", "PROFILES" },
    { "privacy", "隐私", "PRIVACY" },
    { "settings", "设置", "SETTINGS" },
    { "appearance", "外观", "APPEARANCE" },
    { "web_page", "网页", "WEB PAGE" },
    { "startup_search", "启动与搜索", "STARTUP & SEARCH" },
    { "privacy_data", "隐私与数据", "PRIVACY & DATA" },
    { "language", "语言", "LANGUAGE" },
    { "about", "关于", "ABOUT" },
    { "theme", "主题", "THEME" },
    { "toolbar_auto_hide", "工具栏自动隐藏", "TOOLBAR AUTO-HIDE" },
    { "page_zoom", "页面缩放", "PAGE ZOOM" },
    { "default_font", "默认字体", "DEFAULT FONT" },
    { "gpu_acceleration", "GPU 加速", "GPU ACCELERATION" },
    { "light", "浅色", "LIGHT" },
    { "dark", "深色", "DARK" },
    { "small", "小", "SMALL" },
    { "standard", "标准", "STANDARD" },
    { "large", "大", "LARGE" },
    { "site_mode", "网页模式", "SITE MODE" },
    { "javascript", "JavaScript", "JAVASCRIPT" },
    { "autoplay", "自动播放", "AUTOPLAY" },
    { "smooth_scroll", "平滑滚动", "SMOOTH SCROLL" },
    { "block_popups", "阻止弹窗", "BLOCK POPUPS" },
    { "desktop", "电脑模式", "DESKTOP" },
    { "mobile", "手机模式", "MOBILE" },
    { "user_action", "需要用户操作", "USER ACTION" },
    { "allow", "允许", "ALLOW" },
    { "search_engine", "搜索引擎", "SEARCH ENGINE" },
    { "custom_search", "自定义搜索模板", "CUSTOM SEARCH TEMPLATE" },
    { "home_page", "主页", "HOME PAGE" },
    { "restore_tabs", "恢复标签页", "RESTORE TABS" },
    { "set", "已设置", "SET" },
    { "not_set", "未设置", "NOT SET" },
    { "cookie_policy", "Cookie 策略", "COOKIE POLICY" },
    { "clear_site_data", "清除 Cookie 与站点数据", "CLEAR COOKIES & SITE DATA" },
    { "clearing_site_data", "正在清除站点数据...", "CLEARING SITE DATA..." },
    { "clear_cache", "清除缓存", "CLEAR CACHE" },
    { "clear_history", "清除历史记录", "CLEAR HISTORY" },
    { "site_data_cleared", "站点数据已清除", "SITE DATA CLEARED" },
    { "clear_failed", "清除失败", "CLEAR FAILED" },
    { "cache_cleared", "缓存已清除", "CACHE CLEARED" },
    { "cancel", "取消", "CANCEL" },
    { "confirm_clear_site_data", "确认清除 Cookie 与站点数据", "CONFIRM CLEAR COOKIES & SITE DATA" },
    { "confirm_clear_history", "确认清除历史记录", "CONFIRM CLEAR HISTORY" },
    { "confirm_delete_profile", "确认删除用户配置", "CONFIRM DELETE PROFILE" },
    { "guest", "访客", "GUEST" },
    { "default_profile", "默认", "DEFAULT" },
    { "new_profile", "新建用户配置", "NEW PROFILE" },
    { "previous", "上一页", "PREV" },
    { "next", "下一页", "NEXT" },
    { "manage_active", "管理当前用户", "MANAGE ACTIVE" },
    { "rename", "重命名", "RENAME" },
    { "delete_profile", "删除用户配置", "DELETE PROFILE" },
    { "delete_default_disabled", "默认用户不可删除", "DELETE DISABLED (DEFAULT)" },
    { "delete", "删除", "DELETE" },
    { "delete_mode", "删除模式", "DELETE MODE" },
    { "done_deleting", "完成删除", "DONE DELETING" },
    { "previous_page", "上一页", "PREV PAGE" },
    { "next_page", "下一页", "NEXT PAGE" },
    { "browser_name", "Direct WPE DRM 浏览器", "DIRECT WPE DRM BROWSER" },
    { "webkit_runtime", "WebKit 运行时", "WEBKIT RUNTIME" },
    { "current_profile", "当前用户", "CURRENT PROFILE" },
    { "renderer", "渲染器", "RENDERER" },
    { "display", "显示", "DISPLAY" },
    { "language_zh_cn", "简体中文", "SIMPLIFIED CHINESE" },
    { "language_en_us", "English", "ENGLISH" },
    { "privacy_cookies", "隐私与 Cookie", "PRIVACY & COOKIES" },
    { "clear_site_data_title", "清除站点数据？", "CLEAR SITE DATA?" },
    { "profile", "用户配置", "PROFILE" },
    { "delete_profile_title", "删除用户配置？", "DELETE PROFILE?" },
    { "clear_history_title", "清除历史记录？", "CLEAR HISTORY?" },
    { "tabs", "标签页", "TABS" },
    { "home", "主页", "HOME" },
    { "touch_debug", "触摸调试", "TOUCH DEBUG" },
    { "address_placeholder", "输入网址或搜索", "ENTER URL OR SEARCH" },
    { "input_placeholder", "请输入内容", "ENTER TEXT" },
    { "home_url_placeholder", "设置主页 URL", "SET HOME PAGE URL" },
    { "custom_search_placeholder", "HTTPS 搜索模板，使用一个 %s", "HTTPS SEARCH TEMPLATE WITH ONE %s" },
    { "new_profile_placeholder", "新建用户配置名称", "NEW PROFILE NAME" },
    { "rename_profile_placeholder", "重命名用户配置", "RENAME PROFILE" },
    { "all", "全部接受", "ALL" },
    { "no_third_party", "拒绝第三方", "NO THIRD PARTY" },
    { "never", "全部拒绝", "NEVER" },
};

BrowserLanguage browser_language_parse(const char *value)
{
    if (value && (!g_ascii_strcasecmp(value, "en-US")
            || !g_ascii_strcasecmp(value, "en_US")
            || !g_ascii_strcasecmp(value, "en")))
        return BROWSER_LANGUAGE_EN_US;
    return BROWSER_LANGUAGE_ZH_CN;
}

const char *browser_language_code(BrowserLanguage language)
{
    return language == BROWSER_LANGUAGE_EN_US ? "en-US" : "zh-CN";
}

const char *browser_i18n(BrowserLanguage language, const char *message_id)
{
    if (!message_id)
        return "";
    for (guint index = 0; index < G_N_ELEMENTS(translations); ++index) {
        if (!strcmp(translations[index].id, message_id))
            return language == BROWSER_LANGUAGE_EN_US
                ? translations[index].en_us : translations[index].zh_cn;
    }
    return message_id;
}
