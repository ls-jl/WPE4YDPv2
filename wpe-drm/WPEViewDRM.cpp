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
#include "WPEViewDRM.h"

#include "DRMUniquePtr.h"
#include "WPEDisplayDRMPrivate.h"
#include "WPEScreenDRMPrivate.h"
#include "WPEToplevelDRM.h"
#include "WPEBufferSHM.h"
#include "WPEViewDRMPrivate.h"
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <linux/dma-buf.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <optional>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <wtf/FastMalloc.h>
#include <wtf/OptionSet.h>
#include <wtf/RunLoop.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Seconds.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/WTFGType.h>

enum class UpdateFlags : uint8_t {
    BufferUpdateRequested = 1 << 0,
    CursorUpdateRequested = 1 << 1,
    BufferUpdatePending = 1 << 2,
    CursorUpdatePending = 1 << 3
};

enum class OutputRotation : uint16_t {
    Rotate0 = 0,
    Rotate90 = 90,
    Rotate180 = 180,
    Rotate270 = 270
};

struct PanelSize {
    uint32_t width { 960 };
    uint32_t height { 266 };
};

static bool parseSize(const char* value, uint32_t& width, uint32_t& height)
{
    if (!value || !*value)
        return false;

    int parsedWidth = 0;
    int parsedHeight = 0;
    char separator = 0;
    if (sscanf(value, "%d%c%d", &parsedWidth, &separator, &parsedHeight) != 3)
        return false;
    if (separator != 'x' && separator != 'X' && separator != ',')
        return false;
    if (parsedWidth < 64 || parsedHeight < 64 || parsedWidth > 4096 || parsedHeight > 4096)
        return false;

    width = static_cast<uint32_t>(parsedWidth);
    height = static_cast<uint32_t>(parsedHeight);
    return true;
}

// 以下配置全部来自环境变量，进程生命周期内不变；用 static 缓存，
// 避免在每帧 copy/commit 热路径里反复 getenv+解析。
static PanelSize configuredPanelSize()
{
    static PanelSize size = []() {
        PanelSize parsed;
        uint32_t width = 0;
        uint32_t height = 0;
        if (parseSize(getenv("WPE_PANEL_SIZE"), width, height)) {
            parsed.width = width;
            parsed.height = height;
        }
        return parsed;
    }();
    return size;
}

static bool chromeLayoutResizeEnabled()
{
    static bool enabled = []() {
        const char* value = getenv("WPE_CHROME_LAYOUT");
        if (!value || !*value)
            return false;
        if (!g_ascii_strcasecmp(value, "resize"))
            return true;
        if (!g_ascii_strcasecmp(value, "inset") || !g_ascii_strcasecmp(value, "visual-inset") || !g_ascii_strcasecmp(value, "overlay"))
            return false;
        return strcmp(value, "0") && g_ascii_strcasecmp(value, "false") && g_ascii_strcasecmp(value, "off") && g_ascii_strcasecmp(value, "no");
    }();
    return enabled;
}

static PanelSize scanoutPanelForSource(uint32_t sourceWidth, uint32_t sourceHeight)
{
    PanelSize sourcePanel { sourceWidth, sourceHeight };
    if (!chromeLayoutResizeEnabled())
        return sourcePanel;

    auto panel = configuredPanelSize();
    if (panel.width >= sourceWidth && panel.height >= sourceHeight)
        return panel;
    return sourcePanel;
}

static const char* drmFitMode()
{
    static const char* mode = []() {
        const char* fit = getenv("WPE_DRM_FIT");
        return fit && *fit ? fit : "panel-native";
    }();
    return mode;
}

static uint32_t rotatedWidth(uint32_t width, uint32_t height, OutputRotation rotation)
{
    return rotation == OutputRotation::Rotate90 || rotation == OutputRotation::Rotate270 ? height : width;
}

static uint32_t rotatedHeight(uint32_t width, uint32_t height, OutputRotation rotation)
{
    return rotation == OutputRotation::Rotate90 || rotation == OutputRotation::Rotate270 ? width : height;
}

static bool parseRotation(const char* value, OutputRotation& rotation)
{
    if (!value || !*value)
        return false;
    char* end = nullptr;
    auto degrees = strtol(value, &end, 10);
    if (end == value)
        return false;
    switch (degrees) {
    case 0:
        rotation = OutputRotation::Rotate0;
        return true;
    case 90:
        rotation = OutputRotation::Rotate90;
        return true;
    case 180:
        rotation = OutputRotation::Rotate180;
        return true;
    case 270:
    case -90:
        rotation = OutputRotation::Rotate270;
        return true;
    default:
        return false;
    }
}

static OutputRotation configuredOutputRotation()
{
    OutputRotation rotation = OutputRotation::Rotate0;
    if (const char* value = getenv("WPE_DRM_ROTATION"))
        parseRotation(value, rotation);

    const char* path = getenv("WPE_DRM_ROTATION_FILE");
    if (!path || !*path)
        path = "/tmp/wpe-drm2-rotation";

    char* contents = nullptr;
    gsize length = 0;
    if (g_file_get_contents(path, &contents, &length, nullptr)) {
        auto* stripped = g_strstrip(contents);
        OutputRotation fileRotation;
        if (parseRotation(stripped, fileRotation))
            rotation = fileRotation;
        else if (*stripped)
            g_warning("Ignoring invalid WPE DRM rotation '%s' from %s", stripped, path);
    }
    g_free(contents);
    return rotation;
}

static uint32_t configuredMaxFPS()
{
    const char* value = getenv("WPE_DRM_MAX_FPS");
    if (!value || !*value)
        return 30;

    char* end = nullptr;
    auto fps = strtol(value, &end, 10);
    if (end == value || fps < 0)
        return 30;
    if (!fps)
        return 0;
    return static_cast<uint32_t>(std::min<long>(fps, 240));
}

static uint64_t drmPlaneRotate0Value()
{
#ifdef DRM_MODE_ROTATE_0
    return DRM_MODE_ROTATE_0;
#else
    return 1;
#endif
}

static uint32_t chromeReservedTopInset(uint32_t panelHeight);

static void fillARGB8888(uint8_t* destination, uint32_t destinationPitch, uint32_t width, uint32_t height, uint32_t color)
{
    for (uint32_t y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(y) * destinationPitch);
        std::fill(row, row + width, color);
    }
}

// 把一行 panel 坐标系的像素写入旋转后的目标缓冲。调用方保证
// panelY < panelHeight 且 x0+width <= panelWidth，因此无需逐像素做边界检查。
static void writeRotatedPanelRow(const uint32_t* sourceRow, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t panelY, uint32_t x0, uint32_t width)
{
    switch (rotation) {
    case OutputRotation::Rotate0: {
        auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(panelY) * destinationPitch);
        memcpy(row + x0, sourceRow, static_cast<size_t>(width) * 4);
        return;
    }
    case OutputRotation::Rotate90: {
        // dx = panelHeight-1-panelY（列固定），dy = x
        auto* column = destination + static_cast<size_t>(panelHeight - 1 - panelY) * 4 + static_cast<size_t>(x0) * destinationPitch;
        for (uint32_t x = 0; x < width; ++x)
            *reinterpret_cast<uint32_t*>(column + static_cast<size_t>(x) * destinationPitch) = sourceRow[x];
        return;
    }
    case OutputRotation::Rotate180: {
        auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(panelHeight - 1 - panelY) * destinationPitch);
        for (uint32_t x = 0; x < width; ++x)
            row[panelWidth - 1 - (x0 + x)] = sourceRow[x];
        return;
    }
    case OutputRotation::Rotate270: {
        // dx = panelY（列固定），dy = panelWidth-1-x
        auto* column = destination + static_cast<size_t>(panelY) * 4 + static_cast<size_t>(panelWidth - 1 - x0) * destinationPitch;
        for (uint32_t x = 0; x < width; ++x)
            *reinterpret_cast<uint32_t*>(column - static_cast<size_t>(x) * destinationPitch) = sourceRow[x];
        return;
    }
    }
}

// 把源坐标脏矩形变换到旋转后的目标缓冲坐标（用于填充、增量拷贝与 FB_DAMAGE_CLIPS）。
static drm_mode_rect rotatedDestRect(int x1, int y1, int x2, int y2, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset)
{
    int inset = static_cast<int>(topInset);
    switch (rotation) {
    case OutputRotation::Rotate0:
        return { x1, y1 + inset, x2, y2 + inset };
    case OutputRotation::Rotate90:
        return { static_cast<int>(panelHeight) - inset - y2, x1, static_cast<int>(panelHeight) - inset - y1, x2 };
    case OutputRotation::Rotate180:
        return { static_cast<int>(panelWidth) - x2, static_cast<int>(panelHeight) - inset - y2, static_cast<int>(panelWidth) - x1, static_cast<int>(panelHeight) - inset - y1 };
    case OutputRotation::Rotate270:
        return { inset + y1, static_cast<int>(panelWidth) - x2, inset + y2, static_cast<int>(panelWidth) - x1 };
    }
    return { x1, y1, x2, y2 };
}

// 填充 panel 坐标系矩形 [x1,x2)×[y1,y2)：先变换到目标坐标，再按目标行序
// std::fill，取代旧的逐 panel 行填充（90/270 度时那是每 4 字节跨一个 pitch
// 的散写，44 行让位带一帧就是 4 万多次 write-combining 缓冲打断）。
static void fillRotatedPanelRect(uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color)
{
    if (x2 <= x1 || y2 <= y1)
        return;
    auto rect = rotatedDestRect(x1, y1, x2, y2, panelWidth, panelHeight, rotation, 0);
    for (int dy = rect.y1; dy < rect.y2; ++dy) {
        auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(dy) * destinationPitch);
        std::fill(row + rect.x1, row + rect.x2, color);
    }
}

// 旋转写入的分块核心：把源图子矩形 [x0,x0+width)×[y0,y0+height)（源坐标，
// panel 行 = 源行 + topInset）旋转写入目标缓冲。与旧的 writeRotatedPanelRow
// 逐源行实现不同，这里按目标行序遍历（dumb buffer 是 write-combining 映射，
// 顺序写才能合并），90/270 度用 32x32 分块使源缓存行在块内被复用，
// 避免旧实现每写 4 字节跨一个 pitch 的读写放大。
static void rotateRegionARGB8888(const uint8_t* source, uint32_t sourceStride, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height)
{
    if (!width || !height)
        return;

    auto sourcePixel = [&](uint32_t x, uint32_t y) {
        return *reinterpret_cast<const uint32_t*>(source + static_cast<size_t>(y) * sourceStride + static_cast<size_t>(x) * 4);
    };
    constexpr uint32_t tileSize = 32;

    switch (rotation) {
    case OutputRotation::Rotate0:
        for (uint32_t y = 0; y < height; ++y)
            memcpy(destination + static_cast<size_t>(y0 + y + topInset) * destinationPitch + static_cast<size_t>(x0) * 4,
                source + static_cast<size_t>(y0 + y) * sourceStride + static_cast<size_t>(x0) * 4,
                static_cast<size_t>(width) * 4);
        return;
    case OutputRotation::Rotate180:
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t panelY = y0 + y + topInset;
            auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(panelHeight - 1 - panelY) * destinationPitch);
            const auto* sourceRow = reinterpret_cast<const uint32_t*>(source + static_cast<size_t>(y0 + y) * sourceStride);
            uint32_t destStart = panelWidth - x0 - width;
            for (uint32_t x = 0; x < width; ++x)
                row[destStart + x] = sourceRow[x0 + width - 1 - x];
        }
        return;
    case OutputRotation::Rotate90: {
        // dest(dx,dy) = src(x=dy, y=panelHeight-1-dx-topInset)
        uint32_t dyBegin = x0;
        uint32_t dyEnd = x0 + width;
        uint32_t dxBegin = panelHeight - topInset - y0 - height;
        uint32_t dxEnd = panelHeight - topInset - y0;
        for (uint32_t tileY = dyBegin; tileY < dyEnd; tileY += tileSize) {
            uint32_t tileYEnd = std::min(tileY + tileSize, dyEnd);
            for (uint32_t tileX = dxBegin; tileX < dxEnd; tileX += tileSize) {
                uint32_t tileXEnd = std::min(tileX + tileSize, dxEnd);
                for (uint32_t dy = tileY; dy < tileYEnd; ++dy) {
                    auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(dy) * destinationPitch);
                    for (uint32_t dx = tileX; dx < tileXEnd; ++dx)
                        row[dx] = sourcePixel(dy, panelHeight - 1 - dx - topInset);
                }
            }
        }
        return;
    }
    case OutputRotation::Rotate270: {
        // dest(dx,dy) = src(x=panelWidth-1-dy, y=dx-topInset)
        uint32_t dyBegin = panelWidth - x0 - width;
        uint32_t dyEnd = panelWidth - x0;
        uint32_t dxBegin = topInset + y0;
        uint32_t dxEnd = topInset + y0 + height;
        for (uint32_t tileY = dyBegin; tileY < dyEnd; tileY += tileSize) {
            uint32_t tileYEnd = std::min(tileY + tileSize, dyEnd);
            for (uint32_t tileX = dxBegin; tileX < dxEnd; tileX += tileSize) {
                uint32_t tileXEnd = std::min(tileX + tileSize, dxEnd);
                for (uint32_t dy = tileY; dy < tileYEnd; ++dy) {
                    auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(dy) * destinationPitch);
                    for (uint32_t dx = tileX; dx < tileXEnd; ++dx)
                        row[dx] = sourcePixel(panelWidth - 1 - dy, dx - topInset);
                }
            }
        }
        return;
    }
    }
}

static void copyRotatedARGB8888(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t sourceStride, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation)
{
    auto destinationWidth = rotatedWidth(panelWidth, panelHeight, rotation);
    auto destinationHeight = rotatedHeight(panelWidth, panelHeight, rotation);
    auto topInset = chromeReservedTopInset(panelHeight);
    if (topInset || panelWidth != sourceWidth || panelHeight != sourceHeight) {
        uint32_t copyWidth = std::min(sourceWidth, panelWidth);
        uint32_t availableHeight = panelHeight > topInset ? panelHeight - topInset : 0;
        uint32_t copyHeight = std::min(sourceHeight, availableHeight);
        if (!copyWidth || !copyHeight) {
            fillARGB8888(destination, destinationPitch, destinationWidth, destinationHeight, 0xff000000);
            return;
        }

        // 只清网页内容覆盖不到的区域（顶部 chrome 让位带 + 右侧信箱带），
        // 不再每帧整幅清屏后重画。
        fillRotatedPanelRect(destination, destinationPitch, panelWidth, panelHeight, rotation, 0, 0, panelWidth, topInset, 0xff000000);
        if (copyWidth < panelWidth)
            fillRotatedPanelRect(destination, destinationPitch, panelWidth, panelHeight, rotation, copyWidth, topInset, panelWidth, panelHeight, 0xff000000);

        rotateRegionARGB8888(source, sourceStride, destination, destinationPitch, panelWidth, panelHeight, rotation, topInset, 0, 0, copyWidth, copyHeight);
        if (copyHeight < availableHeight) {
            const auto* sourceRow = reinterpret_cast<const uint32_t*>(source + static_cast<size_t>(copyHeight - 1) * sourceStride);
            for (uint32_t panelY = topInset + copyHeight; panelY < topInset + availableHeight; ++panelY)
                writeRotatedPanelRow(sourceRow, destination, destinationPitch, panelWidth, panelHeight, rotation, panelY, 0, copyWidth);
        }
        return;
    }

    rotateRegionARGB8888(source, sourceStride, destination, destinationPitch, panelWidth, panelHeight, rotation, 0, 0, 0, sourceWidth, sourceHeight);
}

struct ChromeRenderState {
    bool enabled { true };
    bool visible { true };
    bool loading { false };
    double loadProgress { 0.0 };
    bool canBack { false };
    bool canForward { false };
    bool touchDebug { false };
    unsigned height { 44 };
    gint64 transitionUS { 0 };
    unsigned tabCount { 1 };
    unsigned activeTab { 0 };
    char panel[32] { "none" };
    char url[256] { };
    char title[128] { };
    char lines[10][96] { };
    unsigned lineCount { 0 };
};

static bool chromeEnabled()
{
    static bool enabled = []() {
        const char* value = getenv("WPE_CHROME_ENABLED");
        if (!value || !*value)
            return true;
        return strcmp(value, "0") && g_ascii_strcasecmp(value, "false") && g_ascii_strcasecmp(value, "off");
    }();
    return enabled;
}

static const char* chromeRenderStatePath()
{
    static const char* statePath = []() {
        const char* path = getenv("WPE_CHROME_RENDER_STATE");
        return path && *path ? path : "/tmp/wpe-drm2-chrome-state.ini";
    }();
    return statePath;
}

static long chromeAnimationDurationMS()
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

static ChromeRenderState readChromeRenderState()
{
    ChromeRenderState state;
    state.enabled = chromeEnabled();
    if (!state.enabled)
        return state;

    GError* error = nullptr;
    GKeyFile* keyFile = g_key_file_new();
    if (!g_key_file_load_from_file(keyFile, chromeRenderStatePath(), G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        g_key_file_unref(keyFile);
        return state;
    }

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
    copyKeyString(keyFile, "chrome", "url", state.url, sizeof(state.url));
    copyKeyString(keyFile, "chrome", "title", state.title, sizeof(state.title));

    if (g_key_file_has_group(keyFile, "panel")) {
        auto count = std::clamp<int>(g_key_file_get_integer(keyFile, "panel", "line_count", nullptr), 0, 10);
        state.lineCount = count;
        for (int i = 0; i < count; ++i) {
            char key[16];
            snprintf(key, sizeof(key), "line%d", i);
            copyKeyString(keyFile, "panel", key, state.lines[i], sizeof(state.lines[i]));
        }
    }

    g_key_file_unref(keyFile);
    return state;
}

static guint chromeRenderStateHash()
{
    char* contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(chromeRenderStatePath(), &contents, &length, nullptr))
        return 0;
    guint hash = g_str_hash(contents);
    hash ^= static_cast<guint>(length);
    g_free(contents);
    return hash;
}

// chrome 渲染状态缓存：INI 只在文件真正变化时重新读取解析。
// 探测分两级：先 stat（写端 g_file_set_contents 走临时文件+rename，
// inode 必变），未变直接返回，空闲时把原先每 33ms 的全量读+hash
// 降为一次 stat；stat 变了再读内容比对 hash，内容相同也不重新解析。
struct ChromeStateCache {
    ChromeRenderState state;
    guint hash { 0 };
    gint64 stampMTime { -1 };
    gint64 stampInode { -1 };
    gint64 stampSize { -1 };
    bool valid { false };
};

static ChromeStateCache& chromeStateCache()
{
    static ChromeStateCache cache;
    return cache;
}

// 由 chrome 轮询调用：探测文件变化并刷新缓存，返回是否有变化。
static bool refreshChromeRenderState()
{
    auto& cache = chromeStateCache();
    GStatBuf st;
    if (!g_stat(chromeRenderStatePath(), &st)) {
        if (cache.valid && static_cast<gint64>(st.st_mtime) == cache.stampMTime
            && static_cast<gint64>(st.st_ino) == cache.stampInode
            && static_cast<gint64>(st.st_size) == cache.stampSize)
            return false;
        cache.stampMTime = st.st_mtime;
        cache.stampInode = st.st_ino;
        cache.stampSize = st.st_size;
    }
    guint hash = chromeRenderStateHash();
    if (cache.valid && hash == cache.hash)
        return false;
    cache.hash = hash;
    cache.state = readChromeRenderState();
    cache.valid = true;
    return true;
}

static const ChromeRenderState& cachedChromeRenderState()
{
    auto& cache = chromeStateCache();
    if (!cache.valid)
        refreshChromeRenderState();
    return cache.state;
}

static double chromeShownFraction(const ChromeRenderState& chrome)
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

static uint32_t chromeReservedTopInset(uint32_t panelHeight)
{
    if (!panelHeight)
        return 0;
    const auto& chrome = cachedChromeRenderState();
    if (!chrome.enabled)
        return 0;
    auto chromeHeight = std::min<uint32_t>(std::clamp<unsigned>(chrome.height, 24, 80), panelHeight - 1);
    bool hasPanel = chrome.panel[0] && strcmp(chrome.panel, "none");
    if (chromeLayoutResizeEnabled())
        return (chrome.visible || hasPanel) ? chromeHeight : 0;

    return std::min<uint32_t>(chromeHeight, static_cast<uint32_t>(std::lround(chromeHeight * chromeShownFraction(chrome))));
}

static bool chromeAnimationActive()
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
    }

    void setPixel(int x, int y, uint32_t color)
    {
        if (x < 0 || y < 0 || x >= static_cast<int>(m_panelWidth) || y >= static_cast<int>(m_panelHeight))
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

    // 大块填充（工具栏底色、按钮、面板背景）按目标行序整段 std::fill；
    // 旧实现逐像素 setPixel，90/270 度下等于每 4 字节打断一次 WC 缓冲。
    void fillRect(int x, int y, int width, int height, uint32_t color)
    {
        int x1 = std::max(x, 0);
        int y1 = std::max(y, 0);
        int x2 = std::min(x + width, static_cast<int>(m_panelWidth));
        int y2 = std::min(y + height, static_cast<int>(m_panelHeight));
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

    void drawText(int x, int y, const char* text, uint32_t color, int scale, int maxWidth)
    {
        if (!text)
            return;
        int cursor = x;
        int advance = 6 * scale;
        for (const char* p = text; *p && cursor + advance <= x + maxWidth; ++p) {
            drawChar(cursor, y, *p, color, scale);
            cursor += advance;
        }
    }

private:
    uint8_t* m_destination { nullptr };
    uint32_t m_destinationPitch { 0 };
    uint32_t m_destinationWidth { 0 };
    uint32_t m_destinationHeight { 0 };
    uint32_t m_panelWidth { 0 };
    uint32_t m_panelHeight { 0 };
    OutputRotation m_rotation { OutputRotation::Rotate0 };
};

static void drawChromeOverlay(uint8_t* destination, uint32_t destinationPitch, uint32_t destinationWidth, uint32_t destinationHeight, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation)
{
    const auto& chrome = cachedChromeRenderState();
    if (!chrome.enabled)
        return;
    static bool loggedChromeOverlay;
    if (!loggedChromeOverlay) {
        loggedChromeOverlay = true;
        g_message("WPEViewDRM chrome_overlay=enabled state=%s", chromeRenderStatePath());
    }

    PanelPixelWriter painter(destination, destinationPitch, destinationWidth, destinationHeight, panelWidth, panelHeight, rotation);
    const uint32_t black = 0xff111820;
    const uint32_t dark = 0xee202a33;
    const uint32_t mid = 0xff53606b;
    const uint32_t white = 0xffffffff;
    const uint32_t text = 0xff1a222b;
    const uint32_t disabled = 0xff7f8a94;
    const uint32_t accent = 0xff1d8dbb;
    const uint32_t progressBlue = 0xff4285f4;
    const uint32_t panel = 0xf8f5f7fa;

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
    painter.fillRect(8, toolbarY + 6, std::max<int>(1, panelWidth / 2 - 16), chromeHeight - 12, white);
    painter.strokeRect(8, toolbarY + 6, std::max<int>(1, panelWidth / 2 - 16), chromeHeight - 12, mid);
    painter.drawText(18, toolbarY + 17, chrome.url[0] ? chrome.url : "HOME", text, 1, std::max<int>(1, panelWidth / 2 - 36));

    int buttonX = panelWidth / 2 + 8;
    int buttonW = std::max<int>(36, (static_cast<int>(panelWidth) - buttonX - 8) / 6);
    const char* labels[6] = { "<", ">", "TAB", "+", chrome.loading ? "X" : "R", "SET" };
    bool enabled[6] = { chrome.canBack, chrome.canForward, true, true, true, true };
    for (int i = 0; i < 6; ++i) {
        int x = buttonX + i * buttonW;
        painter.fillRect(x + 2, toolbarY + 6, buttonW - 4, chromeHeight - 12, black);
        painter.strokeRect(x + 2, toolbarY + 6, buttonW - 4, chromeHeight - 12, enabled[i] ? accent : mid);
        painter.drawText(x + 10, toolbarY + 17, labels[i], enabled[i] ? white : disabled, 1, buttonW - 16);
    }

    char tabLabel[32];
    snprintf(tabLabel, sizeof(tabLabel), "%u/%u", chrome.activeTab + 1, std::max<unsigned>(1, chrome.tabCount));
    painter.drawText(buttonX + buttonW * 2 + 8, toolbarY + chromeHeight - 12, tabLabel, white, 1, buttonW - 12);

    if (!hasPanel)
        return;

    int panelY = toolbarY + chromeHeight;
    int panelHeightPixels = std::min<int>(static_cast<int>(panelHeight) - panelY, 214);
    if (panelHeightPixels <= 0)
        return;
    painter.fillRect(0, panelY, panelWidth, panelHeightPixels, panel);
    painter.strokeRect(0, panelY, panelWidth, panelHeightPixels, mid);
    painter.drawText(14, panelY + 10, chrome.panel, accent, 1, panelWidth - 28);

    int rowY = panelY + 30;
    for (unsigned i = 0; i < chrome.lineCount && rowY + 24 <= panelY + panelHeightPixels; ++i) {
        uint32_t rowColor = (i % 2) ? 0xffedf1f5 : 0xffffffff;
        painter.fillRect(8, rowY, panelWidth - 16, 24, rowColor);
        painter.strokeRect(8, rowY, panelWidth - 16, 24, 0xffd4dbe2);
        painter.drawText(18, rowY + 8, chrome.lines[i], text, 1, panelWidth - 36);
        rowY += 28;
    }

    if (chrome.touchDebug)
        painter.drawText(panelWidth - 140, panelY + 10, "TOUCH DEBUG", accent, 1, 130);
}

class DRMScanoutBuffer;

// 挂在 WPEBuffer user_data 上的缓存：直扫路径缓存 scanout buffer（原有行为），
// 旋转路径缓存源 dmabuf 的 mmap 映射，避免每帧 mmap/munmap（WebKit 复用一个
// 小 buffer 池，映射可以跟随 buffer 生命周期）。
struct WPEBufferDRMUserData {
    DRMScanoutBuffer* scanoutBuffer { nullptr }; // owned
    void* sourceMapping { nullptr };
    size_t sourceMappingSize { 0 };
    ~WPEBufferDRMUserData();
};

static WPEBufferDRMUserData* ensureBufferUserData(WPEBuffer* buffer)
{
    auto* userData = static_cast<WPEBufferDRMUserData*>(wpe_buffer_get_user_data(buffer));
    if (!userData) {
        userData = new WPEBufferDRMUserData();
        wpe_buffer_set_user_data(buffer, userData, reinterpret_cast<GDestroyNotify>(+[](void* data) {
            delete static_cast<WPEBufferDRMUserData*>(data);
        }));
    }
    return userData;
}

class DRMScanoutBuffer {
public:
    enum class Kind : uint8_t {
        DMABufGBM,
        DMABufDirect,
        DMABufRotatedDumb,
        SHMRotatedDumb,
        SHMDumb
    };

    static std::unique_ptr<DRMScanoutBuffer> createDMABuf(std::unique_ptr<WPE::DRM::Buffer>&& buffer)
    {
        auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::DMABufGBM));
        scanoutBuffer->m_drmBuffer = WTF::move(buffer);
        return scanoutBuffer;
    }

    static std::unique_ptr<DRMScanoutBuffer> createDMABufDirect(int fd, WPEBuffer* buffer, GError** error)
    {
        auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);
        if (wpe_buffer_dma_buf_get_n_planes(dmaBuffer) != 1) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: direct dmabuf scanout requires one plane");
            return nullptr;
        }

        if (wpe_buffer_dma_buf_get_format(dmaBuffer) != DRM_FORMAT_ARGB8888) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: direct dmabuf scanout requires ARGB8888, got 0x%x", wpe_buffer_dma_buf_get_format(dmaBuffer));
            return nullptr;
        }

        if (wpe_buffer_dma_buf_get_modifier(dmaBuffer) != DRM_FORMAT_MOD_INVALID) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: direct dmabuf scanout requires linear modifier, got 0x%" G_GINT64_MODIFIER "x", static_cast<gint64>(wpe_buffer_dma_buf_get_modifier(dmaBuffer)));
            return nullptr;
        }

        uint32_t gemHandle = 0;
        if (drmPrimeFDToHandle(fd, wpe_buffer_dma_buf_get_fd(dmaBuffer, 0), &gemHandle)) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: drmPrimeFDToHandle failed: %s", strerror(errno));
            return nullptr;
        }

        auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::DMABufDirect));
        scanoutBuffer->m_fd = fd;
        scanoutBuffer->m_width = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
        scanoutBuffer->m_height = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
        scanoutBuffer->m_format = DRM_FORMAT_ARGB8888;
        scanoutBuffer->m_primeHandle = gemHandle;
        scanoutBuffer->m_pitch = wpe_buffer_dma_buf_get_stride(dmaBuffer, 0);

        uint32_t handles[4] = { scanoutBuffer->m_primeHandle, 0, 0, 0 };
        uint32_t strides[4] = { scanoutBuffer->m_pitch, 0, 0, 0 };
        uint32_t offsets[4] = { wpe_buffer_dma_buf_get_offset(dmaBuffer, 0), 0, 0, 0 };
        if (drmModeAddFB2(fd, scanoutBuffer->m_width, scanoutBuffer->m_height, scanoutBuffer->m_format, handles, strides, offsets, &scanoutBuffer->m_frameBufferID, 0)) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: direct dmabuf drmModeAddFB2 failed: %s", strerror(errno));
            return nullptr;
        }

        return scanoutBuffer;
    }

    static std::unique_ptr<DRMScanoutBuffer> createRotatedDMABufDumb(int fd, WPEBuffer* buffer, OutputRotation rotation, GError** error)
    {
        auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);
        if (wpe_buffer_dma_buf_get_n_planes(dmaBuffer) != 1) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render rotated buffer: dmabuf requires one plane");
            return nullptr;
        }
        if (wpe_buffer_dma_buf_get_format(dmaBuffer) != DRM_FORMAT_ARGB8888) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render rotated buffer: expected ARGB8888, got 0x%x", wpe_buffer_dma_buf_get_format(dmaBuffer));
            return nullptr;
        }

        uint32_t sourceWidth = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
        uint32_t sourceHeight = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
        auto panel = scanoutPanelForSource(sourceWidth, sourceHeight);
        auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::DMABufRotatedDumb));
        scanoutBuffer->m_sourceWidth = sourceWidth;
        scanoutBuffer->m_sourceHeight = sourceHeight;
        scanoutBuffer->m_rotation = rotation;
        if (!scanoutBuffer->initializeDumb(fd, rotatedWidth(panel.width, panel.height, rotation), rotatedHeight(panel.width, panel.height, rotation), error))
            return nullptr;
        if (!scanoutBuffer->copyRotatedFromDMABuf(buffer, rotation, error))
            return nullptr;
        return scanoutBuffer;
    }

    static std::unique_ptr<DRMScanoutBuffer> createSHMDumb(int fd, WPEBuffer* buffer, GError** error)
    {
        auto* shmBuffer = WPE_BUFFER_SHM(buffer);
        if (wpe_buffer_shm_get_format(shmBuffer) != WPE_PIXEL_FORMAT_ARGB8888) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: unsupported SHM format, expected ARGB8888");
            return nullptr;
        }

        auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::SHMDumb));
        if (!scanoutBuffer->initializeDumb(fd, static_cast<uint32_t>(wpe_buffer_get_width(buffer)), static_cast<uint32_t>(wpe_buffer_get_height(buffer)), error))
            return nullptr;

        if (!scanoutBuffer->copyFromSHM(buffer, error))
            return nullptr;

        return scanoutBuffer;
    }

    static std::unique_ptr<DRMScanoutBuffer> createRotatedSHMDumb(int fd, WPEBuffer* buffer, OutputRotation rotation, GError** error)
    {
        auto* shmBuffer = WPE_BUFFER_SHM(buffer);
        if (wpe_buffer_shm_get_format(shmBuffer) != WPE_PIXEL_FORMAT_ARGB8888) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render rotated SHM buffer: unsupported format, expected ARGB8888");
            return nullptr;
        }

        uint32_t sourceWidth = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
        uint32_t sourceHeight = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
        auto panel = scanoutPanelForSource(sourceWidth, sourceHeight);
        auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::SHMRotatedDumb));
        scanoutBuffer->m_sourceWidth = sourceWidth;
        scanoutBuffer->m_sourceHeight = sourceHeight;
        scanoutBuffer->m_rotation = rotation;
        if (!scanoutBuffer->initializeDumb(fd, rotatedWidth(panel.width, panel.height, rotation), rotatedHeight(panel.width, panel.height, rotation), error))
            return nullptr;
        if (!scanoutBuffer->copyRotatedFromSHM(buffer, rotation, error))
            return nullptr;
        return scanoutBuffer;
    }

    ~DRMScanoutBuffer()
    {
        if (m_mapping)
            munmap(m_mapping, m_size);

        if (m_frameBufferID)
            drmModeRmFB(m_fd, m_frameBufferID);

        if (m_dumbHandle) {
            struct drm_mode_destroy_dumb destroyDumb = { };
            destroyDumb.handle = m_dumbHandle;
            if (ioctl(m_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyDumb) < 0)
                g_warning("Failed to destroy DRM dumb buffer: %s", strerror(errno));
        }

        if (m_primeHandle) {
            struct drm_gem_close close = { };
            close.handle = m_primeHandle;
            if (ioctl(m_fd, DRM_IOCTL_GEM_CLOSE, &close) < 0)
                g_warning("Failed to close DRM GEM handle: %s", strerror(errno));
        }
    }

    Kind kind() const { return m_kind; }

    uint32_t frameBufferID() const
    {
        return m_drmBuffer ? m_drmBuffer->frameBufferID() : m_frameBufferID;
    }

    uint32_t width() const
    {
        return m_drmBuffer ? gbm_bo_get_width(m_drmBuffer->bufferObject()) : m_width;
    }

    uint32_t height() const
    {
        return m_drmBuffer ? gbm_bo_get_height(m_drmBuffer->bufferObject()) : m_height;
    }

    bool matchesSHMBuffer(WPEBuffer* buffer) const
    {
        return m_kind == Kind::SHMDumb
            && m_width == static_cast<uint32_t>(wpe_buffer_get_width(buffer))
            && m_height == static_cast<uint32_t>(wpe_buffer_get_height(buffer))
            && m_format == DRM_FORMAT_ARGB8888;
    }

    bool matchesRotatedDMABuf(WPEBuffer* buffer, OutputRotation rotation) const
    {
        auto sourceWidth = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
        auto sourceHeight = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
        auto panel = scanoutPanelForSource(sourceWidth, sourceHeight);
        return m_kind == Kind::DMABufRotatedDumb
            && m_sourceWidth == sourceWidth
            && m_sourceHeight == sourceHeight
            && m_width == rotatedWidth(panel.width, panel.height, rotation)
            && m_height == rotatedHeight(panel.width, panel.height, rotation)
            && m_format == DRM_FORMAT_ARGB8888
            && m_rotation == rotation;
    }

    bool matchesRotatedSHM(WPEBuffer* buffer, OutputRotation rotation) const
    {
        auto sourceWidth = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
        auto sourceHeight = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
        auto panel = scanoutPanelForSource(sourceWidth, sourceHeight);
        return m_kind == Kind::SHMRotatedDumb
            && m_sourceWidth == sourceWidth
            && m_sourceHeight == sourceHeight
            && m_width == rotatedWidth(panel.width, panel.height, rotation)
            && m_height == rotatedHeight(panel.width, panel.height, rotation)
            && m_format == DRM_FORMAT_ARGB8888
            && m_rotation == rotation;
    }

    bool initializeDumb(int fd, uint32_t width, uint32_t height, GError** error)
    {
        m_fd = fd;
        m_width = width;
        m_height = height;
        m_format = DRM_FORMAT_ARGB8888;

        struct drm_mode_create_dumb createDumb = { };
        createDumb.width = m_width;
        createDumb.height = m_height;
        createDumb.bpp = 32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &createDumb) < 0) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to create DRM dumb buffer: %s", strerror(errno));
            return false;
        }

        m_dumbHandle = createDumb.handle;
        m_pitch = createDumb.pitch;
        m_size = createDumb.size;

        uint32_t handles[4] = { m_dumbHandle, 0, 0, 0 };
        uint32_t strides[4] = { m_pitch, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        if (drmModeAddFB2(fd, m_width, m_height, m_format, handles, strides, offsets, &m_frameBufferID, 0)) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to add dumb framebuffer: %s", strerror(errno));
            return false;
        }

        struct drm_mode_map_dumb mapDumb = { };
        mapDumb.handle = m_dumbHandle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mapDumb) < 0) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to map DRM dumb buffer: %s", strerror(errno));
            return false;
        }

        m_mapping = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(mapDumb.offset));
        if (m_mapping == MAP_FAILED) {
            m_mapping = nullptr;
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to mmap DRM dumb buffer: %s", strerror(errno));
            return false;
        }
        return true;
    }

    bool copyFromSHM(WPEBuffer* buffer, GError** error)
    {
        auto* shmBuffer = WPE_BUFFER_SHM(buffer);
        if (wpe_buffer_shm_get_format(shmBuffer) != WPE_PIXEL_FORMAT_ARGB8888) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: unsupported SHM format, expected ARGB8888");
            return false;
        }

        auto sourceStride = wpe_buffer_shm_get_stride(shmBuffer);
        auto rowBytes = static_cast<size_t>(m_width) * 4;
        if (sourceStride < rowBytes) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: invalid SHM stride %u for %ux%u ARGB8888", sourceStride, m_width, m_height);
            return false;
        }

        gsize sourceSize = 0;
        const auto* source = static_cast<const uint8_t*>(g_bytes_get_data(wpe_buffer_shm_get_data(shmBuffer), &sourceSize));
        if (!source) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: SHM data is empty");
            return false;
        }

        auto requiredSize = static_cast<size_t>(sourceStride) * (m_height - 1) + rowBytes;
        if (sourceSize < requiredSize) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: SHM data too small: have %zu need %zu", static_cast<size_t>(sourceSize), requiredSize);
            return false;
        }

        auto* destination = static_cast<uint8_t*>(m_mapping);
        auto topInset = chromeReservedTopInset(m_height);
        if (topInset) {
            // 只清顶部 chrome 让位带；其余行随后被内容整行覆盖，无需先 memset。
            // 填色与旋转路径统一为不透明黑。
            for (uint32_t y = 0; y < topInset && y < m_height; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(y) * m_pitch);
                std::fill(row, row + m_width, 0xff000000);
            }
            uint32_t copyHeight = m_height > topInset ? m_height - topInset : 0;
            for (uint32_t y = 0; y < copyHeight; ++y)
                memcpy(destination + static_cast<size_t>(y + topInset) * m_pitch, source + static_cast<size_t>(y) * sourceStride, rowBytes);
        } else {
            for (uint32_t y = 0; y < m_height; ++y)
                memcpy(destination + static_cast<size_t>(y) * m_pitch, source + static_cast<size_t>(y) * sourceStride, rowBytes);
        }
        drawChromeOverlay(destination, m_pitch, m_width, m_height, m_width, m_height, OutputRotation::Rotate0);

        return true;
    }

    bool copyRotatedFromSHM(WPEBuffer* buffer, OutputRotation rotation, GError** error)
    {
        auto* shmBuffer = WPE_BUFFER_SHM(buffer);
        if (wpe_buffer_shm_get_format(shmBuffer) != WPE_PIXEL_FORMAT_ARGB8888) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate SHM buffer: unsupported format, expected ARGB8888");
            return false;
        }

        auto sourceWidth = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
        auto sourceHeight = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
        auto sourceStride = wpe_buffer_shm_get_stride(shmBuffer);
        auto rowBytes = static_cast<size_t>(sourceWidth) * 4;
        if (sourceStride < rowBytes) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate SHM buffer: invalid stride %u for %ux%u", sourceStride, sourceWidth, sourceHeight);
            return false;
        }

        gsize sourceSize = 0;
        const auto* source = static_cast<const uint8_t*>(g_bytes_get_data(wpe_buffer_shm_get_data(shmBuffer), &sourceSize));
        if (!source) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate SHM buffer: data is empty");
            return false;
        }

        auto requiredSize = static_cast<size_t>(sourceStride) * (sourceHeight - 1) + rowBytes;
        if (sourceSize < requiredSize) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate SHM buffer: data too small: have %zu need %zu", static_cast<size_t>(sourceSize), requiredSize);
            return false;
        }

        copyRotatedPixels(source, sourceWidth, sourceHeight, sourceStride, rotation);
        return true;
    }

    bool copyRotatedFromDMABuf(WPEBuffer* buffer, OutputRotation rotation, GError** error)
    {
        auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);
        auto sourceWidth = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
        auto sourceHeight = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
        auto sourceStride = wpe_buffer_dma_buf_get_stride(dmaBuffer, 0);
        auto sourceOffset = wpe_buffer_dma_buf_get_offset(dmaBuffer, 0);
        auto rowBytes = static_cast<size_t>(sourceWidth) * 4;
        if (sourceStride < rowBytes) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate dmabuf: invalid stride %u for %ux%u", sourceStride, sourceWidth, sourceHeight);
            return false;
        }

        size_t mapSize = static_cast<size_t>(sourceOffset) + static_cast<size_t>(sourceStride) * sourceHeight;
        int sourceFD = wpe_buffer_dma_buf_get_fd(dmaBuffer, 0);
        auto* userData = ensureBufferUserData(buffer);
        if (userData->sourceMapping && userData->sourceMappingSize != mapSize) {
            munmap(userData->sourceMapping, userData->sourceMappingSize);
            userData->sourceMapping = nullptr;
            userData->sourceMappingSize = 0;
        }
        if (!userData->sourceMapping) {
            void* mappedSource = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, sourceFD, 0);
            if (mappedSource == MAP_FAILED) {
                g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate dmabuf: mmap source failed: %s", strerror(errno));
                return false;
            }
            userData->sourceMapping = mappedSource;
            userData->sourceMappingSize = mapSize;
        }

        struct dma_buf_sync syncStart = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        ioctl(sourceFD, DMA_BUF_IOCTL_SYNC, &syncStart);

        const auto* source = static_cast<const uint8_t*>(userData->sourceMapping) + sourceOffset;
        copyRotatedPixels(source, sourceWidth, sourceHeight, sourceStride, rotation);

        struct dma_buf_sync syncEnd = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        ioctl(sourceFD, DMA_BUF_IOCTL_SYNC, &syncEnd);
        return true;
    }

    // 旋转 dumb buffer 的脏区簿记：两块缓冲轮换，每帧到来时把当前帧脏区累积到
    // 所有候选缓冲上；被选中写入的缓冲消费自己的累计脏区（= 相对其上次内容的
    // 全部差异），其余缓冲继续累积。无脏区信息或矩形过多时退化为整帧重拷。
    void accumulateSourceDamage(const Vector<drm_mode_rect>& rects)
    {
        if (m_pendingFullRepaint)
            return;
        if (rects.isEmpty() || m_pendingDamage.size() + rects.size() > 16) {
            m_pendingFullRepaint = true;
            m_pendingDamage.clear();
            return;
        }
        m_pendingDamage.appendVector(rects);
    }

    // 本次 copy 的目标坐标脏区：增量拷贝时为变换后的矩形（是相对上一次 scanout
    // 内容差异的超集，作 FB_DAMAGE_CLIPS 安全），整帧拷贝时为空（不带 clips）。
    Vector<drm_mode_rect> takeDestDamage() { return WTF::move(m_lastDestDamage); }

    void copyRotatedPixels(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t sourceStride, OutputRotation rotation)
    {
        auto* destination = static_cast<uint8_t*>(m_mapping);
        auto panel = scanoutPanelForSource(sourceWidth, sourceHeight);
        auto topInset = chromeReservedTopInset(panel.height);
        const auto& chrome = cachedChromeRenderState();
        guint chromeHash = chromeStateCache().hash;
        bool panelOpen = chrome.panel[0] && strcmp(chrome.panel, "none");
        uint32_t availableHeight = panel.height > topInset ? panel.height - topInset : 0;

        // 增量路径条件：本缓冲上帧内容有效且仅差累计脏区；chrome 状态与让位带
        // 未变（resize 布局下工具栏与内容区不相交，无需重画 overlay）；无面板
        // 悬浮在内容上、无显隐动画；源尺寸与内容区精确匹配（稳态）。
        bool partial = !m_pendingFullRepaint && !m_pendingDamage.isEmpty()
            && chromeLayoutResizeEnabled() && !panelOpen && !chromeAnimationActive()
            && chromeHash == m_lastChromeHash && topInset == m_lastTopInset
            && sourceWidth == panel.width && sourceHeight == availableHeight;

        m_lastDestDamage.clear();
        if (partial) {
            for (const auto& rect : m_pendingDamage) {
                int x1 = std::clamp<int>(rect.x1, 0, static_cast<int>(sourceWidth));
                int x2 = std::clamp<int>(rect.x2, 0, static_cast<int>(sourceWidth));
                int y1 = std::clamp<int>(rect.y1, 0, static_cast<int>(sourceHeight));
                int y2 = std::clamp<int>(rect.y2, 0, static_cast<int>(sourceHeight));
                if (x2 <= x1 || y2 <= y1)
                    continue;
                rotateRegionARGB8888(source, sourceStride, destination, m_pitch, panel.width, panel.height, rotation, topInset, x1, y1, x2 - x1, y2 - y1);
                m_lastDestDamage.append(rotatedDestRect(x1, y1, x2, y2, panel.width, panel.height, rotation, topInset));
            }
        } else {
            copyRotatedARGB8888(source, sourceWidth, sourceHeight, sourceStride, destination, m_pitch, panel.width, panel.height, rotation);
            drawChromeOverlay(destination, m_pitch, m_width, m_height, panel.width, panel.height, rotation);
        }
        m_pendingDamage.clear();
        m_pendingFullRepaint = false;
        m_lastTopInset = topInset;
        m_lastChromeHash = chromeHash;
    }

    void setFenceFD(UnixFileDescriptor&& fenceFD)
    {
        if (m_drmBuffer)
            m_drmBuffer->setFenceFD(WTF::move(fenceFD));
        else
            m_fenceFD = WTF::move(fenceFD);
    }

    const UnixFileDescriptor& fenceFD() const LIFETIME_BOUND
    {
        return m_drmBuffer ? m_drmBuffer->fenceFD() : m_fenceFD;
    }

private:
    explicit DRMScanoutBuffer(Kind kind)
        : m_kind(kind)
    {
    }

    Kind m_kind { Kind::DMABufGBM };
    std::unique_ptr<WPE::DRM::Buffer> m_drmBuffer;
    int m_fd { -1 };
    uint32_t m_width { 0 };
    uint32_t m_height { 0 };
    uint32_t m_sourceWidth { 0 };
    uint32_t m_sourceHeight { 0 };
    uint32_t m_format { DRM_FORMAT_INVALID };
    OutputRotation m_rotation { OutputRotation::Rotate0 };
    uint32_t m_dumbHandle { 0 };
    uint32_t m_primeHandle { 0 };
    uint32_t m_pitch { 0 };
    uint64_t m_size { 0 };
    uint32_t m_frameBufferID { 0 };
    void* m_mapping { nullptr };
    mutable UnixFileDescriptor m_fenceFD;
    Vector<drm_mode_rect> m_pendingDamage;
    bool m_pendingFullRepaint { true };
    uint32_t m_lastTopInset { 0 };
    guint m_lastChromeHash { 0 };
    Vector<drm_mode_rect> m_lastDestDamage;
};

WPEBufferDRMUserData::~WPEBufferDRMUserData()
{
    delete scanoutBuffer;
    if (sourceMapping)
        munmap(sourceMapping, sourceMappingSize);
}

/**
 * WPEViewDRM:
 *
 * A [class@WPEPlatform.View] implementation for DRM/KMS.
 *
 * [class@ViewDRM] is the [class@WPEPlatform.View] implementation used by
 * [class@DisplayDRM]. It displays the web view contents by scanning out
 * buffers directly to the DRM device.
 */
struct _WPEViewDRMPrivate {
    Seconds refreshDuration;
    std::optional<uint32_t> modeBlob;
    GRefPtr<WPEBuffer> pendingBuffer;
    GRefPtr<WPEBuffer> committedBuffer;
    GRefPtr<WPEBuffer> queuedBuffer;
    DRMScanoutBuffer* pendingScanoutBuffer { nullptr };
    DRMScanoutBuffer* committedScanoutBuffer { nullptr };
    std::array<std::unique_ptr<DRMScanoutBuffer>, 2> shmScanoutBuffers;
    std::array<std::unique_ptr<DRMScanoutBuffer>, 2> rotatedScanoutBuffers;
    unsigned nextSHMScanoutBufferIndex { 0 };
    unsigned nextRotatedScanoutBufferIndex { 0 };
    Vector<drm_mode_rect> damageRects;
    Vector<drm_mode_rect> queuedDamageRects;
    drmEventContext eventContext;
    GRefPtr<GSource> eventSource;
    GRefPtr<GSource> chromeSource;
    GRefPtr<GSource> frameThrottleSource;
    OptionSet<UpdateFlags> updateFlags;
    std::unique_ptr<RunLoop::Timer> cursorUpdateTimer;
    guint64 frameCount { 0 };
    guint64 pageFlipCount { 0 };
    bool lastCommitWasSynchronous { false };
    gint64 copyTotalUS { 0 };
    gint64 commitTotalUS { 0 };
    gint64 statsStartUS { 0 };
    gint64 lastFrameCommitUS { 0 };
    gint64 frameThrottleIntervalUS { 0 };
    unsigned chromePollTick { 0 };
    OutputRotation outputRotation { OutputRotation::Rotate0 };
};
WEBKIT_DEFINE_FINAL_TYPE(WPEViewDRM, wpe_view_drm, WPE_TYPE_VIEW, WPEView)

static void wpeViewDRMDidPageFlip(WPEViewDRM*);
static gboolean wpeViewDRMRequestUpdate(WPEViewDRM*, GError**);
static void wpeViewDRMCompleteSynchronousCommitIfNeeded(WPEViewDRM*);
static void setDamageRects(Vector<drm_mode_rect>&, const WPERectangle*, guint);

static void wpeViewDRMFinishBufferCommit(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (!priv->pendingBuffer)
        return;

    if (priv->committedBuffer)
        wpe_view_buffer_released(WPE_VIEW(view), priv->committedBuffer.get());
    priv->committedBuffer = WTF::move(priv->pendingBuffer);
    priv->committedScanoutBuffer = priv->pendingScanoutBuffer;
    priv->pendingScanoutBuffer = nullptr;
    wpe_view_buffer_rendered(WPE_VIEW(view), priv->committedBuffer.get());
    priv->frameCount++;

    if (!(priv->frameCount % 60)) {
        gint64 elapsedUS = g_get_monotonic_time() - priv->statsStartUS;
        if (elapsedUS <= 0)
            elapsedUS = 1;

        auto fps = static_cast<double>(priv->frameCount) * G_USEC_PER_SEC / elapsedUS;
        auto copyAverageMS = priv->copyTotalUS / 1000. / priv->frameCount;
        auto commitAverageMS = priv->commitTotalUS / 1000. / priv->frameCount;
        g_message("WPEViewDRM fps=%.1f copy_avg_ms=%.2f commit_avg_ms=%.2f frames=%" G_GUINT64_FORMAT " pageflips=%" G_GUINT64_FORMAT,
            fps, copyAverageMS, commitAverageMS, priv->frameCount, priv->pageFlipCount);
    }
}

static void wpeViewDRMQueueBuffer(WPEViewDRM* view, WPEBuffer* buffer, const WPERectangle* damageRects, guint nDamageRects)
{
    auto* priv = view->priv;
    if (priv->queuedBuffer && priv->queuedBuffer.get() != buffer)
        wpe_view_buffer_released(WPE_VIEW(view), priv->queuedBuffer.get());
    priv->queuedBuffer = buffer;
    setDamageRects(priv->queuedDamageRects, damageRects, nDamageRects);
    priv->updateFlags.add(UpdateFlags::BufferUpdatePending);
}

static bool wpeViewDRMThrottleDelay(WPEViewDRM* view, gint64& delayUS)
{
    auto* priv = view->priv;
    if (!priv->frameThrottleIntervalUS || !priv->lastFrameCommitUS)
        return false;

    auto elapsedUS = g_get_monotonic_time() - priv->lastFrameCommitUS;
    if (elapsedUS >= priv->frameThrottleIntervalUS)
        return false;

    delayUS = priv->frameThrottleIntervalUS - elapsedUS;
    return true;
}

static gboolean wpeViewDRMCommitQueuedBuffer(WPEViewDRM* view, GError** error)
{
    auto* priv = view->priv;
    if (!priv->queuedBuffer)
        return TRUE;

    priv->updateFlags.remove(UpdateFlags::BufferUpdatePending);
    priv->pendingBuffer = WTF::move(priv->queuedBuffer);
    priv->damageRects = WTF::move(priv->queuedDamageRects);
    if (wpeViewDRMRequestUpdate(view, error)) {
        priv->updateFlags.add(UpdateFlags::BufferUpdateRequested);
        wpeViewDRMCompleteSynchronousCommitIfNeeded(view);
        return TRUE;
    }
    return FALSE;
}

static void wpeViewDRMScheduleQueuedCommit(WPEViewDRM* view, gint64 delayUS)
{
    auto* priv = view->priv;
    if (priv->frameThrottleSource)
        return;

    guint delayMS = std::max<guint>(1, static_cast<guint>((delayUS + 999) / 1000));
    priv->frameThrottleSource = adoptGRef(g_timeout_source_new(delayMS));
    g_source_set_name(priv->frameThrottleSource.get(), "WPE DRM frame throttle");
    g_source_set_callback(priv->frameThrottleSource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(+[](gpointer userData) -> gboolean {
        auto* view = WPE_VIEW_DRM(userData);
        auto* priv = view->priv;
        priv->frameThrottleSource = nullptr;

        gint64 delayUS = 0;
        if (wpeViewDRMThrottleDelay(view, delayUS)) {
            wpeViewDRMScheduleQueuedCommit(view, delayUS);
            return G_SOURCE_REMOVE;
        }

        GError* error = nullptr;
        if (!wpeViewDRMCommitQueuedBuffer(view, &error)) {
            g_warning("WPEViewDRM throttled commit failed: %s", error ? error->message : "unknown");
            g_clear_error(&error);
        }
        return G_SOURCE_REMOVE;
    })), view, nullptr);
    g_source_attach(priv->frameThrottleSource.get(), g_main_context_get_thread_default());
}

static void wpeViewDRMCompleteSynchronousCommitIfNeeded(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (!std::exchange(priv->lastCommitWasSynchronous, false))
        return;

    priv->updateFlags.remove(UpdateFlags::BufferUpdateRequested);
    wpeViewDRMFinishBufferCommit(view);
}

static void wpeViewDRMConstructed(GObject* object)
{
    G_OBJECT_CLASS(wpe_view_drm_parent_class)->constructed(object);

    auto* view = WPE_VIEW(object);
    g_signal_connect(view, "notify::toplevel", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer) {
        auto* toplevel = wpe_view_get_toplevel(view);
        if (!toplevel) {
            wpe_view_unmap(view);
            return;
        }

        int width;
        int height;
        wpe_toplevel_get_size(toplevel, &width, &height);
        if (width && height)
            wpe_view_resized(view, width, height);

        wpe_view_map(view);
    }), nullptr);

    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(view));
    auto* priv = WPE_VIEW_DRM(view)->priv;
    priv->refreshDuration = Seconds(1 / (wpe_screen_get_refresh_rate(wpeDisplayDRMGetScreen(display)) / 1000.));
    priv->statsStartUS = g_get_monotonic_time();
    // 注意：生产 run.sh 设 WPE_DRM_MAX_FPS=0，此节流机制全程不生效，
    // 实际限帧由 WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS 承担。
    if (auto maxFPS = configuredMaxFPS())
        priv->frameThrottleIntervalUS = G_USEC_PER_SEC / maxFPS;

    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    priv->outputRotation = configuredOutputRotation();
    auto panel = configuredPanelSize();
    g_message("WPEViewDRM output_rotation=%u max_fps=%u panel=%ux%u fit=%s", static_cast<unsigned>(priv->outputRotation),
        priv->frameThrottleIntervalUS ? static_cast<unsigned>(G_USEC_PER_SEC / priv->frameThrottleIntervalUS) : 0,
        panel.width, panel.height, drmFitMode());
    priv->eventContext.version = DRM_EVENT_CONTEXT_VERSION;
    priv->eventContext.page_flip_handler = [](int, unsigned, unsigned, unsigned, void* userData) {
        wpeViewDRMDidPageFlip(WPE_VIEW_DRM(userData));
    };

    priv->eventSource = adoptGRef(g_unix_fd_source_new(fd, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP)));
    g_source_set_name(priv->eventSource.get(), "WPE DRM events");
    g_source_set_priority(priv->eventSource.get(), G_PRIORITY_DEFAULT);
    g_source_set_can_recurse(priv->eventSource.get(), TRUE);
    g_source_set_callback(priv->eventSource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(+[](int fd, GIOCondition condition, gpointer userData) -> gboolean {
        if (condition & (G_IO_ERR | G_IO_HUP))
            return G_SOURCE_REMOVE;

        if (condition & G_IO_IN) {
            auto* priv = WPE_VIEW_DRM(userData)->priv;
            drmHandleEvent(fd, &priv->eventContext);
        }
        return G_SOURCE_CONTINUE;
    })), object, nullptr);
    g_source_attach(priv->eventSource.get(), g_main_context_get_thread_default());

    refreshChromeRenderState();
    priv->chromeSource = adoptGRef(g_timeout_source_new(33));
    g_source_set_name(priv->chromeSource.get(), "WPE DRM chrome state poll");
    g_source_set_callback(priv->chromeSource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(+[](gpointer userData) -> gboolean {
        auto* view = WPE_VIEW_DRM(userData);
        auto* priv = view->priv;
        bool needsUpdate = refreshChromeRenderState() || chromeAnimationActive();

        // 旋转文件极少变化：与 chrome 轮询共用一个定时器，每 8 tick（约 264ms）读一次，
        // 取代原先独立的 250ms rotation GSource。
        if (!(++priv->chromePollTick % 8)) {
            auto rotation = configuredOutputRotation();
            if (rotation != priv->outputRotation) {
                priv->outputRotation = rotation;
                g_message("WPEViewDRM output_rotation=%u", static_cast<unsigned>(rotation));
                needsUpdate = true;
            }
        }

        if (!needsUpdate)
            return G_SOURCE_CONTINUE;
        if (priv->committedBuffer && !priv->updateFlags.contains(UpdateFlags::BufferUpdateRequested)) {
            if (wpeViewDRMRequestUpdate(view, nullptr)) {
                priv->updateFlags.add(UpdateFlags::BufferUpdateRequested);
                wpeViewDRMCompleteSynchronousCommitIfNeeded(view);
            }
        }
        return G_SOURCE_CONTINUE;
    })), object, nullptr);
    g_source_attach(priv->chromeSource.get(), g_main_context_get_thread_default());
}

static void wpeViewDRMDispose(GObject* object)
{
    auto* priv = WPE_VIEW_DRM(object)->priv;

    priv->cursorUpdateTimer = nullptr;

    if (priv->modeBlob) {
        auto fd = gbm_device_get_fd(wpe_display_drm_get_device(WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(object)))));
        drmModeDestroyPropertyBlob(fd, priv->modeBlob.value());
        priv->modeBlob = std::nullopt;
    }

    if (priv->eventSource) {
        g_source_destroy(priv->eventSource.get());
        priv->eventSource = nullptr;
    }

    if (priv->chromeSource) {
        g_source_destroy(priv->chromeSource.get());
        priv->chromeSource = nullptr;
    }

    if (priv->frameThrottleSource) {
        g_source_destroy(priv->frameThrottleSource.get());
        priv->frameThrottleSource = nullptr;
    }

    G_OBJECT_CLASS(wpe_view_drm_parent_class)->dispose(object);
}

static void logBufferPathOnce(DRMScanoutBuffer::Kind kind)
{
    static bool loggedDMABufGBM;
    static bool loggedDMABufDirect;
    static bool loggedDMABufRotated;
    static bool loggedSHMRotated;
    static bool loggedSHMDumb;

    if (kind == DRMScanoutBuffer::Kind::DMABufGBM) {
        if (!loggedDMABufGBM) {
            loggedDMABufGBM = true;
            g_message("WPEViewDRM buffer_path=dmabuf_gbm");
        }
        return;
    }

    if (kind == DRMScanoutBuffer::Kind::DMABufDirect) {
        if (!loggedDMABufDirect) {
            loggedDMABufDirect = true;
            g_message("WPEViewDRM buffer_path=dma_heap_scanout");
        }
        return;
    }

    if (kind == DRMScanoutBuffer::Kind::DMABufRotatedDumb) {
        if (!loggedDMABufRotated) {
            loggedDMABufRotated = true;
            g_message("WPEViewDRM buffer_path=dma_heap_rotated_dumb");
        }
        return;
    }

    if (kind == DRMScanoutBuffer::Kind::SHMRotatedDumb) {
        if (!loggedSHMRotated) {
            loggedSHMRotated = true;
            g_message("WPEViewDRM buffer_path=shm_rotated_dumb");
        }
        return;
    }

    if (!loggedSHMDumb) {
        loggedSHMDumb = true;
        g_message("WPEViewDRM buffer_path=shm_dumb");
    }
}

static bool forceDMAHeapBufferPath()
{
    static bool force = []() {
        const char* bufferPath = getenv("WPE_DRM_BUFFER_PATH");
        return bufferPath && !strcmp(bufferPath, "dma_heap");
    }();
    return force;
}

static DRMScanoutBuffer* drmBufferCreateDMABuf(WPEView* view, WPEBuffer* buffer, bool modifiersSupported, GError** error)
{
    auto* device = wpe_display_drm_get_device(WPE_DISPLAY_DRM(wpe_view_get_display(view)));
    auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);

    if (forceDMAHeapBufferPath()) {
        int fd = gbm_device_get_fd(device);
        auto scanoutBuffer = DRMScanoutBuffer::createDMABufDirect(fd, buffer, error);
        if (!scanoutBuffer)
            return nullptr;

        auto* userData = ensureBufferUserData(buffer);
        userData->scanoutBuffer = scanoutBuffer.release();
        logBufferPathOnce(DRMScanoutBuffer::Kind::DMABufDirect);
        return userData->scanoutBuffer;
    }

    struct gbm_bo* bo;
    if (modifiersSupported) {
        auto planeCount = wpe_buffer_dma_buf_get_n_planes(dmaBuffer);
        struct gbm_import_fd_modifier_data fdModifierData = {
            static_cast<uint32_t>(wpe_buffer_get_width(buffer)),
            static_cast<uint32_t>(wpe_buffer_get_height(buffer)),
            wpe_buffer_dma_buf_get_format(dmaBuffer),
            planeCount, {
                wpe_buffer_dma_buf_get_fd(dmaBuffer, 0),
                planeCount > 1 ? wpe_buffer_dma_buf_get_fd(dmaBuffer, 1) : -1,
                planeCount > 2 ? wpe_buffer_dma_buf_get_fd(dmaBuffer, 2) : -1,
                planeCount > 3 ? wpe_buffer_dma_buf_get_fd(dmaBuffer, 3) : -1
            }, {
                static_cast<int>(wpe_buffer_dma_buf_get_stride(dmaBuffer, 0)),
                planeCount > 1 ? static_cast<int>(wpe_buffer_dma_buf_get_stride(dmaBuffer, 1)) : 0,
                planeCount > 2 ? static_cast<int>(wpe_buffer_dma_buf_get_stride(dmaBuffer, 2)) : 0,
                planeCount > 3 ? static_cast<int>(wpe_buffer_dma_buf_get_stride(dmaBuffer, 3)) : 0
            }, {
                static_cast<int>(wpe_buffer_dma_buf_get_offset(dmaBuffer, 0)),
                planeCount > 1 ? static_cast<int>(wpe_buffer_dma_buf_get_offset(dmaBuffer, 1)) : 0,
                planeCount > 2 ? static_cast<int>(wpe_buffer_dma_buf_get_offset(dmaBuffer, 2)) : 0,
                planeCount > 3 ? static_cast<int>(wpe_buffer_dma_buf_get_offset(dmaBuffer, 3)) : 0
            },
            wpe_buffer_dma_buf_get_modifier(dmaBuffer)
        };
        bo = gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &fdModifierData, GBM_BO_USE_SCANOUT);
    } else {
        struct gbm_import_fd_data fdData = {
            wpe_buffer_dma_buf_get_fd(dmaBuffer, 0),
            static_cast<uint32_t>(wpe_buffer_get_width(buffer)),
            static_cast<uint32_t>(wpe_buffer_get_height(buffer)),
            wpe_buffer_dma_buf_get_stride(dmaBuffer, 0),
            wpe_buffer_dma_buf_get_format(dmaBuffer)
        };
        bo = gbm_bo_import(device, GBM_BO_IMPORT_FD, &fdData, GBM_BO_USE_SCANOUT);
    }
    if (!bo) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to import buffer for scanout");
        return nullptr;
    }

    auto drmBuffer = WPE::DRM::Buffer::create(bo);
    if (!drmBuffer) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to create DRM frame buffer");
        gbm_bo_destroy(bo);
        return nullptr;
    }

    auto scanoutBuffer = DRMScanoutBuffer::createDMABuf(WTF::move(drmBuffer));
    auto* userData = ensureBufferUserData(buffer);
    userData->scanoutBuffer = scanoutBuffer.release();
    logBufferPathOnce(DRMScanoutBuffer::Kind::DMABufGBM);
    return userData->scanoutBuffer;
}

static DRMScanoutBuffer* nextSHMDumbBuffer(WPEViewDRM* view, WPEBuffer* buffer, GError** error)
{
    auto* priv = view->priv;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));

    for (unsigned i = 0; i < priv->shmScanoutBuffers.size(); ++i) {
        auto index = (priv->nextSHMScanoutBufferIndex + i) % priv->shmScanoutBuffers.size();
        auto& candidate = priv->shmScanoutBuffers[index];
        if (candidate
            && (candidate.get() == priv->committedScanoutBuffer
                || (priv->pendingBuffer && candidate.get() == priv->pendingScanoutBuffer)))
            continue;

        if (!candidate || !candidate->matchesSHMBuffer(buffer)) {
            candidate = DRMScanoutBuffer::createSHMDumb(fd, buffer, error);
            if (!candidate)
                return nullptr;
        } else if (!candidate->copyFromSHM(buffer, error))
            return nullptr;

        priv->nextSHMScanoutBufferIndex = (index + 1) % priv->shmScanoutBuffers.size();
        logBufferPathOnce(DRMScanoutBuffer::Kind::SHMDumb);
        return candidate.get();
    }

    g_warning("SHM dumb buffers busy: committedScanout=%p pendingScanout=%p committedBuffer=%p pendingBuffer=%p",
        priv->committedScanoutBuffer, priv->pendingScanoutBuffer, priv->committedBuffer.get(), priv->pendingBuffer.get());
    g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: no reusable SHM dumb framebuffer available while page flip is pending");
    return nullptr;
}

static DRMScanoutBuffer* nextRotatedDMABufBuffer(WPEViewDRM* view, WPEBuffer* buffer, OutputRotation rotation, GError** error)
{
    auto* priv = view->priv;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));

    for (unsigned i = 0; i < priv->rotatedScanoutBuffers.size(); ++i) {
        auto index = (priv->nextRotatedScanoutBufferIndex + i) % priv->rotatedScanoutBuffers.size();
        auto& candidate = priv->rotatedScanoutBuffers[index];
        if (candidate
            && (candidate.get() == priv->committedScanoutBuffer
                || (priv->pendingBuffer && candidate.get() == priv->pendingScanoutBuffer)))
            continue;

        if (!candidate || !candidate->matchesRotatedDMABuf(buffer, rotation)) {
            candidate = DRMScanoutBuffer::createRotatedDMABufDumb(fd, buffer, rotation, error);
            if (!candidate)
                return nullptr;
        } else if (!candidate->copyRotatedFromDMABuf(buffer, rotation, error))
            return nullptr;

        priv->nextRotatedScanoutBufferIndex = (index + 1) % priv->rotatedScanoutBuffers.size();
        logBufferPathOnce(DRMScanoutBuffer::Kind::DMABufRotatedDumb);
        return candidate.get();
    }

    g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: no reusable rotated framebuffer available while page flip is pending");
    return nullptr;
}

static DRMScanoutBuffer* nextRotatedSHMBuffer(WPEViewDRM* view, WPEBuffer* buffer, OutputRotation rotation, GError** error)
{
    auto* priv = view->priv;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));

    for (unsigned i = 0; i < priv->rotatedScanoutBuffers.size(); ++i) {
        auto index = (priv->nextRotatedScanoutBufferIndex + i) % priv->rotatedScanoutBuffers.size();
        auto& candidate = priv->rotatedScanoutBuffers[index];
        if (candidate
            && (candidate.get() == priv->committedScanoutBuffer
                || (priv->pendingBuffer && candidate.get() == priv->pendingScanoutBuffer)))
            continue;

        if (!candidate || !candidate->matchesRotatedSHM(buffer, rotation)) {
            candidate = DRMScanoutBuffer::createRotatedSHMDumb(fd, buffer, rotation, error);
            if (!candidate)
                return nullptr;
        } else if (!candidate->copyRotatedFromSHM(buffer, rotation, error))
            return nullptr;

        priv->nextRotatedScanoutBufferIndex = (index + 1) % priv->rotatedScanoutBuffers.size();
        logBufferPathOnce(DRMScanoutBuffer::Kind::SHMRotatedDumb);
        return candidate.get();
    }

    g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: no reusable rotated SHM framebuffer available while page flip is pending");
    return nullptr;
}

static DRMScanoutBuffer* drmScanoutBufferForRender(WPEViewDRM* view, WPEBuffer* buffer, OutputRotation rotation, GError** error)
{
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));

    if (WPE_IS_BUFFER_DMA_BUF(buffer)) {
        if (rotation != OutputRotation::Rotate0)
            return nextRotatedDMABufBuffer(view, buffer, rotation, error);

        // 未旋转的 dmabuf 直接进 zero-copy scanout（无 CPU 合成步骤），
        // drawChromeOverlay() 只在 CPU 拷贝路径（SHM / 旋转 dumb）里调用，
        // 因此这条路径下不会画 WPE 侧工具栏。这是当前直扫设计的固有限制，
        // 不是遗漏：若默认配置（rotation=0 + WPE_DRM_BUFFER_PATH=dma_heap）
        // 需要工具栏常驻，要么强制走 dumb 合成路径，要么把工具栏做成独立 plane。
        auto* userData = static_cast<WPEBufferDRMUserData*>(wpe_buffer_get_user_data(buffer));
        auto* scanoutBuffer = userData ? userData->scanoutBuffer : nullptr;
        if (!scanoutBuffer)
            scanoutBuffer = drmBufferCreateDMABuf(WPE_VIEW(view), buffer, wpe_display_drm_supports_modifiers(display), error);
        return scanoutBuffer;
    }

    if (WPE_IS_BUFFER_SHM(buffer)) {
        if (rotation != OutputRotation::Rotate0)
            return nextRotatedSHMBuffer(view, buffer, rotation, error);
        return nextSHMDumbBuffer(view, buffer, error);
    }

    g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: unsupported buffer");
    return nullptr;
}

static bool drmAtomicAddProperty(drmModeAtomicReq* request, uint32_t id, const WPE::DRM::Property& property)
{
    if (!property.first)
        return false;

    return drmModeAtomicAddProperty(request, id, property.first, property.second) > 0;
}

static bool addCrtcProperties(drmModeAtomicReq* request, const WPE::DRM::Crtc& crtc, uint32_t modeID)
{
    auto properties = crtc.properties();
    properties.active.second = 1;
    properties.modeID.second = modeID;

    bool success = drmAtomicAddProperty(request, crtc.id(), properties.active);
    success &= drmAtomicAddProperty(request, crtc.id(), properties.modeID);
    return success;
}

static bool addConnectorProperties(drmModeAtomicReq* request, const WPE::DRM::Connector& connector, uint32_t crtcID)
{
    auto properties = connector.properties();
    properties.crtcID.second = crtcID;
    properties.linkStatus.second = DRM_MODE_LINK_STATUS_GOOD;

    bool success = drmAtomicAddProperty(request, connector.id(), properties.crtcID);
    success &= drmAtomicAddProperty(request, connector.id(), properties.linkStatus);
    return success;
}

WPE::DRM::Plane::Properties emptyPlaneProperties(const WPE::DRM::Plane& plane)
{
    auto properties = plane.properties();
    properties.crtcID.second = 0;
    properties.crtcX.second = 0;
    properties.crtcY.second = 0;
    properties.crtcW.second = 0;
    properties.crtcH.second = 0;
    properties.fbID.second = 0;
    properties.srcX.second = 0;
    properties.srcY.second = 0;
    properties.srcW.second = 0;
    properties.srcH.second = 0;
    properties.fbDamageClips.second = 0;
    properties.rotation.second = drmPlaneRotate0Value();
    return properties;
}

static bool shouldStretchToMode()
{
    return !strcmp(drmFitMode(), "stretch");
}

static bool shouldUsePanelNativeFit()
{
    return !strcmp(drmFitMode(), "panel-native");
}

static std::optional<uint32_t> configuredRotatedXOffset(uint32_t maxOffset)
{
    static std::optional<long> offset = []() -> std::optional<long> {
        const char* value = getenv("WPE_DRM_ROTATED_X");
        if (!value || !*value)
            return std::nullopt;
        char* end = nullptr;
        auto parsed = strtol(value, &end, 10);
        if (end == value || parsed < 0)
            return std::nullopt;
        return parsed;
    }();
    if (!offset)
        return std::nullopt;
    return std::min<uint32_t>(static_cast<uint32_t>(*offset), maxOffset);
}

static void destinationRectForBuffer(drmModeModeInfo* mode, const DRMScanoutBuffer& buffer, uint32_t& x, uint32_t& y, uint32_t& width, uint32_t& height)
{
    x = 0;
    y = 0;
    width = mode->hdisplay;
    height = mode->vdisplay;

    if (shouldUsePanelNativeFit()) {
        auto panel = configuredPanelSize();
        bool framebufferIsRotatedPanel = buffer.width() == panel.height && buffer.height() == panel.width;
        if (framebufferIsRotatedPanel) {
            width = std::min<uint32_t>(buffer.width(), mode->hdisplay);
            height = std::min<uint32_t>(buffer.height(), mode->vdisplay);
            uint32_t maxX = mode->hdisplay > width ? mode->hdisplay - width : 0;
            if (auto configuredX = configuredRotatedXOffset(maxX))
                x = configuredX.value();
            else
                x = maxX / 2;
            y = mode->vdisplay > height ? (mode->vdisplay - height) / 2 : 0;
        }
        return;
    }

    if (shouldStretchToMode() || !buffer.width() || !buffer.height())
        return;

    double scaleX = static_cast<double>(mode->hdisplay) / buffer.width();
    double scaleY = static_cast<double>(mode->vdisplay) / buffer.height();
    double scale = std::min(scaleX, scaleY);
    width = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(buffer.width() * scale)));
    height = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(buffer.height() * scale)));
    x = (mode->hdisplay - width) / 2;
    y = (mode->vdisplay - height) / 2;
}

WPE::DRM::Plane::Properties primaryPlaneProperties(const WPE::DRM::Plane& plane, uint32_t crtcID, drmModeModeInfo* mode, const DRMScanoutBuffer& buffer, std::optional<uint32_t> damageID)
{
    auto properties = plane.properties();
    uint32_t destinationX = 0;
    uint32_t destinationY = 0;
    uint32_t destinationWidth = 0;
    uint32_t destinationHeight = 0;
    destinationRectForBuffer(mode, buffer, destinationX, destinationY, destinationWidth, destinationHeight);
    properties.crtcID.second = crtcID;
    properties.crtcX.second = destinationX;
    properties.crtcY.second = destinationY;
    properties.crtcW.second = destinationWidth;
    properties.crtcH.second = destinationHeight;
    properties.fbID.second = buffer.frameBufferID();
    properties.srcX.second = 0;
    properties.srcY.second = 0;
    properties.srcW.second = (static_cast<uint64_t>(buffer.width()) << 16);
    properties.srcH.second = (static_cast<uint64_t>(buffer.height()) << 16);
    properties.rotation.second = drmPlaneRotate0Value();
    static guint64 commitLogCounter = 0;
    commitLogCounter++;
    if (commitLogCounter <= 8 || !(commitLogCounter % 600)) {
        auto panel = configuredPanelSize();
        g_message("WPEViewDRM commit fit=%s panel=%ux%u drm_mode=%ux%u framebuffer=%ux%u src=%ux%u crtc=%ux%u+%u+%u commit=%" G_GUINT64_FORMAT,
            drmFitMode(), panel.width, panel.height,
            mode->hdisplay, mode->vdisplay,
            buffer.width(), buffer.height(),
            buffer.width(), buffer.height(),
            destinationWidth, destinationHeight, destinationX, destinationY,
            commitLogCounter);
    }
    if (properties.fbDamageClips.first && damageID)
        properties.fbDamageClips.second = damageID.value();
    if (properties.inFenceFD.first) {
        if (const auto& inFenceFD = buffer.fenceFD())
            properties.inFenceFD.second = inFenceFD.value();
    }
    return properties;
}

WPE::DRM::Plane::Properties cursorPlaneProperties(uint32_t crtcID, const WPE::DRM::Cursor& cursor)
{
    auto properties = cursor.plane().properties();
    properties.crtcID.second = crtcID;
    properties.crtcX.second = cursor.x();
    properties.crtcY.second = cursor.y();
    properties.crtcW.second = gbm_bo_get_width(cursor.buffer()->bufferObject());
    properties.crtcH.second = gbm_bo_get_height(cursor.buffer()->bufferObject());
    properties.fbID.second = cursor.buffer()->frameBufferID();
    properties.srcX.second = 0;
    properties.srcY.second = 0;
    properties.srcW.second = (static_cast<uint64_t>(gbm_bo_get_width(cursor.buffer()->bufferObject())) << 16);
    properties.srcH.second = (static_cast<uint64_t>(gbm_bo_get_height(cursor.buffer()->bufferObject())) << 16);
    properties.rotation.second = drmPlaneRotate0Value();
    return properties;
}

static bool addPlaneProperties(drmModeAtomicReq* request, const WPE::DRM::Plane& plane, WPE::DRM::Plane::Properties&& properties)
{
    bool success = drmAtomicAddProperty(request, plane.id(), properties.crtcID);
    success &= drmAtomicAddProperty(request, plane.id(), properties.crtcX);
    success &= drmAtomicAddProperty(request, plane.id(), properties.crtcY);
    success &= drmAtomicAddProperty(request, plane.id(), properties.crtcW);
    success &= drmAtomicAddProperty(request, plane.id(), properties.crtcH);
    success &= drmAtomicAddProperty(request, plane.id(), properties.fbID);
    success &= drmAtomicAddProperty(request, plane.id(), properties.srcX);
    success &= drmAtomicAddProperty(request, plane.id(), properties.srcY);
    success &= drmAtomicAddProperty(request, plane.id(), properties.srcW);
    success &= drmAtomicAddProperty(request, plane.id(), properties.srcH);
    if (properties.fbDamageClips.first)
        success &= drmAtomicAddProperty(request, plane.id(), properties.fbDamageClips);
    if (properties.rotation.first)
        success &= drmAtomicAddProperty(request, plane.id(), properties.rotation);
    return success;
}

static bool bufferUsesSynchronousCommit(DRMScanoutBuffer* buffer)
{
    if (!buffer)
        return false;
    return buffer->kind() == DRMScanoutBuffer::Kind::SHMDumb
        || buffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb
        || buffer->kind() == DRMScanoutBuffer::Kind::DMABufRotatedDumb;
}

static bool wpeViewDRMCommitAtomic(WPEViewDRM* view, DRMScanoutBuffer* buffer, std::optional<uint32_t> damageID, GError** error)
{
    WPE::DRM::UniquePtr<drmModeAtomicReq> request(drmModeAtomicAlloc());
    bool synchronousCommit = bufferUsesSynchronousCommit(buffer);
    uint32_t flags = synchronousCommit ? 0 : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);

    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto* screen = WPE_SCREEN_DRM(wpeDisplayDRMGetScreen(display));
    auto& crtc = wpeScreenDRMGetCrtc(screen);
    auto* mode = wpeScreenDRMGetMode(screen);
    auto fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    if (!crtc.modeIsCurrent(mode)) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

        if (!view->priv->modeBlob) {
            uint32_t blobID;
            auto result = drmModeCreatePropertyBlob(fd, mode, sizeof(drmModeModeInfo), &blobID);
            if (result < 0) {
                g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to crate blob from DRM mode: %s", safeStrerror(-result).data());
                return false;
            }

            view->priv->modeBlob = blobID;
        }

        const auto& connector = wpeDisplayDRMGetConnector(display);
        bool success = addCrtcProperties(request.get(), crtc, view->priv->modeBlob.value());
        success &= addConnectorProperties(request.get(), connector, crtc.id());
        if (!success) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to set DRM mode");
            return false;
        }
    }

    auto& plane = wpeDisplayDRMGetPrimaryPlane(display);
    if (!addPlaneProperties(request.get(), plane, buffer ? primaryPlaneProperties(plane, crtc.id(), mode, *buffer, damageID) : emptyPlaneProperties(plane))) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to set plane properties");
        return false;
    }

    if (auto* cursor = wpeDisplayDRMGetCursor(display))
        addPlaneProperties(request.get(), cursor->plane(), cursor->buffer() ? cursorPlaneProperties(crtc.id(), *cursor) : emptyPlaneProperties(cursor->plane()));

    if (drmModeAtomicCommit(fd, request.get(), flags, view)) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to commit properties: %s", strerror(errno));
        return false;
    }

    if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET)
        crtc.setCurrentMode(mode);

    wpeScreenDRMDestroyDumbBufferIfNeeded(screen, fd);

    return true;
}

static bool wpeViewDRMCommitLegacy(WPEViewDRM* view, const DRMScanoutBuffer& buffer, GError** error)
{
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto* screen = WPE_SCREEN_DRM(wpeDisplayDRMGetScreen(display));
    auto& crtc = wpeScreenDRMGetCrtc(screen);
    auto* mode = wpeScreenDRMGetMode(screen);
    auto fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    if (!crtc.modeIsCurrent(mode)) {
        const auto& connector = wpeDisplayDRMGetConnector(display);
        auto connectorID = connector.id();
        if (drmModeSetCrtc(fd, crtc.id(), buffer.frameBufferID(), 0, 0, &connectorID, 1, mode)) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to set CRTC");
            return false;
        }

        crtc.setCurrentMode(mode);
    }

    // FIXME: support cursors in legacy mode.

    if (drmModePageFlip(fd, crtc.id(), buffer.frameBufferID(), DRM_MODE_PAGE_FLIP_EVENT, view)) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to request page flip");
        return false;
    }

    wpeScreenDRMDestroyDumbBufferIfNeeded(screen, fd);

    return true;
}

static std::pair<uint32_t, uint64_t> wpeBufferFormat(WPEBuffer* buffer)
{
    if (WPE_IS_BUFFER_DMA_BUF(buffer)) {
        auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);
        return { wpe_buffer_dma_buf_get_format(dmaBuffer), wpe_buffer_dma_buf_get_modifier(dmaBuffer) };
    }

    if (WPE_IS_BUFFER_SHM(buffer) && wpe_buffer_shm_get_format(WPE_BUFFER_SHM(buffer)) == WPE_PIXEL_FORMAT_ARGB8888)
        return { DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID };

    return { DRM_FORMAT_INVALID, DRM_FORMAT_MOD_INVALID };
}

static std::optional<uint32_t> buildDamageBlob(WPEDisplayDRM* display, const Vector<drm_mode_rect>& damageRects, GError** error)
{
    if (damageRects.isEmpty())
        return std::nullopt;

    uint32_t blobID;
    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    auto result = drmModeCreatePropertyBlob(fd, damageRects.span().data(), damageRects.sizeInBytes(), &blobID);
    if (result < 0) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: failed to crate damage blob: %s", safeStrerror(-result).data());
        return 0;
    }

    return blobID;
}

static void destroyDamageBlob(WPEDisplayDRM* display, uint32_t blobID)
{
    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    drmModeDestroyPropertyBlob(fd, blobID);
}

static void setDamageRects(Vector<drm_mode_rect>& destination, const WPERectangle* damageRects, guint nDamageRects)
{
    destination.clear();
    destination.reserveInitialCapacity(nDamageRects);
    for (unsigned i = 0; i < nDamageRects; ++i)
        destination.append({ damageRects[i].x, damageRects[i].y, damageRects[i].x + damageRects[i].width, damageRects[i].y + damageRects[i].height });
}

static gboolean wpeViewDRMRequestUpdate(WPEViewDRM* view, GError** error)
{
    auto* priv = view->priv;
    auto* buffer = priv->pendingBuffer ? priv->pendingBuffer.get() : priv->committedBuffer.get();
    DRMScanoutBuffer* drmBuffer = nullptr;
    auto rotation = priv->outputRotation;
    bool needsScanoutRebuild = buffer && (rotation != OutputRotation::Rotate0
        || (priv->committedScanoutBuffer
            && (priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::DMABufRotatedDumb
                || priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb)));
    if (priv->pendingBuffer || needsScanoutRebuild) {
        gint64 copyStartUS = g_get_monotonic_time();
        if (rotation != OutputRotation::Rotate0 && priv->pendingBuffer) {
            for (auto& candidate : priv->rotatedScanoutBuffers) {
                if (candidate)
                    candidate->accumulateSourceDamage(priv->damageRects);
            }
        }
        drmBuffer = drmScanoutBufferForRender(view, buffer, rotation, error);
        if (!drmBuffer)
            return FALSE;

        if (drmBuffer->kind() == DRMScanoutBuffer::Kind::SHMDumb
            || drmBuffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb
            || drmBuffer->kind() == DRMScanoutBuffer::Kind::DMABufRotatedDumb)
            priv->copyTotalUS += g_get_monotonic_time() - copyStartUS;
        if (rotation != OutputRotation::Rotate0)
            priv->damageRects = drmBuffer->takeDestDamage();

        if (priv->pendingBuffer) {
            drmBuffer->setFenceFD(UnixFileDescriptor { wpe_buffer_take_rendering_fence(buffer), UnixFileDescriptor::Adopt });
            priv->pendingScanoutBuffer = drmBuffer;
        } else
            priv->committedScanoutBuffer = drmBuffer;
    } else
        drmBuffer = priv->committedScanoutBuffer;

    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    gint64 commitStartUS = g_get_monotonic_time();
    if (wpe_display_drm_supports_atomic(display)) {
        auto damageID = drmBuffer ? buildDamageBlob(display, priv->damageRects, error) : std::nullopt;
        if (damageID.has_value() && !damageID.value())
            return FALSE;

        auto result = wpeViewDRMCommitAtomic(WPE_VIEW_DRM(view), drmBuffer, damageID, error);
        if (damageID)
            destroyDamageBlob(display, damageID.value());
        priv->damageRects.clear();
        priv->commitTotalUS += g_get_monotonic_time() - commitStartUS;
        if (result && drmBuffer)
            priv->lastFrameCommitUS = commitStartUS;
        priv->lastCommitWasSynchronous = result && bufferUsesSynchronousCommit(drmBuffer);
        return result;
    }

    if (!drmBuffer) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: no DRM buffer to commit");
        return FALSE;
    }

    auto result = wpeViewDRMCommitLegacy(WPE_VIEW_DRM(view), *drmBuffer, error);
    priv->commitTotalUS += g_get_monotonic_time() - commitStartUS;
    if (result)
        priv->lastFrameCommitUS = commitStartUS;
    return result;
}

static gboolean wpeViewDRMRenderBuffer(WPEView* view, WPEBuffer* buffer, const WPERectangle* damageRects, guint nDamageRects, GError** error)
{
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(view));
    auto& plane = wpeDisplayDRMGetPrimaryPlane(display);
    auto format = wpeBufferFormat(buffer);
    if (!plane.supportsFormat(format.first, format.second)) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: buffer format 0x%x modifier 0x%" G_GINT64_MODIFIER "x is not supported by DRM plane", format.first, static_cast<gint64>(format.second));
        return FALSE;
    }

    auto* priv = WPE_VIEW_DRM(view)->priv;
    if (priv->updateFlags.contains(UpdateFlags::CursorUpdateRequested) && !priv->committedBuffer) {
        priv->updateFlags.remove(UpdateFlags::CursorUpdateRequested);
        priv->updateFlags.remove(UpdateFlags::CursorUpdatePending);
    }
    if (priv->updateFlags.contains(UpdateFlags::BufferUpdateRequested)) {
        wpeViewDRMQueueBuffer(WPE_VIEW_DRM(view), buffer, damageRects, nDamageRects);
        return TRUE;
    }

    gint64 throttleDelayUS = 0;
    if (priv->frameThrottleSource || (priv->committedBuffer && wpeViewDRMThrottleDelay(WPE_VIEW_DRM(view), throttleDelayUS))) {
        wpeViewDRMQueueBuffer(WPE_VIEW_DRM(view), buffer, damageRects, nDamageRects);
        if (!priv->frameThrottleSource)
            wpeViewDRMScheduleQueuedCommit(WPE_VIEW_DRM(view), throttleDelayUS);
        return TRUE;
    }

    priv->pendingBuffer = buffer;
    setDamageRects(priv->damageRects, damageRects, nDamageRects);

    if (priv->cursorUpdateTimer)
        priv->cursorUpdateTimer->stop();
    if (wpeViewDRMRequestUpdate(WPE_VIEW_DRM(view), error)) {
        priv->updateFlags.add(UpdateFlags::BufferUpdateRequested);
        wpeViewDRMCompleteSynchronousCommitIfNeeded(WPE_VIEW_DRM(view));
        return TRUE;
    }

    return FALSE;
}

static void wpeViewDRMSetCursorFromName(WPEView* view, const char* name)
{
    if (auto* cursor = wpeDisplayDRMGetCursor(WPE_DISPLAY_DRM(wpe_view_get_display(view))))
        cursor->setFromName(name, wpe_view_get_scale(view));
}

static void wpeViewDRMSetCursorFromBytes(WPEView* view, GBytes* bytes, guint width, guint height, guint stride, guint hotspotX, guint hotspotY)
{
    if (auto* cursor = wpeDisplayDRMGetCursor(WPE_DISPLAY_DRM(wpe_view_get_display(view))))
        cursor->setFromBytes(bytes, width, height, stride, hotspotX, hotspotY);
}

static void wpeViewDRMScheduleCursorUpdate(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (priv->cursorUpdateTimer && priv->cursorUpdateTimer->isActive())
        return;

    if (!priv->cursorUpdateTimer) {
        priv->cursorUpdateTimer = makeUnique<RunLoop::Timer>(RunLoop::currentSingleton(), "_WPEViewDRMPrivate::cursorUpdateTimer"_s, [view] {
            if (wpeViewDRMRequestUpdate(view, nullptr))
                view->priv->updateFlags.add(UpdateFlags::CursorUpdateRequested);
        });
    }

    // Wait until the end of the frame to do the cursor update.
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto* screen = WPE_SCREEN_DRM(wpeDisplayDRMGetScreen(display));
    auto crtcIndex = wpeScreenDRMGetCrtc(screen).index();
    int crtcBitmask = 0;
    if (crtcIndex > 1)
        crtcBitmask = ((crtcIndex << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK);
    else if (crtcIndex > 0)
        crtcBitmask = DRM_VBLANK_SECONDARY;

    drmVBlank vblank;
    vblank.request.type = static_cast<drmVBlankSeqType>(DRM_VBLANK_RELATIVE | crtcBitmask);
    vblank.request.sequence = 0;
    vblank.request.signal = 0;
    drmWaitVBlank(gbm_device_get_fd(wpe_display_drm_get_device(display)), &vblank);

    auto lastVBlank = Seconds::fromMicroseconds(vblank.reply.tval_sec * G_USEC_PER_SEC + vblank.reply.tval_usec);
    auto elapsed = MonotonicTime::now().secondsSinceEpoch() - lastVBlank;
    priv->cursorUpdateTimer->startOneShot(priv->refreshDuration - elapsed - 1_ms);
}

static void wpeViewDRMDidPageFlip(WPEViewDRM* view)
{
    auto* priv = view->priv;
    auto updateFlags = std::exchange(priv->updateFlags, OptionSet<UpdateFlags> { });
    priv->pageFlipCount++;
    if (updateFlags.contains(UpdateFlags::BufferUpdateRequested))
        wpeViewDRMFinishBufferCommit(view);

    if (updateFlags.contains(UpdateFlags::BufferUpdatePending)) {
        gint64 delayUS = 0;
        if (wpeViewDRMThrottleDelay(view, delayUS)) {
            priv->updateFlags.add(UpdateFlags::BufferUpdatePending);
            wpeViewDRMScheduleQueuedCommit(view, delayUS);
        } else
            wpeViewDRMCommitQueuedBuffer(view, nullptr);
    } else if (updateFlags.contains(UpdateFlags::CursorUpdatePending))
        wpeViewDRMScheduleCursorUpdate(view);
}

void wpeViewDRMUpdateCursor(WPEViewDRM* view, double x, double y)
{
    auto* cursor = wpeDisplayDRMGetCursor(WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view))));
    if (!cursor)
        return;

    if (!cursor->setPosition(x, y))
        return;

    if (view->priv->updateFlags.containsAny({ UpdateFlags::CursorUpdateRequested, UpdateFlags::BufferUpdateRequested })) {
        view->priv->updateFlags.add(UpdateFlags::CursorUpdatePending);
        return;
    }

    wpeViewDRMScheduleCursorUpdate(view);
}

static void wpe_view_drm_class_init(WPEViewDRMClass* viewDRMClass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(viewDRMClass);
    objectClass->constructed = wpeViewDRMConstructed;
    objectClass->dispose = wpeViewDRMDispose;

    WPEViewClass* viewClass = WPE_VIEW_CLASS(viewDRMClass);
    viewClass->render_buffer = wpeViewDRMRenderBuffer;
    viewClass->set_cursor_from_name = wpeViewDRMSetCursorFromName;
    viewClass->set_cursor_from_bytes = wpeViewDRMSetCursorFromBytes;
}
