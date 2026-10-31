/*
 * Copyright (C) 2023 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ChromeRenderer.h"
#include "ChromeSnapshot.h"

struct ChromeGlyph {
    int width { 0 };
    int height { 0 };
    int left { 0 };
    int top { 0 };
    int advance { 0 };
    uint64_t lastUsed { 0 };
    std::vector<uint8_t> pixels;
};

enum class ChromeFontWeight : uint8_t {
    Regular = 0,
    Medium = 1,
    Bold = 2,
};

class ChromeFontCache {
public:
    static ChromeFontCache& singleton()
    {
        static ChromeFontCache cache;
        return cache;
    }

    const ChromeGlyph* glyph(gunichar character,
                             ChromeFontWeight weight = ChromeFontWeight::Regular)
    {
        auto& font = m_fonts[static_cast<size_t>(weight)];
        if (!font.face)
            return nullptr;
        auto found = font.glyphs.find(character);
        if (found != font.glyphs.end()) {
            found->second.lastUsed = ++m_generation;
            return &found->second;
        }
        if (FT_Load_Char(font.face, character, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT))
            return nullptr;
        auto& bitmap = font.face->glyph->bitmap;
        ChromeGlyph glyph;
        glyph.width = bitmap.width;
        glyph.height = bitmap.rows;
        glyph.left = font.face->glyph->bitmap_left;
        glyph.top = font.face->glyph->bitmap_top;
        glyph.advance = std::max<int>(1, font.face->glyph->advance.x >> 6);
        glyph.lastUsed = ++m_generation;
        glyph.pixels.resize(static_cast<size_t>(glyph.width) * glyph.height);
        for (int row = 0; row < glyph.height; ++row) {
            const uint8_t* source = bitmap.buffer + static_cast<ptrdiff_t>(row) * bitmap.pitch;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
                memcpy(glyph.pixels.data() + static_cast<size_t>(row) * glyph.width, source, glyph.width);
            else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                for (int column = 0; column < glyph.width; ++column)
                    glyph.pixels[static_cast<size_t>(row) * glyph.width + column] =
                        source[column / 8] & (0x80 >> (column % 8)) ? 255 : 0;
            }
        }
        if (font.glyphs.size() >= maximumGlyphsPerFont)
            evictOldest(font);
        return &font.glyphs.emplace(character, std::move(glyph)).first->second;
    }

    bool available(ChromeFontWeight weight = ChromeFontWeight::Regular) const
    {
        return m_fonts[static_cast<size_t>(weight)].face;
    }

    size_t bytes() const
    {
        size_t total = 0;
        for (const auto& font : m_fonts) {
            for (const auto& entry : font.glyphs)
                total += sizeof(entry) + entry.second.pixels.capacity();
        }
        return total;
    }

    void trim(bool critical)
    {
        for (auto& font : m_fonts) {
            size_t target = critical ? 0 : warningGlyphsPerFont;
            while (font.glyphs.size() > target)
                evictOldest(font);
            if (critical)
                font.glyphs.rehash(0);
        }
    }

private:
    static constexpr size_t maximumGlyphsPerFont = 512;
    static constexpr size_t warningGlyphsPerFont = 256;

    struct Font {
        FT_Face face { nullptr };
        std::unordered_map<gunichar, ChromeGlyph> glyphs;
    };

    static void evictOldest(Font& font)
    {
        if (font.glyphs.empty())
            return;
        auto oldest = font.glyphs.begin();
        for (auto it = std::next(font.glyphs.begin()); it != font.glyphs.end(); ++it) {
            if (it->second.lastUsed < oldest->second.lastUsed)
                oldest = it;
        }
        font.glyphs.erase(oldest);
    }

    ChromeFontCache()
    {
        if (FT_Init_FreeType(&m_library)) {
            m_library = nullptr;
            g_warning("WPEViewDRM chrome_font=ascii-fallback freetype_init_failed");
            return;
        }
        const char* regular = g_getenv("WPE_CHROME_FONT");
        const char* paths[] = {
            regular,
            g_getenv("WPE_CHROME_FONT_MEDIUM"),
            g_getenv("WPE_CHROME_FONT_BOLD"),
        };
        const char* names[] = { "regular", "medium", "bold" };
        for (size_t index = 0; index < m_fonts.size(); ++index) {
            const char* path = paths[index] && *paths[index] ? paths[index] : regular;
            if (!path || !*path
                    || FT_New_Face(m_library, path, 0, &m_fonts[index].face)
                    || FT_Set_Pixel_Sizes(m_fonts[index].face, 0, 13)) {
                if (m_fonts[index].face)
                    FT_Done_Face(m_fonts[index].face);
                m_fonts[index].face = nullptr;
                g_warning("WPEViewDRM chrome_font_weight=%s unavailable path=%s",
                          names[index], path ? path : "(unset)");
                continue;
            }
            g_message("WPEViewDRM chrome_font_weight=%s path=%s family=%s",
                      names[index], path,
                      m_fonts[index].face->family_name
                        ? m_fonts[index].face->family_name : "unknown");
        }
    }

    ~ChromeFontCache()
    {
        for (auto& font : m_fonts) {
            if (font.face)
                FT_Done_Face(font.face);
        }
        if (m_library)
            FT_Done_FreeType(m_library);
    }

    FT_Library m_library { nullptr };
    std::array<Font, 3> m_fonts;
    uint64_t m_generation { 0 };
};

bool chromeEnabled()
{
    static bool enabled = []() {
        const char* value = getenv("WPE_CHROME_ENABLED");
        if (!value || !*value)
            return true;
        return strcmp(value, "0") && g_ascii_strcasecmp(value, "false") && g_ascii_strcasecmp(value, "off");
    }();
    return enabled;
}

long chromeAnimationDurationMS()
{
    static long durationMS = []() -> long {
        const char* value = getenv("WPE_CHROME_ANIMATION_MS");
        char* end = nullptr;
        auto parsed = value && *value ? strtol(value, &end, 10) : 160;
        if (end == value || parsed <= 0)
            parsed = 160;
        return parsed;
    }();
    return durationMS;
}

static void copyKeyString(GKeyFile* keyFile, const char* group, const char* key, char* target, size_t targetSize)
{
    GError* error = nullptr;
    char* value = g_key_file_get_string(keyFile, group, key, &error);
    if (!value) {
        g_clear_error(&error);
        return;
    }
    g_strlcpy(target, value, targetSize);
    g_free(value);
}

static ChromeRenderState readChromeRenderState(GKeyFile* keyFile)
{
    ChromeRenderState state;
    for (unsigned line = 0; line < 10; ++line) {
        state.lineEnabled[line] = true;
        state.lineSegmentCount[line] = 1;
        for (unsigned segment = 0; segment < 3; ++segment)
            state.lineSegmentEnabled[line][segment] = true;
    }
    state.enabled = chromeEnabled();
    if (!state.enabled)
        return state;

    if (!keyFile)
        return state;

    if (g_key_file_has_key(keyFile, "chrome", "enabled", nullptr))
        state.enabled = g_key_file_get_boolean(keyFile, "chrome", "enabled", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "visible", nullptr))
        state.visible = g_key_file_get_boolean(keyFile, "chrome", "visible", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "loading", nullptr))
        state.loading = g_key_file_get_boolean(keyFile, "chrome", "loading", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "load_progress", nullptr))
        state.loadProgress = std::clamp(g_key_file_get_double(keyFile, "chrome", "load_progress", nullptr), 0.0, 1.0);
    if (g_key_file_has_key(keyFile, "chrome", "can_back", nullptr))
        state.canBack = g_key_file_get_boolean(keyFile, "chrome", "can_back", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "can_forward", nullptr))
        state.canForward = g_key_file_get_boolean(keyFile, "chrome", "can_forward", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "touch_debug", nullptr))
        state.touchDebug = g_key_file_get_boolean(keyFile, "chrome", "touch_debug", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "height", nullptr))
        state.height = std::clamp<int>(g_key_file_get_integer(keyFile, "chrome", "height", nullptr), 24, 80);
    if (g_key_file_has_key(keyFile, "chrome", "toolbar_stacked", nullptr))
        state.toolbarStacked = g_key_file_get_boolean(keyFile, "chrome", "toolbar_stacked", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "address_x", nullptr))
        state.addressX = g_key_file_get_integer(keyFile, "chrome", "address_x", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "address_y", nullptr))
        state.addressY = g_key_file_get_integer(keyFile, "chrome", "address_y", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "address_width", nullptr))
        state.addressWidth = g_key_file_get_integer(keyFile, "chrome", "address_width", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "address_height", nullptr))
        state.addressHeight = g_key_file_get_integer(keyFile, "chrome", "address_height", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "controls_x", nullptr))
        state.controlsX = g_key_file_get_integer(keyFile, "chrome", "controls_x", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "controls_y", nullptr))
        state.controlsY = g_key_file_get_integer(keyFile, "chrome", "controls_y", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "controls_width", nullptr))
        state.controlsWidth = g_key_file_get_integer(keyFile, "chrome", "controls_width", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "controls_height", nullptr))
        state.controlsHeight = g_key_file_get_integer(keyFile, "chrome", "controls_height", nullptr);
    if (g_key_file_has_key(keyFile, "chrome", "button_count", nullptr))
        state.buttonCount = std::clamp(
            g_key_file_get_integer(keyFile, "chrome", "button_count", nullptr), 1, 8);
    if (g_key_file_has_key(keyFile, "chrome", "transition_us", nullptr)) {
        char* value = g_key_file_get_string(keyFile, "chrome", "transition_us", nullptr);
        if (value) {
            char* end = nullptr;
            auto parsed = g_ascii_strtoll(value, &end, 10);
            if (end != value)
                state.transitionUS = parsed;
            g_free(value);
        }
    }
    if (g_key_file_has_key(keyFile, "chrome", "tab_count", nullptr))
        state.tabCount = std::max<int>(1, g_key_file_get_integer(keyFile, "chrome", "tab_count", nullptr));
    if (g_key_file_has_key(keyFile, "chrome", "active_tab", nullptr))
        state.activeTab = std::max<int>(0, g_key_file_get_integer(keyFile, "chrome", "active_tab", nullptr));

    copyKeyString(keyFile, "chrome", "panel", state.panel, sizeof(state.panel));
    copyKeyString(keyFile, "chrome", "theme", state.theme, sizeof(state.theme));
    copyKeyString(keyFile, "chrome", "url", state.url, sizeof(state.url));
    copyKeyString(keyFile, "chrome", "title", state.title, sizeof(state.title));
    copyKeyString(keyFile, "chrome", "touch_debug_label",
                  state.touchDebugLabel, sizeof(state.touchDebugLabel));
    copyKeyString(keyFile, "chrome", "home_label",
                  state.homeLabel, sizeof(state.homeLabel));

    if (g_key_file_has_group(keyFile, "panel")) {
        copyKeyString(keyFile, "panel", "title", state.panelTitle,
                      sizeof(state.panelTitle));
        auto count = std::clamp<int>(g_key_file_get_integer(keyFile, "panel", "line_count", nullptr), 0, 10);
        state.lineCount = count;
        for (int i = 0; i < count; ++i) {
            char key[16];
            snprintf(key, sizeof(key), "line%d", i);
            copyKeyString(keyFile, "panel", key, state.lines[i], sizeof(state.lines[i]));
        }
        if (g_key_file_has_key(keyFile, "panel", "x", nullptr))
            state.panelX = g_key_file_get_integer(keyFile, "panel", "x", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "y", nullptr))
            state.panelY = g_key_file_get_integer(keyFile, "panel", "y", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "width", nullptr))
            state.panelWidth = g_key_file_get_integer(keyFile, "panel", "width", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "height", nullptr))
            state.panelHeight = g_key_file_get_integer(keyFile, "panel", "height", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "header_height", nullptr))
            state.panelHeaderHeight = g_key_file_get_integer(keyFile, "panel", "header_height", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "footer_height", nullptr))
            state.panelFooterHeight = g_key_file_get_integer(keyFile, "panel", "footer_height", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "row_height", nullptr))
            state.panelRowHeight = g_key_file_get_integer(keyFile, "panel", "row_height", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "scroll_offset", nullptr))
            state.panelScrollOffset = std::max(0.0, g_key_file_get_double(keyFile, "panel", "scroll_offset", nullptr));
        if (g_key_file_has_key(keyFile, "panel", "max_scroll", nullptr))
            state.panelMaxScroll = std::max(0.0, g_key_file_get_double(keyFile, "panel", "max_scroll", nullptr));
        if (g_key_file_has_key(keyFile, "panel", "can_add", nullptr))
            state.panelCanAdd = g_key_file_get_boolean(keyFile, "panel", "can_add", nullptr);
        if (g_key_file_has_key(keyFile, "panel", "columns", nullptr))
            state.panelColumns = std::clamp(g_key_file_get_integer(keyFile, "panel", "columns", nullptr), 1, 6);
        if (g_key_file_has_key(keyFile, "panel", "rows", nullptr))
            state.panelRows = std::clamp(g_key_file_get_integer(keyFile, "panel", "rows", nullptr), 1, 4);
        if (g_key_file_has_key(keyFile, "panel", "page_count", nullptr))
            state.panelPageCount = std::max(1, g_key_file_get_integer(keyFile, "panel", "page_count", nullptr));
        for (int i = 0; i < 10; ++i) {
            char key[32];
            snprintf(key, sizeof(key), "line%d_enabled", i);
            if (g_key_file_has_key(keyFile, "panel", key, nullptr))
                state.lineEnabled[i] = g_key_file_get_boolean(keyFile, "panel", key, nullptr);
            snprintf(key, sizeof(key), "line%d_danger", i);
            if (g_key_file_has_key(keyFile, "panel", key, nullptr))
                state.lineDanger[i] = g_key_file_get_boolean(keyFile, "panel", key, nullptr);
            snprintf(key, sizeof(key), "line%d_checked", i);
            if (g_key_file_has_key(keyFile, "panel", key, nullptr))
                state.lineChecked[i] = g_key_file_get_boolean(keyFile, "panel", key, nullptr);
            snprintf(key, sizeof(key), "line%d_kind", i);
            copyKeyString(keyFile, "panel", key,
                          state.lineKinds[i], sizeof(state.lineKinds[i]));
            snprintf(key, sizeof(key), "line%d_value", i);
            copyKeyString(keyFile, "panel", key,
                          state.lineValues[i], sizeof(state.lineValues[i]));
            snprintf(key, sizeof(key), "line%d_icon", i);
            copyKeyString(keyFile, "panel", key,
                          state.lineIcons[i], sizeof(state.lineIcons[i]));
            snprintf(key, sizeof(key), "line%d_segment_count", i);
            if (g_key_file_has_key(keyFile, "panel", key, nullptr))
                state.lineSegmentCount[i] = std::clamp(
                    g_key_file_get_integer(keyFile, "panel", key, nullptr), 1, 3);
            for (int segment = 0; segment < 3; ++segment) {
                snprintf(key, sizeof(key), "line%d_segment%d_label", i, segment);
                copyKeyString(keyFile, "panel", key,
                              state.lineSegmentLabels[i][segment],
                              sizeof(state.lineSegmentLabels[i][segment]));
                snprintf(key, sizeof(key), "line%d_segment%d_enabled", i, segment);
                if (g_key_file_has_key(keyFile, "panel", key, nullptr))
                    state.lineSegmentEnabled[i][segment] =
                        g_key_file_get_boolean(keyFile, "panel", key, nullptr);
                snprintf(key, sizeof(key), "line%d_segment%d_danger", i, segment);
                if (g_key_file_has_key(keyFile, "panel", key, nullptr))
                    state.lineSegmentDanger[i][segment] =
                        g_key_file_get_boolean(keyFile, "panel", key, nullptr);
            }
            snprintf(key, sizeof(key), "item%d_id", i);
            copyKeyString(keyFile, "panel", key, state.itemIDs[i], sizeof(state.itemIDs[i]));
            snprintf(key, sizeof(key), "item%d_enabled", i);
            if (g_key_file_has_key(keyFile, "panel", key, nullptr))
                state.itemEnabled[i] = g_key_file_get_boolean(keyFile, "panel", key, nullptr);
        }
    }

    return state;
}

static thread_local ChromeStateCache* activeChromeCache = nullptr;

ChromeRenderScope::ChromeRenderScope(ChromeStateCache& cache)
    : m_previous(activeChromeCache)
{
    activeChromeCache = &cache;
}

ChromeRenderScope::~ChromeRenderScope()
{
    activeChromeCache = m_previous;
}

ChromeStateCache& chromeStateCache()
{
    static thread_local ChromeStateCache defaults;
    return activeChromeCache ? *activeChromeCache : defaults;
}

bool refreshChromeRenderState(WPEView* view)
{
    auto& cache = chromeStateCache();
    auto* snapshot = static_cast<const WPEChromeSnapshot*>(
        g_object_get_data(G_OBJECT(view), WPE_CHROME_SNAPSHOT_DATA_KEY));
    if (!snapshot || snapshot->version != WPE_CHROME_SNAPSHOT_VERSION
        || snapshot->sequence == cache.snapshotSequence)
        return false;
    cache.state = readChromeRenderState(snapshot->state);
    cache.snapshotSequence = snapshot->sequence;
    cache.valid = true;
    return true;
}

const ChromeRenderState& cachedChromeRenderState()
{
    auto& cache = chromeStateCache();
    if (!cache.valid) {
        cache.state.enabled = chromeEnabled();
        cache.valid = true;
    }
    return cache.state;
}

bool nativeChromeCompositionRequired()
{
    // A direct DMA-BUF cannot be modified in place. Keep the full-panel dumb
    // composition path alive whenever native chrome is enabled so a toolbar or
    // menu can be shown without waiting for (or reverting to) a stale WebKit
    // frame. This also restores the toolbar for absolute rotation 0.
    return cachedChromeRenderState().enabled;
}

double chromeShownFraction(const ChromeRenderState& chrome)
{
    double fraction = chrome.visible ? 1.0 : 0.0;
    if (chrome.transitionUS <= 0)
        return fraction;
    auto durationMS = chromeAnimationDurationMS();
    auto elapsedUS = std::max<gint64>(0, g_get_monotonic_time() - chrome.transitionUS);
    double t = std::min<double>(1.0, static_cast<double>(elapsedUS) / (durationMS * 1000.0));
    double eased = 1.0 - std::pow(1.0 - t, 3.0);
    return chrome.visible ? eased : (1.0 - eased);
}

uint32_t chromeReservedTopInset(uint32_t panelHeight)
{
    if (!panelHeight)
        return 0;
    const auto& chrome = cachedChromeRenderState();
    if (!chrome.enabled)
        return 0;
    auto chromeHeight = std::min<uint32_t>(std::clamp<unsigned>(chrome.height, 24, 80), panelHeight - 1);
    bool hasPanel = chrome.panel[0] && strcmp(chrome.panel, "none");
    return (chrome.visible || hasPanel) ? chromeHeight : 0;
}

// The render-state file and WebKit's resized frame are published through
// independent event loops. During a toolbar toggle, a 960x222 content frame
// can therefore arrive while the DRM side still has the previous visible
// state. In resize mode the frame height is authoritative: a frame shorter by
// exactly one toolbar height belongs below that toolbar, while a panel-height
// frame starts at y=0. This prevents a short frame from being placed at the
// top and leaving a toolbar-sized black strip at the opposite edge.
uint32_t chromeFrameTopInset(uint32_t panelHeight, uint32_t sourceHeight)
{
    auto stateInset = chromeReservedTopInset(panelHeight);
    if (!panelHeight)
        return stateInset;

    uint32_t frameInset = 0;
    const auto& chrome = cachedChromeRenderState();
    bool panelOpen = chrome.panel[0] && strcmp(chrome.panel, "none");
    if (sourceHeight >= panelHeight)
        return stateInset; // Reserve space immediately while a resized frame is pending.
    if (sourceHeight < panelHeight) {
        auto chromeHeight = std::min<uint32_t>(
            std::clamp<unsigned>(chrome.height, 24, 80), panelHeight - 1);
        auto missingHeight = panelHeight - sourceHeight;
        if (missingHeight + 2 >= chromeHeight
            && missingHeight <= chromeHeight + 2)
            frameInset = std::min(missingHeight, chromeHeight);
        else
            return stateInset;
    }
    if (!chrome.visible && !panelOpen && !chromeAnimationActive())
        return 0; // Move the old short frame up; extend its final row until full height arrives.

    if (frameInset != stateInset) {
        static uint32_t lastStateInset = UINT32_MAX;
        static uint32_t lastFrameInset = UINT32_MAX;
        static uint32_t lastSourceHeight = UINT32_MAX;
        if (stateInset != lastStateInset || frameInset != lastFrameInset
            || sourceHeight != lastSourceHeight) {
            g_message("WPEViewDRM chrome frame geometry reconciled: state_inset=%u frame_inset=%u source_height=%u panel_height=%u",
                stateInset, frameInset, sourceHeight, panelHeight);
            lastStateInset = stateInset;
            lastFrameInset = frameInset;
            lastSourceHeight = sourceHeight;
        }
    }
    return frameInset;
}

bool chromeAnimationActive()
{
    const auto& chrome = cachedChromeRenderState();
    if (!chrome.enabled || chrome.transitionUS <= 0)
        return false;
    return g_get_monotonic_time() - chrome.transitionUS < chromeAnimationDurationMS() * 1000;
}

static void fontRows(char c, uint8_t rows[7])
{
    memset(rows, 0, 7);
    switch (g_ascii_toupper(c)) {
    case '0': { uint8_t v[7] = { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e }; memcpy(rows, v, 7); return; }
    case '1': { uint8_t v[7] = { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }; memcpy(rows, v, 7); return; }
    case '2': { uint8_t v[7] = { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f }; memcpy(rows, v, 7); return; }
    case '3': { uint8_t v[7] = { 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e }; memcpy(rows, v, 7); return; }
    case '4': { uint8_t v[7] = { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }; memcpy(rows, v, 7); return; }
    case '5': { uint8_t v[7] = { 0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e }; memcpy(rows, v, 7); return; }
    case '6': { uint8_t v[7] = { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }; memcpy(rows, v, 7); return; }
    case '7': { uint8_t v[7] = { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }; memcpy(rows, v, 7); return; }
    case '8': { uint8_t v[7] = { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e }; memcpy(rows, v, 7); return; }
    case '9': { uint8_t v[7] = { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }; memcpy(rows, v, 7); return; }
    case 'A': { uint8_t v[7] = { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }; memcpy(rows, v, 7); return; }
    case 'B': { uint8_t v[7] = { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e }; memcpy(rows, v, 7); return; }
    case 'C': { uint8_t v[7] = { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e }; memcpy(rows, v, 7); return; }
    case 'D': { uint8_t v[7] = { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e }; memcpy(rows, v, 7); return; }
    case 'E': { uint8_t v[7] = { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f }; memcpy(rows, v, 7); return; }
    case 'F': { uint8_t v[7] = { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 }; memcpy(rows, v, 7); return; }
    case 'G': { uint8_t v[7] = { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f }; memcpy(rows, v, 7); return; }
    case 'H': { uint8_t v[7] = { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }; memcpy(rows, v, 7); return; }
    case 'I': { uint8_t v[7] = { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e }; memcpy(rows, v, 7); return; }
    case 'J': { uint8_t v[7] = { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e }; memcpy(rows, v, 7); return; }
    case 'K': { uint8_t v[7] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }; memcpy(rows, v, 7); return; }
    case 'L': { uint8_t v[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f }; memcpy(rows, v, 7); return; }
    case 'M': { uint8_t v[7] = { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 }; memcpy(rows, v, 7); return; }
    case 'N': { uint8_t v[7] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }; memcpy(rows, v, 7); return; }
    case 'O': { uint8_t v[7] = { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }; memcpy(rows, v, 7); return; }
    case 'P': { uint8_t v[7] = { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 }; memcpy(rows, v, 7); return; }
    case 'Q': { uint8_t v[7] = { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d }; memcpy(rows, v, 7); return; }
    case 'R': { uint8_t v[7] = { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 }; memcpy(rows, v, 7); return; }
    case 'S': { uint8_t v[7] = { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e }; memcpy(rows, v, 7); return; }
    case 'T': { uint8_t v[7] = { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }; memcpy(rows, v, 7); return; }
    case 'U': { uint8_t v[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }; memcpy(rows, v, 7); return; }
    case 'V': { uint8_t v[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 }; memcpy(rows, v, 7); return; }
    case 'W': { uint8_t v[7] = { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a }; memcpy(rows, v, 7); return; }
    case 'X': { uint8_t v[7] = { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 }; memcpy(rows, v, 7); return; }
    case 'Y': { uint8_t v[7] = { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 }; memcpy(rows, v, 7); return; }
    case 'Z': { uint8_t v[7] = { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f }; memcpy(rows, v, 7); return; }
    case ':': { uint8_t v[7] = { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 }; memcpy(rows, v, 7); return; }
    case '.': { uint8_t v[7] = { 0, 0, 0, 0, 0, 0x0c, 0x0c }; memcpy(rows, v, 7); return; }
    case '/': { uint8_t v[7] = { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 }; memcpy(rows, v, 7); return; }
    case '-': { uint8_t v[7] = { 0, 0, 0, 0x1f, 0, 0, 0 }; memcpy(rows, v, 7); return; }
    case '_': { uint8_t v[7] = { 0, 0, 0, 0, 0, 0, 0x1f }; memcpy(rows, v, 7); return; }
    case '+': { uint8_t v[7] = { 0, 0x04, 0x04, 0x1f, 0x04, 0x04, 0 }; memcpy(rows, v, 7); return; }
    case '<': { uint8_t v[7] = { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 }; memcpy(rows, v, 7); return; }
    case '>': { uint8_t v[7] = { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }; memcpy(rows, v, 7); return; }
    case '[': { uint8_t v[7] = { 0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e }; memcpy(rows, v, 7); return; }
    case ']': { uint8_t v[7] = { 0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e }; memcpy(rows, v, 7); return; }
    case '#': { uint8_t v[7] = { 0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a }; memcpy(rows, v, 7); return; }
    default:
        return;
    }
}

class PanelPixelWriter {
public:
    PanelPixelWriter(uint8_t* destination, uint32_t destinationPitch, uint32_t destinationWidth, uint32_t destinationHeight, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation)
        : m_destination(destination)
        , m_destinationPitch(destinationPitch)
        , m_destinationWidth(destinationWidth)
        , m_destinationHeight(destinationHeight)
        , m_panelWidth(panelWidth)
        , m_panelHeight(panelHeight)
        , m_rotation(rotation)
    {
        resetClip();
    }

    void setClipRect(int x, int y, int width, int height)
    {
        m_clipX = std::clamp(x, 0, static_cast<int>(m_panelWidth));
        m_clipY = std::clamp(y, 0, static_cast<int>(m_panelHeight));
        m_clipRight = std::clamp(x + std::max(0, width), m_clipX, static_cast<int>(m_panelWidth));
        m_clipBottom = std::clamp(y + std::max(0, height), m_clipY, static_cast<int>(m_panelHeight));
    }

    void resetClip()
    {
        m_clipX = 0;
        m_clipY = 0;
        m_clipRight = static_cast<int>(m_panelWidth);
        m_clipBottom = static_cast<int>(m_panelHeight);
    }

    void setPixel(int x, int y, uint32_t color)
    {
        if (x < m_clipX || y < m_clipY || x >= m_clipRight || y >= m_clipBottom)
            return;

        int dx = x;
        int dy = y;
        switch (m_rotation) {
        case OutputRotation::Rotate0:
            break;
        case OutputRotation::Rotate90:
            dx = static_cast<int>(m_panelHeight) - 1 - y;
            dy = x;
            break;
        case OutputRotation::Rotate180:
            dx = static_cast<int>(m_panelWidth) - 1 - x;
            dy = static_cast<int>(m_panelHeight) - 1 - y;
            break;
        case OutputRotation::Rotate270:
            dx = y;
            dy = static_cast<int>(m_panelWidth) - 1 - x;
            break;
        }

        if (dx < 0 || dy < 0 || dx >= static_cast<int>(m_destinationWidth) || dy >= static_cast<int>(m_destinationHeight))
            return;
        auto* row = reinterpret_cast<uint32_t*>(m_destination + static_cast<size_t>(dy) * m_destinationPitch);
        row[dx] = color;
    }

    void blendPixel(int x, int y, uint32_t color, uint8_t coverage)
    {
        if (!coverage || x < m_clipX || y < m_clipY || x >= m_clipRight || y >= m_clipBottom)
            return;
        int dx = x;
        int dy = y;
        switch (m_rotation) {
        case OutputRotation::Rotate0:
            break;
        case OutputRotation::Rotate90:
            dx = static_cast<int>(m_panelHeight) - 1 - y;
            dy = x;
            break;
        case OutputRotation::Rotate180:
            dx = static_cast<int>(m_panelWidth) - 1 - x;
            dy = static_cast<int>(m_panelHeight) - 1 - y;
            break;
        case OutputRotation::Rotate270:
            dx = y;
            dy = static_cast<int>(m_panelWidth) - 1 - x;
            break;
        }
        if (dx < 0 || dy < 0 || dx >= static_cast<int>(m_destinationWidth) || dy >= static_cast<int>(m_destinationHeight))
            return;
        auto* row = reinterpret_cast<uint32_t*>(m_destination + static_cast<size_t>(dy) * m_destinationPitch);
        uint32_t destination = row[dx];
        auto blend = [coverage](uint8_t source, uint8_t target) {
            return static_cast<uint8_t>((source * coverage + target * (255 - coverage) + 127) / 255);
        };
        uint8_t red = blend((color >> 16) & 0xff, (destination >> 16) & 0xff);
        uint8_t green = blend((color >> 8) & 0xff, (destination >> 8) & 0xff);
        uint8_t blue = blend(color & 0xff, destination & 0xff);
        row[dx] = 0xff000000 | (static_cast<uint32_t>(red) << 16)
            | (static_cast<uint32_t>(green) << 8) | blue;
    }

    // 大块填充（工具栏底色、按钮、面板背景）按目标行序整段 std::fill；
    // 旧实现逐像素 setPixel，90/270 度下等于每 4 字节打断一次 WC 缓冲。
    void fillRect(int x, int y, int width, int height, uint32_t color)
    {
        int x1 = std::max(x, m_clipX);
        int y1 = std::max(y, m_clipY);
        int x2 = std::min(x + width, m_clipRight);
        int y2 = std::min(y + height, m_clipBottom);
        if (x2 <= x1 || y2 <= y1)
            return;
        auto rect = rotatedDestRect(x1, y1, x2, y2, m_panelWidth, m_panelHeight, m_rotation, 0);
        int dx1 = std::clamp(rect.x1, 0, static_cast<int>(m_destinationWidth));
        int dx2 = std::clamp(rect.x2, 0, static_cast<int>(m_destinationWidth));
        int dy1 = std::clamp(rect.y1, 0, static_cast<int>(m_destinationHeight));
        int dy2 = std::clamp(rect.y2, 0, static_cast<int>(m_destinationHeight));
        for (int dy = dy1; dy < dy2; ++dy) {
            auto* row = reinterpret_cast<uint32_t*>(m_destination + static_cast<size_t>(dy) * m_destinationPitch);
            std::fill(row + dx1, row + dx2, color);
        }
    }

    void strokeRect(int x, int y, int width, int height, uint32_t color)
    {
        fillRect(x, y, width, 1, color);
        fillRect(x, y + height - 1, width, 1, color);
        fillRect(x, y, 1, height, color);
        fillRect(x + width - 1, y, 1, height, color);
    }

    static double roundedRectCoverage(double pixelX, double pixelY,
                                      double left, double top, double right, double bottom,
                                      double radius)
    {
        if (right <= left || bottom <= top)
            return 0;
        double halfWidth = (right - left) * 0.5;
        double halfHeight = (bottom - top) * 0.5;
        radius = std::clamp(radius, 0.0, std::min(halfWidth, halfHeight));
        double centerX = (left + right) * 0.5;
        double centerY = (top + bottom) * 0.5;
        double qx = std::abs(pixelX - centerX) - (halfWidth - radius);
        double qy = std::abs(pixelY - centerY) - (halfHeight - radius);
        double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
        double inside = std::min(std::max(qx, qy), 0.0);
        double signedDistance = outside + inside - radius;
        return std::clamp(0.5 - signedDistance, 0.0, 1.0);
    }

    void drawRoundedRect(int x, int y, int width, int height, int radius,
                         uint32_t fillColor, uint32_t borderColor, int borderWidth)
    {
        if (width <= 0 || height <= 0)
            return;
        radius = std::clamp(radius, 0, std::min(width, height) / 2);
        borderWidth = std::clamp(borderWidth, 0, std::min(width, height) / 2);
        if (!radius) {
            fillRect(x, y, width, height, borderColor);
            if (width > borderWidth * 2 && height > borderWidth * 2)
                fillRect(x + borderWidth, y + borderWidth,
                         width - borderWidth * 2, height - borderWidth * 2, fillColor);
            return;
        }

        // Keep the large body writes contiguous. Only the four radius-sized corner
        // regions need coverage blending, which matters on 90/270 degree outputs.
        fillRect(x + radius, y, width - radius * 2, height, fillColor);
        if (height > radius * 2)
            fillRect(x, y + radius, width, height - radius * 2, fillColor);

        if (borderWidth > 0) {
            fillRect(x + radius, y, width - radius * 2, borderWidth, borderColor);
            fillRect(x + radius, y + height - borderWidth,
                     width - radius * 2, borderWidth, borderColor);
            fillRect(x, y + radius, borderWidth, height - radius * 2, borderColor);
            fillRect(x + width - borderWidth, y + radius,
                     borderWidth, height - radius * 2, borderColor);
        }

        double innerLeft = x + borderWidth;
        double innerTop = y + borderWidth;
        double innerRight = x + width - borderWidth;
        double innerBottom = y + height - borderWidth;
        double innerRadius = std::max(0, radius - borderWidth);
        for (int localY = 0; localY < height; ++localY) {
            bool cornerY = localY < radius || localY >= height - radius;
            if (!cornerY)
                continue;
            for (int localX = 0; localX < width; ++localX) {
                if (localX >= radius && localX < width - radius)
                    continue;
                double pixelX = x + localX + 0.5;
                double pixelY = y + localY + 0.5;
                double outerCoverage = roundedRectCoverage(pixelX, pixelY,
                    x, y, x + width, y + height, radius);
                if (outerCoverage <= 0)
                    continue;
                if (borderWidth <= 0) {
                    blendPixel(x + localX, y + localY, fillColor,
                               static_cast<uint8_t>(std::lround(outerCoverage * 255.0)));
                    continue;
                }
                blendPixel(x + localX, y + localY, borderColor,
                           static_cast<uint8_t>(std::lround(outerCoverage * 255.0)));
                double innerCoverage = roundedRectCoverage(pixelX, pixelY,
                    innerLeft, innerTop, innerRight, innerBottom, innerRadius);
                if (innerCoverage > 0)
                    blendPixel(x + localX, y + localY, fillColor,
                               static_cast<uint8_t>(std::lround(innerCoverage * 255.0)));
            }
        }
    }

    void fillCircle(int centerX, int centerY, int radius, uint32_t color)
    {
        for (int y = -radius; y <= radius; ++y) {
            int span = static_cast<int>(std::floor(std::sqrt(static_cast<double>(radius * radius - y * y))));
            fillRect(centerX - span, centerY + y, span * 2 + 1, 1, color);
        }
    }

    void drawLine(int x1, int y1, int x2, int y2, int thickness, uint32_t color, bool roundCaps = true)
    {
        int startX = x1;
        int startY = y1;
        int dx = std::abs(x2 - x1);
        int sx = x1 < x2 ? 1 : -1;
        int dy = -std::abs(y2 - y1);
        int sy = y1 < y2 ? 1 : -1;
        int error = dx + dy;
        int radius = std::max(0, thickness / 2);
        for (;;) {
            fillCircle(x1, y1, radius, color);
            if (x1 == x2 && y1 == y2)
                break;
            int twiceError = error * 2;
            if (twiceError >= dy) {
                error += dy;
                x1 += sx;
            }
            if (twiceError <= dx) {
                error += dx;
                y1 += sy;
            }
        }
        if (roundCaps) {
            fillCircle(startX, startY, radius, color);
            fillCircle(x2, y2, radius, color);
        }
    }

    void drawArc(int centerX, int centerY, int radius, int startDegrees, int endDegrees, int thickness, uint32_t color)
    {
        constexpr double pi = 3.14159265358979323846;
        int previousX = 0;
        int previousY = 0;
        bool hasPrevious = false;
        for (int degrees = startDegrees; degrees <= endDegrees; degrees += 5) {
            double radians = static_cast<double>(degrees) * pi / 180.0;
            int x = centerX + static_cast<int>(std::lround(std::cos(radians) * radius));
            int y = centerY + static_cast<int>(std::lround(std::sin(radians) * radius));
            if (hasPrevious)
                drawLine(previousX, previousY, x, y, thickness, color, false);
            previousX = x;
            previousY = y;
            hasPrevious = true;
        }
    }

    void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, uint32_t color)
    {
        int minX = std::min({ x1, x2, x3 });
        int maxX = std::max({ x1, x2, x3 });
        int minY = std::min({ y1, y2, y3 });
        int maxY = std::max({ y1, y2, y3 });
        auto edge = [](int ax, int ay, int bx, int by, int px, int py) {
            return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
        };
        int orientation = edge(x1, y1, x2, y2, x3, y3);
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                int e1 = edge(x1, y1, x2, y2, x, y);
                int e2 = edge(x2, y2, x3, y3, x, y);
                int e3 = edge(x3, y3, x1, y1, x, y);
                if ((orientation >= 0 && e1 >= 0 && e2 >= 0 && e3 >= 0)
                    || (orientation < 0 && e1 <= 0 && e2 <= 0 && e3 <= 0))
                    fillRect(x, y, 1, 1, color);
            }
        }
    }

    void drawChar(int x, int y, char c, uint32_t color, int scale)
    {
        if (c == ' ')
            return;
        uint8_t rows[7];
        fontRows(c, rows);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (!(rows[row] & (1 << (4 - col))))
                    continue;
                fillRect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }

    void drawText(int x, int y, const char* text, uint32_t color, int scale,
                  int maxWidth,
                  ChromeFontWeight weight = ChromeFontWeight::Regular)
    {
        if (!text)
            return;
        auto& font = ChromeFontCache::singleton();
        if (font.available(weight)) {
            int cursor = x;
            const char* position = text;
            while (*position) {
                gunichar character = g_utf8_get_char_validated(position, -1);
                if (character == static_cast<gunichar>(-1) || character == static_cast<gunichar>(-2)) {
                    character = '?';
                    position++;
                } else
                    position = g_utf8_next_char(position);
                const ChromeGlyph* glyph = font.glyph(character, weight);
                int advance = glyph ? glyph->advance : 7;
                if (cursor + advance > x + maxWidth)
                    break;
                if (glyph) {
                    int glyphX = cursor + glyph->left;
                    int glyphY = y + 12 - glyph->top;
                    for (int row = 0; row < glyph->height; ++row) {
                        for (int column = 0; column < glyph->width; ++column) {
                            uint8_t coverage = glyph->pixels[static_cast<size_t>(row) * glyph->width + column];
                            blendPixel(glyphX + column, glyphY + row, color, coverage);
                        }
                    }
                }
                cursor += advance;
            }
            return;
        }
        int cursor = x;
        int advance = 6 * scale;
        for (const char* p = text; *p && cursor + advance <= x + maxWidth; ++p) {
            drawChar(cursor, y, *p, color, scale);
            cursor += advance;
        }
    }

    int measureText(const char* text, int scale,
                    ChromeFontWeight weight = ChromeFontWeight::Regular)
    {
        if (!text)
            return 0;
        auto& font = ChromeFontCache::singleton();
        if (!font.available(weight))
            return static_cast<int>(strlen(text)) * 6 * scale;

        int width = 0;
        const char* position = text;
        while (*position) {
            gunichar character = g_utf8_get_char_validated(position, -1);
            if (character == static_cast<gunichar>(-1) || character == static_cast<gunichar>(-2)) {
                character = '?';
                position++;
            } else
                position = g_utf8_next_char(position);
            const ChromeGlyph* glyph = font.glyph(character, weight);
            width += glyph ? glyph->advance : 7;
        }
        return width;
    }

private:
    uint8_t* m_destination { nullptr };
    uint32_t m_destinationPitch { 0 };
    uint32_t m_destinationWidth { 0 };
    uint32_t m_destinationHeight { 0 };
    uint32_t m_panelWidth { 0 };
    uint32_t m_panelHeight { 0 };
    OutputRotation m_rotation { OutputRotation::Rotate0 };
    int m_clipX { 0 };
    int m_clipY { 0 };
    int m_clipRight { 0 };
    int m_clipBottom { 0 };
};

static void drawChromeMenuIcon(PanelPixelWriter& painter, const char* id,
                               int centerX, int centerY, uint32_t color)
{
    if (!strcmp(id, "bookmark")) {
        painter.drawLine(centerX - 7, centerY - 8, centerX + 7, centerY - 8, 2, color);
        painter.drawLine(centerX - 7, centerY - 8, centerX - 7, centerY + 8, 2, color);
        painter.drawLine(centerX + 7, centerY - 8, centerX + 7, centerY + 8, 2, color);
        painter.drawLine(centerX - 7, centerY + 8, centerX, centerY + 3, 2, color);
        painter.drawLine(centerX, centerY + 3, centerX + 7, centerY + 8, 2, color);
    } else if (!strcmp(id, "history")) {
        painter.drawArc(centerX, centerY, 9, 0, 359, 2, color);
        painter.drawLine(centerX, centerY, centerX, centerY - 6, 2, color);
        painter.drawLine(centerX, centerY, centerX + 5, centerY + 3, 2, color);
    } else if (!strcmp(id, "bookmarks")) {
        painter.drawRoundedRect(centerX - 9, centerY - 8, 14, 17, 3, 0x00000000, color, 2);
        painter.drawRoundedRect(centerX - 3, centerY - 5, 12, 14, 3, 0x00000000, color, 2);
    } else if (!strcmp(id, "profiles")) {
        painter.fillCircle(centerX, centerY - 6, 5, color);
        painter.drawArc(centerX, centerY + 10, 10, 200, 340, 3, color);
        painter.fillCircle(centerX - 10, centerY - 1, 3, color);
        painter.fillCircle(centerX + 10, centerY - 1, 3, color);
    } else if (!strcmp(id, "privacy")) {
        painter.drawLine(centerX, centerY - 10, centerX - 8, centerY - 6, 2, color);
        painter.drawLine(centerX - 8, centerY - 6, centerX - 6, centerY + 4, 2, color);
        painter.drawLine(centerX - 6, centerY + 4, centerX, centerY + 10, 2, color);
        painter.drawLine(centerX, centerY + 10, centerX + 6, centerY + 4, 2, color);
        painter.drawLine(centerX + 6, centerY + 4, centerX + 8, centerY - 6, 2, color);
        painter.drawLine(centerX + 8, centerY - 6, centerX, centerY - 10, 2, color);
    } else if (!strcmp(id, "theme")) {
        painter.fillCircle(centerX, centerY, 5, color);
        for (int offset = -10; offset <= 10; offset += 20) {
            painter.drawLine(centerX + offset, centerY - 3,
                             centerX + offset, centerY + 3, 2, color);
            painter.drawLine(centerX - 3, centerY + offset,
                             centerX + 3, centerY + offset, 2, color);
        }
    } else if (!strcmp(id, "web") || !strcmp(id, "webkit") || !strcmp(id, "language")) {
        painter.drawArc(centerX, centerY, 9, 0, 359, 2, color);
        painter.drawArc(centerX, centerY, 4, 80, 280, 1, color);
        painter.drawLine(centerX - 8, centerY, centerX + 8, centerY, 1, color);
    } else if (!strcmp(id, "search") || !strcmp(id, "zoom")) {
        painter.drawArc(centerX - 2, centerY - 2, 6, 0, 359, 2, color);
        painter.drawLine(centerX + 3, centerY + 3,
                         centerX + 9, centerY + 9, 2, color);
    } else if (!strcmp(id, "home")) {
        painter.drawLine(centerX - 9, centerY, centerX, centerY - 8, 2, color);
        painter.drawLine(centerX, centerY - 8, centerX + 9, centerY, 2, color);
        painter.drawRoundedRect(centerX - 6, centerY, 12, 9, 2,
                                0x00000000, color, 2);
    } else if (!strcmp(id, "toolbar")) {
        painter.drawRoundedRect(centerX - 10, centerY - 7, 20, 14, 3,
                                0x00000000, color, 2);
        painter.fillRect(centerX - 7, centerY - 4, 14, 3, color);
    } else if (!strcmp(id, "font")) {
        painter.drawLine(centerX - 7, centerY + 8, centerX, centerY - 9, 2, color);
        painter.drawLine(centerX, centerY - 9, centerX + 7, centerY + 8, 2, color);
        painter.drawLine(centerX - 4, centerY + 2, centerX + 4, centerY + 2, 2, color);
    } else if (!strcmp(id, "javascript") || !strcmp(id, "custom")) {
        painter.drawLine(centerX - 2, centerY - 7, centerX - 8, centerY, 2, color);
        painter.drawLine(centerX - 8, centerY, centerX - 2, centerY + 7, 2, color);
        painter.drawLine(centerX + 2, centerY - 7, centerX + 8, centerY, 2, color);
        painter.drawLine(centerX + 8, centerY, centerX + 2, centerY + 7, 2, color);
    } else if (!strcmp(id, "autoplay")) {
        painter.drawArc(centerX, centerY, 10, 0, 359, 2, color);
        painter.fillTriangle(centerX - 3, centerY - 6,
                             centerX - 3, centerY + 6,
                             centerX + 7, centerY, color);
    } else if (!strcmp(id, "scroll")) {
        painter.drawLine(centerX - 8, centerY - 6, centerX + 8, centerY - 6, 2, color);
        painter.drawLine(centerX - 8, centerY, centerX + 5, centerY, 2, color);
        painter.drawLine(centerX - 8, centerY + 6, centerX + 8, centerY + 6, 2, color);
    } else if (!strcmp(id, "popup") || !strcmp(id, "display")) {
        painter.drawRoundedRect(centerX - 10, centerY - 8, 20, 16, 3,
                                0x00000000, color, 2);
        painter.fillRect(centerX - 7, centerY - 5, 14, 2, color);
    } else if (!strcmp(id, "tabs")) {
        painter.drawRoundedRect(centerX - 8, centerY - 9, 16, 18, 4,
                                0x00000000, color, 2);
    } else if (!strcmp(id, "clear") || !strcmp(id, "cache")) {
        painter.drawArc(centerX, centerY + 2, 8, 35, 315, 2, color);
        painter.fillTriangle(centerX + 9, centerY + 1,
                             centerX + 3, centerY,
                             centerX + 8, centerY - 6, color);
    } else if (!strcmp(id, "about") || !strcmp(id, "renderer")) {
        painter.drawArc(centerX, centerY, 9, 0, 359, 2, color);
        painter.fillCircle(centerX, centerY - 4, 2, color);
        painter.drawLine(centerX, centerY, centerX, centerY + 6, 2, color);
    } else if (!strcmp(id, "rotation")) {
        painter.drawArc(centerX, centerY, 9, 35, 315, 2, color);
        painter.fillTriangle(centerX + 9, centerY - 1,
                             centerX + 2, centerY - 2,
                             centerX + 7, centerY - 8, color);
        painter.fillCircle(centerX - 8, centerY + 4, 1, color);
    } else {
        painter.drawArc(centerX, centerY, 7, 0, 359, 3, color);
        painter.fillCircle(centerX, centerY, 3, color);
        constexpr double pi = 3.14159265358979323846;
        for (int angle = 0; angle < 360; angle += 45) {
            double radians = angle * pi / 180.0;
            int innerX = centerX + static_cast<int>(std::lround(cos(radians) * 9));
            int innerY = centerY + static_cast<int>(std::lround(sin(radians) * 9));
            int outerX = centerX + static_cast<int>(std::lround(cos(radians) * 12));
            int outerY = centerY + static_cast<int>(std::lround(sin(radians) * 12));
            painter.drawLine(innerX, innerY, outerX, outerY, 2, color);
        }
    }
}

struct ChromeTheme {
    uint32_t toolbar;
    uint32_t background;
    uint32_t surface;
    uint32_t surfaceVariant;
    uint32_t outline;
    uint32_t text;
    uint32_t secondaryText;
    uint32_t disabled;
    uint32_t accent;
    uint32_t onAccent;
    uint32_t pressed;
    uint32_t danger;
    uint32_t dangerSurface;
    uint32_t shadow;
};

static ChromeTheme chromeTheme(const ChromeRenderState& state)
{
    if (!g_ascii_strcasecmp(state.theme, "dark")) {
        return {
            0xff202124, 0xff121212, 0xff252525, 0xff303134,
            0xff5f6368, 0xfff1f3f4, 0xffbdc1c6, 0xff777b80,
            0xff8ab4f8, 0xff202124, 0x408ab4f8, 0xffff8a80,
            0xff4a2022, 0x42000000
        };
    }
    return {
        0xfff1f3f4, 0xfff8f9fa, 0xffffffff, 0xffeef3f8,
        0xffd2d8df, 0xff202124, 0xff5f6368, 0xff9aa0a6,
        0xff1a73e8, 0xffffffff, 0x301a73e8, 0xffd93025,
        0xffffe8e6, 0x24000000
    };
}

static void drawMaterialSwitch(PanelPixelWriter& painter, int centerX, int centerY,
                               bool checked, bool enabled, const ChromeTheme& theme)
{
    uint32_t track = !enabled ? theme.outline
        : checked ? theme.accent : theme.secondaryText;
    uint32_t thumb = !enabled ? theme.disabled
        : checked ? theme.onAccent : theme.surface;
    painter.drawRoundedRect(centerX - 16, centerY - 8, 32, 16, 8,
                            track, track, 1);
    painter.fillCircle(centerX + (checked ? 8 : -8), centerY, 6, thumb);
}

static void drawMaterialChevron(PanelPixelWriter& painter, int centerX, int centerY,
                                uint32_t color)
{
    painter.drawLine(centerX - 3, centerY - 5, centerX + 3, centerY, 2, color);
    painter.drawLine(centerX + 3, centerY, centerX - 3, centerY + 5, 2, color);
}

void drawChromeOverlay(uint8_t* destination, uint32_t destinationPitch, uint32_t destinationWidth, uint32_t destinationHeight, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation)
{
    const auto& chrome = cachedChromeRenderState();
    if (!chrome.enabled)
        return;
    static bool loggedChromeOverlay;
    if (!loggedChromeOverlay) {
        loggedChromeOverlay = true;
        g_message("WPEViewDRM chrome_overlay=enabled state=in-process-snapshot");
    }

    PanelPixelWriter painter(destination, destinationPitch, destinationWidth, destinationHeight, panelWidth, panelHeight, rotation);
    const auto theme = chromeTheme(chrome);
    const uint32_t black = theme.surfaceVariant;
    const uint32_t dark = theme.toolbar;
    const uint32_t mid = theme.outline;
    const uint32_t white = theme.surface;
    const uint32_t text = theme.text;
    const uint32_t disabled = theme.disabled;
    const uint32_t accent = theme.accent;
    const uint32_t progressBlue = 0xff4285f4;
    const uint32_t panel = theme.background;

    int chromeHeight = std::clamp<int>(chrome.height, 24, std::min<int>(80, panelHeight));
    bool hasPanel = strcmp(chrome.panel, "none") && chrome.panel[0];
    double shownFraction = hasPanel ? 1.0 : chromeShownFraction(chrome);
    int toolbarY = static_cast<int>(std::lround((shownFraction - 1.0) * chromeHeight));

    if (chrome.loading && panelWidth > 0 && panelHeight >= 3) {
        double progress = std::isfinite(chrome.loadProgress) ? std::clamp(chrome.loadProgress, 0.0, 1.0) : 0.0;
        int progressY = shownFraction <= 0.001 && !hasPanel ? 0 : toolbarY + chromeHeight;
        progressY = std::clamp(progressY, 0, static_cast<int>(panelHeight) - 3);
        int fillWidth = static_cast<int>(std::lround(panelWidth * progress));
        if (progress > 0.0 && fillWidth < 1)
            fillWidth = 1;
        if (fillWidth > 0)
            painter.fillRect(0, progressY, std::min<int>(fillWidth, panelWidth), 3, progressBlue);
    }

    if (shownFraction <= 0.001 && !hasPanel)
        return;

    painter.fillRect(0, toolbarY, panelWidth, chromeHeight, dark);
    int addressX = chrome.addressWidth > 0 ? chrome.addressX : 8;
    int addressY = chrome.addressWidth > 0 ? chrome.addressY : 6;
    int addressWidth = chrome.addressWidth;
    int addressHeight = chrome.addressHeight;
    int controlsX = chrome.controlsX;
    int controlsY = chrome.controlsY;
    int controlsWidth = chrome.controlsWidth;
    int controlsHeight = chrome.controlsHeight;
    int buttonCount = std::clamp(chrome.buttonCount, 1, 8);
    if (addressWidth <= 0 || addressHeight <= 0
        || controlsWidth <= 0 || controlsHeight <= 0) {
        int legacyButtonX = panelWidth / 2 + 8;
        int legacyButtonW = std::max<int>(36,
            (static_cast<int>(panelWidth) - legacyButtonX - 8) / 6);
        controlsX = legacyButtonX + legacyButtonW;
        controlsY = 6;
        controlsWidth = legacyButtonW * 5;
        controlsHeight = chromeHeight - 12;
        buttonCount = 5;
        addressX = 8;
        addressY = 6;
        addressWidth = std::max(1, controlsX - 16);
        addressHeight = controlsHeight;
    }
    painter.drawRoundedRect(addressX, toolbarY + addressY,
                            addressWidth, addressHeight, 8, white, mid, 1);
    if (chrome.pressedControl == -2)
        painter.drawRoundedRect(addressX, toolbarY + addressY,
                                addressWidth, addressHeight, 8,
                                theme.pressed, theme.pressed, 1);
    painter.drawText(addressX + 10, toolbarY + addressY + 11,
                     chrome.url[0] ? chrome.url : chrome.homeLabel,
                     text, 1, std::max(1, addressWidth - 20));

    bool tabsPanelOpen = !strcmp(chrome.panel, "tabs");
    bool overflowPanelOpen = hasPanel && !tabsPanelOpen;
    bool enabled[5] = {
        chrome.canBack,
        chrome.canForward,
        true,
        overflowPanelOpen || !tabsPanelOpen || chrome.panelCanAdd,
        true
    };
    int visibleButtonCount = std::min(5, buttonCount);
    for (int i = 0; i < visibleButtonCount; ++i) {
        int x = controlsX + controlsWidth * i / buttonCount;
        int right = controlsX + controlsWidth * (i + 1) / buttonCount;
        int buttonW = std::max(1, right - x);
        painter.drawRoundedRect(x + 2, toolbarY + controlsY + 1,
                                std::max(1, buttonW - 4),
                                std::max(1, controlsHeight - 2), 6,
                                theme.surface, enabled[i] ? theme.outline : mid, 1);
        if (chrome.pressedControl == i)
            painter.drawRoundedRect(x + 2, toolbarY + controlsY + 1,
                                    std::max(1, buttonW - 4),
                                    std::max(1, controlsHeight - 2), 6,
                                    theme.pressed, theme.pressed, 1);

        int centerX = x + buttonW / 2;
        int centerY = toolbarY + controlsY + controlsHeight / 2;
        uint32_t iconColor = enabled[i] ? accent : disabled;
        switch (i) {
        case 0:
            painter.drawLine(centerX + 4, centerY - 7, centerX - 4, centerY, 2, iconColor);
            painter.drawLine(centerX - 4, centerY, centerX + 4, centerY + 7, 2, iconColor);
            break;
        case 1:
            painter.drawLine(centerX - 4, centerY - 7, centerX + 4, centerY, 2, iconColor);
            painter.drawLine(centerX + 4, centerY, centerX - 4, centerY + 7, 2, iconColor);
            break;
        case 2:
        {
            char tabCount[12];
            snprintf(tabCount, sizeof(tabCount), "%u", std::max<unsigned>(1, chrome.tabCount));
            painter.drawRoundedRect(centerX - 10, centerY - 10, 20, 20, 5,
                                    black, iconColor, 2);
            int labelWidth = painter.measureText(tabCount, 1, ChromeFontWeight::Bold);
            int labelY = ChromeFontCache::singleton().available() ? centerY - 7 : centerY - 3;
            painter.drawText(centerX - labelWidth / 2, labelY, tabCount,
                             iconColor, 1, 18, ChromeFontWeight::Bold);
            break;
        }
        case 3:
            if (tabsPanelOpen) {
                painter.drawLine(centerX - 6, centerY, centerX + 6, centerY, 2, iconColor);
                painter.drawLine(centerX, centerY - 6, centerX, centerY + 6, 2, iconColor);
            } else if (overflowPanelOpen) {
                // Arc runs from upper-right through the bottom to upper-left,
                // leaving a symmetric top gap for the power stem.
                painter.drawArc(centerX, centerY + 1, 8, -45, 225, 2, iconColor);
                painter.drawLine(centerX, centerY - 10, centerX, centerY, 2, iconColor);
            } else if (chrome.loading) {
                painter.drawLine(centerX - 5, centerY - 5, centerX + 5, centerY + 5, 2, iconColor);
                painter.drawLine(centerX + 5, centerY - 5, centerX - 5, centerY + 5, 2, iconColor);
            } else {
                painter.drawArc(centerX, centerY, 7, 35, 315, 2, iconColor);
                painter.fillTriangle(centerX + 8, centerY, centerX + 2, centerY - 1,
                                     centerX + 7, centerY - 6, iconColor);
            }
            break;
        case 4:
            if (overflowPanelOpen) {
                painter.drawLine(centerX - 6, centerY - 6, centerX + 6, centerY + 6, 2, iconColor);
                painter.drawLine(centerX + 6, centerY - 6, centerX - 6, centerY + 6, 2, iconColor);
            } else {
                painter.fillCircle(centerX, centerY - 6, 2, iconColor);
                painter.fillCircle(centerX, centerY, 2, iconColor);
                painter.fillCircle(centerX, centerY + 6, 2, iconColor);
            }
            break;
        }
    }

    if (!hasPanel)
        return;

    if (!strcmp(chrome.panel, "tabs")) {
        int tabsX = std::clamp(chrome.panelX, 0, std::max(0, static_cast<int>(panelWidth) - 1));
        int tabsY = std::clamp(chrome.panelY, toolbarY + chromeHeight, std::max(toolbarY + chromeHeight, static_cast<int>(panelHeight) - 1));
        int tabsWidth = chrome.panelWidth > 0 ? chrome.panelWidth : static_cast<int>(panelWidth) - tabsX;
        int tabsHeight = chrome.panelHeight > 0 ? chrome.panelHeight : static_cast<int>(panelHeight) - tabsY;
        tabsWidth = std::clamp(tabsWidth, 1, static_cast<int>(panelWidth) - tabsX);
        tabsHeight = std::clamp(tabsHeight, 1, static_cast<int>(panelHeight) - tabsY);
        int headerHeight = std::clamp(chrome.panelHeaderHeight, 0, std::min(48, tabsHeight));
        int footerHeight = std::clamp(chrome.panelFooterHeight, 0, std::min(56, tabsHeight));
        int rowHeight = std::clamp(chrome.panelRowHeight, 26, 48);
        int listTop = tabsY + headerHeight;
        int footerTop = tabsY + tabsHeight - footerHeight;
        if (footerTop < listTop)
            footerTop = listTop;

        painter.drawRoundedRect(tabsX + 1, tabsY + 2, tabsWidth - 1, tabsHeight - 2,
                                10, theme.shadow, theme.shadow, 1);
        painter.drawRoundedRect(tabsX, tabsY, tabsWidth, tabsHeight, 10, panel, mid, 1);

        painter.setClipRect(tabsX + 1, listTop, tabsWidth - 2, std::max(0, footerTop - listTop));
        int scrollOffset = static_cast<int>(std::lround(chrome.panelScrollOffset));
        for (unsigned i = 0; i < chrome.lineCount; ++i) {
            int rowY = listTop + static_cast<int>(i) * rowHeight - scrollOffset;
            if (rowY + rowHeight <= listTop || rowY >= footerTop)
                continue;
            bool active = i == chrome.activeTab;
            uint32_t rowFill = active ? theme.surfaceVariant : theme.surface;
            if (chrome.pressedRow == static_cast<int>(i))
                rowFill = theme.pressed;
            uint32_t rowBorder = active ? accent : theme.outline;
            painter.drawRoundedRect(tabsX + 7, rowY + 3, tabsWidth - 14, rowHeight - 6,
                                    8, rowFill, rowBorder, 1);
            int closeWidth = 52;
            painter.drawText(tabsX + 16, rowY + std::max(5, (rowHeight - 14) / 2),
                             chrome.lines[i], text, 1, tabsWidth - closeWidth - 24);
            int closeCenterX = tabsX + tabsWidth - closeWidth / 2;
            int closeCenterY = rowY + rowHeight / 2;
            painter.drawLine(closeCenterX - 5, closeCenterY - 5,
                             closeCenterX + 5, closeCenterY + 5, 2, mid);
            painter.drawLine(closeCenterX + 5, closeCenterY - 5,
                             closeCenterX - 5, closeCenterY + 5, 2, mid);
        }
        painter.resetClip();
        return;
    }

    if (!strcmp(chrome.panel, "menu")) {
        int menuX = std::clamp(chrome.panelX, 0, std::max(0, static_cast<int>(panelWidth) - 1));
        int menuY = std::clamp(chrome.panelY, toolbarY + chromeHeight,
                               std::max(toolbarY + chromeHeight, static_cast<int>(panelHeight) - 1));
        int menuWidth = chrome.panelWidth > 0 ? chrome.panelWidth : static_cast<int>(panelWidth) - menuX;
        int menuHeight = chrome.panelHeight > 0 ? chrome.panelHeight : static_cast<int>(panelHeight) - menuY;
        menuWidth = std::clamp(menuWidth, 1, static_cast<int>(panelWidth) - menuX);
        menuHeight = std::clamp(menuHeight, 1, static_cast<int>(panelHeight) - menuY);
        int columns = std::clamp(chrome.panelColumns, 1, 6);
        int rows = std::clamp(chrome.panelRows, 1, 4);
        int pageCount = std::max(1, chrome.panelPageCount);
        int dotsHeight = pageCount > 1 ? 14 : 0;
        int gridHeight = std::max(1, menuHeight - dotsHeight);
        int cellWidth = std::max(1, menuWidth / columns);
        int cellHeight = std::max(1, gridHeight / rows);

        painter.drawRoundedRect(menuX + 1, menuY + 2, menuWidth - 1, menuHeight - 2,
                                10, theme.shadow, theme.shadow, 1);
        painter.drawRoundedRect(menuX, menuY, menuWidth, menuHeight, 10, panel, mid, 1);
        painter.setClipRect(menuX + 1, menuY + 1, menuWidth - 2, menuHeight - 2);
        int scrollOffset = static_cast<int>(std::lround(chrome.panelScrollOffset));
        int itemsPerPage = columns * rows;
        for (unsigned i = 0; i < chrome.lineCount; ++i) {
            int pageIndex = static_cast<int>(i) / itemsPerPage;
            int pageItem = static_cast<int>(i) % itemsPerPage;
            int row = pageItem / columns;
            int column = pageItem % columns;
            int cellX = menuX + pageIndex * menuWidth - scrollOffset + column * cellWidth;
            int cellY = menuY + row * cellHeight;
            if (cellX + cellWidth <= menuX || cellX >= menuX + menuWidth)
                continue;

            bool itemIsEnabled = i < 10 ? chrome.itemEnabled[i] : true;
            uint32_t cellFill = itemIsEnabled ? theme.surface : theme.surfaceVariant;
            if (chrome.pressedRow == static_cast<int>(i))
                cellFill = theme.pressed;
            uint32_t cellBorder = itemIsEnabled ? theme.outline : mid;
            uint32_t itemColor = itemIsEnabled ? accent : disabled;
            painter.drawRoundedRect(cellX + 7, cellY + 6, cellWidth - 14,
                                    std::max(1, cellHeight - 12), 8,
                                    cellFill, cellBorder, 1);
            int iconCenterY = cellY + std::max(20, cellHeight / 2 - 10);
            drawChromeMenuIcon(painter, i < 10 ? chrome.itemIDs[i] : "",
                               cellX + cellWidth / 2, iconCenterY, itemColor);
            int labelWidth = painter.measureText(chrome.lines[i], 1,
                                                 ChromeFontWeight::Medium);
            int labelX = cellX + std::max(6, (cellWidth - labelWidth) / 2);
            int labelY = cellY + cellHeight - 23;
            painter.drawText(labelX, labelY, chrome.lines[i], itemColor, 1,
                             std::max(1, cellWidth - 12),
                             ChromeFontWeight::Medium);
        }
        painter.resetClip();

        if (pageCount > 1) {
            int dotsWidth = (pageCount - 1) * 12 + 6;
            int firstX = menuX + (menuWidth - dotsWidth) / 2 + 3;
            int activePage = std::clamp(static_cast<int>(std::lround(
                chrome.panelScrollOffset / std::max(1, menuWidth))), 0, pageCount - 1);
            for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex)
                painter.fillCircle(firstX + pageIndex * 12, menuY + menuHeight - 7,
                                   pageIndex == activePage ? 3 : 2,
                                   pageIndex == activePage ? accent : mid);
        }
        return;
    }

    int listX = std::clamp(chrome.panelX, 0,
                           std::max(0, static_cast<int>(panelWidth) - 1));
    int listY = std::clamp(chrome.panelY, toolbarY + chromeHeight,
                           std::max(toolbarY + chromeHeight,
                                    static_cast<int>(panelHeight) - 1));
    int listWidth = chrome.panelWidth > 0
        ? chrome.panelWidth : static_cast<int>(panelWidth) - listX;
    int listHeight = chrome.panelHeight > 0
        ? chrome.panelHeight : static_cast<int>(panelHeight) - listY;
    listWidth = std::clamp(listWidth, 1, static_cast<int>(panelWidth) - listX);
    listHeight = std::clamp(listHeight, 1, static_cast<int>(panelHeight) - listY);
    int headerHeight = std::clamp(chrome.panelHeaderHeight, 24,
                                  std::min(48, listHeight));
    int rowHeight = std::clamp(chrome.panelRowHeight, 40, 64);
    int contentTop = listY + headerHeight;
    int contentBottom = listY + listHeight;
    int scrollOffset = std::clamp(
        static_cast<int>(std::lround(chrome.panelScrollOffset)), 0,
        static_cast<int>(std::ceil(chrome.panelMaxScroll)));
    const uint32_t dangerColor = theme.danger;
    const uint32_t dangerFill = theme.dangerSurface;
    const uint32_t disabledFill = theme.surfaceVariant;

    painter.drawRoundedRect(listX + 1, listY + 2, listWidth - 1, listHeight - 2,
                            10, theme.shadow, theme.shadow, 1);
    painter.drawRoundedRect(listX, listY, listWidth, listHeight, 10,
                            panel, mid, 1);

    int backCenterX = listX + 20;
    int headerCenterY = listY + headerHeight / 2;
    painter.drawLine(backCenterX + 4, headerCenterY - 6,
                     backCenterX - 3, headerCenterY, 2, accent);
    painter.drawLine(backCenterX - 3, headerCenterY,
                     backCenterX + 4, headerCenterY + 6, 2, accent);
    painter.drawText(listX + 38,
                     listY + std::max(5, (headerHeight - 14) / 2),
                     chrome.panelTitle[0] ? chrome.panelTitle : chrome.panel,
                     accent, 1,
                     std::max(1, listWidth - 54),
                     ChromeFontWeight::Bold);
    if (chrome.touchDebug)
        painter.drawText(listX + listWidth - 140,
                         listY + std::max(5, (headerHeight - 14) / 2),
                         chrome.touchDebugLabel, accent, 1, 130);

    painter.setClipRect(listX + 1, contentTop, listWidth - 2,
                        std::max(0, contentBottom - contentTop));
    for (unsigned i = 0; i < chrome.lineCount; ++i) {
        int rowY = contentTop + static_cast<int>(i) * rowHeight - scrollOffset;
        if (rowY + rowHeight <= contentTop || rowY >= contentBottom)
            continue;

        bool enabled = i < 10 ? chrome.lineEnabled[i] : true;
        bool danger = i < 10 ? chrome.lineDanger[i] : false;
        uint32_t rowFill = !enabled ? disabledFill
            : danger ? dangerFill : theme.surface;
        if (chrome.pressedRow == static_cast<int>(i) && enabled)
            rowFill = theme.pressed;
        uint32_t rowBorder = danger ? dangerColor
            : enabled ? theme.outline : mid;
        uint32_t rowText = !enabled ? disabled : danger ? dangerColor : text;
        int cardX = listX + 7;
        int cardY = rowY + 3;
        int cardWidth = std::max(1, listWidth - 14);
        int cardHeight = std::max(1, rowHeight - 6);
        painter.drawRoundedRect(cardX + 1, cardY + 1, cardWidth, cardHeight, 8,
                                theme.shadow, theme.shadow, 1);
        painter.drawRoundedRect(cardX, cardY, cardWidth, cardHeight, 8,
                                rowFill, rowBorder, 1);

        int segmentCount = i < 10
            ? std::clamp(chrome.lineSegmentCount[i], 1, 3) : 1;
        int textY = rowY + std::max(5, (rowHeight - 14) / 2);
        if (segmentCount == 1) {
            const char* kind = chrome.lineKinds[i][0]
                ? chrome.lineKinds[i] : "action";
            int textLeft = cardX + 12;
            if (chrome.lineIcons[i][0]) {
                drawChromeMenuIcon(painter, chrome.lineIcons[i],
                                   cardX + 19, rowY + rowHeight / 2,
                                   enabled ? accent : disabled);
                textLeft = cardX + 38;
            }
            int trailingWidth = 12;
            if (!strcmp(kind, "toggle")) {
                drawMaterialSwitch(painter, cardX + cardWidth - 27,
                                   rowY + rowHeight / 2,
                                   chrome.lineChecked[i], enabled, theme);
                trailingWidth = 58;
            } else if (!strcmp(kind, "navigation")) {
                drawMaterialChevron(painter, cardX + cardWidth - 17,
                                    rowY + rowHeight / 2,
                                    enabled ? theme.secondaryText : disabled);
                trailingWidth = 30;
            }
            if (chrome.lineValues[i][0]) {
                int valueWidth = painter.measureText(chrome.lineValues[i], 1);
                int valueRight = cardX + cardWidth - trailingWidth;
                int valueX = std::max(textLeft + 40, valueRight - valueWidth);
                painter.drawText(valueX, textY, chrome.lineValues[i],
                                 enabled ? theme.secondaryText : disabled, 1,
                                 std::max(1, valueRight - valueX));
                trailingWidth += valueWidth + 12;
            }
            painter.drawText(textLeft, textY, chrome.lines[i], rowText, 1,
                             std::max(1, cardWidth - (textLeft - cardX)
                                              - trailingWidth),
                             ChromeFontWeight::Medium);
            continue;
        }

        for (int segment = 0; segment < segmentCount; ++segment) {
            int segmentLeft = cardX + cardWidth * segment / segmentCount;
            int segmentRight = cardX + cardWidth * (segment + 1) / segmentCount;
            if (segment > 0)
                painter.fillRect(segmentLeft, cardY + 5, 1,
                                 std::max(1, cardHeight - 10), mid);
            bool segmentEnabled = chrome.lineSegmentEnabled[i][segment];
            bool segmentDanger = chrome.lineSegmentDanger[i][segment];
            uint32_t segmentColor = !segmentEnabled ? disabled
                : segmentDanger ? dangerColor : text;
            const char* label = chrome.lineSegmentLabels[i][segment][0]
                ? chrome.lineSegmentLabels[i][segment] : chrome.lines[i];
            int segmentWidth = std::max(1, segmentRight - segmentLeft);
            int labelWidth = painter.measureText(label, 1);
            int labelX = segmentLeft
                + std::max(6, (segmentWidth - labelWidth) / 2);
            painter.drawText(labelX, textY, label, segmentColor, 1,
                             std::max(1, segmentWidth - 12));
        }
    }
    painter.resetClip();
}


size_t chromeFontCacheBytes() { return ChromeFontCache::singleton().bytes(); }
void trimChromeFontCaches(bool critical) { ChromeFontCache::singleton().trim(critical); }
