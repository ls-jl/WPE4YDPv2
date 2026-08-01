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

#include "ChromeMotionState.h"
#include "DRMUniquePtr.h"
#include "WPEDisplayDRMPrivate.h"
#include "WPEScreenDRMPrivate.h"
#include "WPEToplevelDRM.h"
#include "WPEBufferSHM.h"
#include "WPEViewDRMPrivate.h"
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <dlfcn.h>
#include <ft2build.h>
#include FT_FREETYPE_H
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
#include <unordered_map>
#include <vector>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
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

// Minimal Rockchip librga IM2D ABI. The declarations mirror the Apache-2.0
// upstream headers and keep the DRM backend independent from a build-time RGA
// dependency; the packaged runtime is loaded explicitly at run time.
// DRM ARGB/XRGB words are stored as B,G,R,A/X bytes on little-endian systems.
// RGA format names describe byte order, so use BGRA/BGRX for DRM buffers.
static constexpr int rkFormatBGRA8888 = 0x03 << 8;
static constexpr int rkFormatBGRX8888 = 0x16 << 8;
static constexpr int imTransformRotate90 = 1 << 0;
static constexpr int imTransformRotate180 = 1 << 1;
static constexpr int imTransformRotate270 = 1 << 2;

using RGABufferHandle = uint32_t;

struct RGAColorKeyRange {
    int max;
    int min;
};

struct RGANeuralNetwork {
    int scaleR;
    int scaleG;
    int scaleB;
    int offsetR;
    int offsetG;
    int offsetB;
};

struct RGABuffer {
    void* virtualAddress;
    void* physicalAddress;
    int fd;
    int width;
    int height;
    int widthStride;
    int heightStride;
    int format;
    int colorSpaceMode;
    union {
        int globalAlpha;
        struct {
            uint16_t alpha0;
            uint16_t alpha1;
        } alphaBit;
    };
    int readMode;
    int color;
    RGAColorKeyRange colorKeyRange;
    RGANeuralNetwork neuralNetwork;
    int ropCode;
    RGABufferHandle handle;
};

struct RGAHandleParameters {
    uint32_t width;
    uint32_t height;
    uint32_t format;
};

class RockchipRGA {
public:
    enum class Mode : uint8_t {
        Auto,
        Off,
        Required,
    };

    static RockchipRGA& singleton()
    {
        static RockchipRGA rga;
        return rga;
    }

    bool shouldAttempt()
    {
        return mode() != Mode::Off && !m_disabledAfterFailure && load();
    }

    bool required() const { return mode() == Mode::Required; }

    bool rotate(int sourceFD, uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t sourceStride, uint32_t sourceFormat, int destinationFD,
        uint32_t destinationWidth, uint32_t destinationHeight,
        uint32_t destinationStride, OutputRotation rotation, gint64& durationUS)
    {
        durationUS = 0;
        if (!shouldAttempt())
            return false;

        int sourceRGAFormat = drmFormatToRGA(sourceFormat);
        int transform = rotationToRGA(rotation);
        if (sourceRGAFormat < 0 || !transform)
            return fail("unsupported format or rotation");

        RGAHandleParameters sourceParameters {
            sourceStride / 4,
            sourceHeight,
            static_cast<uint32_t>(sourceRGAFormat),
        };
        RGAHandleParameters destinationParameters {
            destinationStride / 4,
            destinationHeight,
            static_cast<uint32_t>(rkFormatBGRA8888),
        };
        RGABufferHandle sourceHandle = m_importBufferFD(sourceFD, &sourceParameters);
        RGABufferHandle destinationHandle = m_importBufferFD(destinationFD, &destinationParameters);
        if (!sourceHandle || !destinationHandle) {
            if (destinationHandle)
                m_releaseBufferHandle(destinationHandle);
            if (sourceHandle)
                m_releaseBufferHandle(sourceHandle);
            return fail("importbuffer_fd failed");
        }

        auto source = m_wrapBufferHandle(sourceHandle, sourceWidth, sourceHeight,
            sourceStride / 4, sourceHeight, sourceRGAFormat);
        auto destination = m_wrapBufferHandle(destinationHandle,
            destinationWidth, destinationHeight, destinationStride / 4,
            destinationHeight, rkFormatBGRA8888);
        gint64 startUS = g_get_monotonic_time();
        int result = m_rotate(source, destination, transform, 1);
        durationUS = g_get_monotonic_time() - startUS;
        m_releaseBufferHandle(destinationHandle);
        m_releaseBufferHandle(sourceHandle);
        if (result <= 0)
            return fail("imrotate_t failed", result);

        m_consecutiveFailures = 0;
        return true;
    }

private:
    using ImportBufferFDFunction = RGABufferHandle (*)(int, RGAHandleParameters*);
    using WrapBufferHandleFunction = RGABuffer (*)(RGABufferHandle, int, int, int, int, int);
    using ReleaseBufferHandleFunction = int (*)(RGABufferHandle);
    using RotateFunction = int (*)(const RGABuffer, RGABuffer, int, int);

    static Mode mode()
    {
        static Mode configuredMode = []() {
            const char* value = g_getenv("WPE_DRM_RGA_ROTATION");
            if (value && (!g_ascii_strcasecmp(value, "off") || !strcmp(value, "0")))
                return Mode::Off;
            if (value && !g_ascii_strcasecmp(value, "required"))
                return Mode::Required;
            return Mode::Auto;
        }();
        return configuredMode;
    }

    static int drmFormatToRGA(uint32_t format)
    {
        if (format == DRM_FORMAT_ARGB8888)
            return rkFormatBGRA8888;
        if (format == DRM_FORMAT_XRGB8888)
            return rkFormatBGRX8888;
        return -1;
    }

    static int rotationToRGA(OutputRotation rotation)
    {
        switch (rotation) {
        case OutputRotation::Rotate90:
            return imTransformRotate90;
        case OutputRotation::Rotate180:
            return imTransformRotate180;
        case OutputRotation::Rotate270:
            return imTransformRotate270;
        case OutputRotation::Rotate0:
            return 0;
        }
        return 0;
    }

    bool load()
    {
        if (m_loadAttempted)
            return m_library;
        m_loadAttempted = true;
        const char* path = g_getenv("WPE_DRM_RGA_LIBRARY");
        if (!path || !*path) {
            g_message("WPEViewDRM rga_rotation=disabled reason=no-packaged-library");
            return false;
        }

        m_library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!m_library) {
            g_warning("WPEViewDRM rga_rotation=unavailable path=%s error=%s", path, dlerror());
            return false;
        }
        m_importBufferFD = reinterpret_cast<ImportBufferFDFunction>(dlsym(m_library, "importbuffer_fd"));
        m_wrapBufferHandle = reinterpret_cast<WrapBufferHandleFunction>(dlsym(m_library, "wrapbuffer_handle_t"));
        m_releaseBufferHandle = reinterpret_cast<ReleaseBufferHandleFunction>(dlsym(m_library, "releasebuffer_handle"));
        m_rotate = reinterpret_cast<RotateFunction>(dlsym(m_library, "imrotate_t"));
        if (!m_importBufferFD || !m_wrapBufferHandle || !m_releaseBufferHandle || !m_rotate) {
            g_warning("WPEViewDRM rga_rotation=unavailable path=%s error=missing-im2d-symbols", path);
            dlclose(m_library);
            m_library = nullptr;
            return false;
        }
        g_message("WPEViewDRM rga_rotation=available mode=%s path=%s",
            mode() == Mode::Required ? "required" : "auto", path);
        return true;
    }

    bool fail(const char* reason, int result = 0)
    {
        m_consecutiveFailures++;
        g_warning("WPEViewDRM rga_rotation_failed reason=%s result=%d consecutive=%u",
            reason, result, m_consecutiveFailures);
        if (mode() == Mode::Auto && m_consecutiveFailures >= 3) {
            m_disabledAfterFailure = true;
            g_warning("WPEViewDRM rga_rotation=disabled reason=repeated-failure cpu_fallback=1");
        }
        return false;
    }

    bool m_loadAttempted { false };
    bool m_disabledAfterFailure { false };
    unsigned m_consecutiveFailures { 0 };
    void* m_library { nullptr };
    ImportBufferFDFunction m_importBufferFD { nullptr };
    WrapBufferHandleFunction m_wrapBufferHandle { nullptr };
    ReleaseBufferHandleFunction m_releaseBufferHandle { nullptr };
    RotateFunction m_rotate { nullptr };
};

struct ChromeGlyph {
    int width { 0 };
    int height { 0 };
    int left { 0 };
    int top { 0 };
    int advance { 0 };
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
        if (found != font.glyphs.end())
            return &found->second;
        if (FT_Load_Char(font.face, character, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT))
            return nullptr;
        auto& bitmap = font.face->glyph->bitmap;
        ChromeGlyph glyph;
        glyph.width = bitmap.width;
        glyph.height = bitmap.rows;
        glyph.left = font.face->glyph->bitmap_left;
        glyph.top = font.face->glyph->bitmap_top;
        glyph.advance = std::max<int>(1, font.face->glyph->advance.x >> 6);
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
        if (font.glyphs.size() >= 512)
            font.glyphs.clear();
        return &font.glyphs.emplace(character, std::move(glyph)).first->second;
    }

    bool available(ChromeFontWeight weight = ChromeFontWeight::Regular) const
    {
        return m_fonts[static_cast<size_t>(weight)].face;
    }

private:
    struct Font {
        FT_Face face { nullptr };
        std::unordered_map<gunichar, ChromeGlyph> glyphs;
    };

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

static bool partialCopyEnabled()
{
    static bool enabled = []() {
        const char* value = getenv("WPE_DRM_PARTIAL_COPY");
        return value && (!strcmp(value, "1") || !g_ascii_strcasecmp(value, "true") || !g_ascii_strcasecmp(value, "yes"));
    }();
    return enabled;
}

static int renderingFenceTimeoutMS()
{
    static int timeout = []() {
        const char* value = getenv("WPE_DRM_FENCE_TIMEOUT_MS");
        if (!value || !*value)
            return 2000;
        char* end = nullptr;
        long parsed = strtol(value, &end, 10);
        return end != value && !*end ? static_cast<int>(std::clamp<long>(parsed, 1, 30000)) : 2000;
    }();
    return timeout;
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
    char theme[8] { "light" };
    char panel[32] { "none" };
    char panelTitle[96] { };
    char touchDebugLabel[32] { "TOUCH DEBUG" };
    char homeLabel[32] { "HOME" };
    char url[256] { };
    char title[128] { };
    char lines[10][96] { };
    unsigned lineCount { 0 };
    int panelX { 0 };
    int panelY { 44 };
    int panelWidth { 0 };
    int panelHeight { 0 };
    int panelHeaderHeight { 0 };
    int panelFooterHeight { 0 };
    int panelRowHeight { 48 };
    double panelScrollOffset { 0 };
    double panelMaxScroll { 0 };
    bool panelCanAdd { true };
    int panelColumns { 3 };
    int panelRows { 2 };
    int panelPageCount { 1 };
    char itemIDs[10][32] { };
    bool itemEnabled[10] { true, true, true, true, true, true, true, true, true, true };
    bool lineEnabled[10] { };
    bool lineDanger[10] { };
    bool lineChecked[10] { };
    char lineKinds[10][16] { };
    char lineValues[10][96] { };
    char lineIcons[10][24] { };
    int lineSegmentCount[10] { };
    char lineSegmentLabels[10][3][64] { };
    bool lineSegmentEnabled[10][3] { };
    bool lineSegmentDanger[10][3] { };
    int pressedRow { -1 };
    int pressedSegment { -1 };
    int pressedControl { -1 };
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
    for (unsigned line = 0; line < 10; ++line) {
        state.lineEnabled[line] = true;
        state.lineSegmentCount[line] = 1;
        for (unsigned segment = 0; segment < 3; ++segment)
            state.lineSegmentEnabled[line][segment] = true;
    }
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
    int legacyButtonX = panelWidth / 2 + 8;
    int buttonW = std::max<int>(36, (static_cast<int>(panelWidth) - legacyButtonX - 8) / 6);
    int buttonX = legacyButtonX + buttonW;
    int addressRight = buttonX - 8;
    int addressWidth = std::max<int>(1, addressRight - 8);
    int controlHeight = chromeHeight - 12;
    painter.drawRoundedRect(8, toolbarY + 6, addressWidth, controlHeight, 8, white, mid, 1);
    if (chrome.pressedControl == -2)
        painter.drawRoundedRect(8, toolbarY + 6, addressWidth, controlHeight, 8,
                                theme.pressed, theme.pressed, 1);
    painter.drawText(18, toolbarY + 17, chrome.url[0] ? chrome.url : chrome.homeLabel,
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
    for (int i = 0; i < 5; ++i) {
        int x = buttonX + i * buttonW;
        painter.drawRoundedRect(x + 2, toolbarY + 6, buttonW - 4, controlHeight, 6,
                                theme.surface, enabled[i] ? theme.outline : mid, 1);
        if (chrome.pressedControl == i)
            painter.drawRoundedRect(x + 2, toolbarY + 6, buttonW - 4,
                                    controlHeight, 6,
                                    theme.pressed, theme.pressed, 1);

        int centerX = x + buttonW / 2;
        int centerY = toolbarY + chromeHeight / 2;
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

class DRMScanoutBuffer;

// 挂在 WPEBuffer user_data 上的缓存：直扫路径缓存 scanout buffer（原有行为），
// 旋转路径缓存源 dmabuf 的 mmap 映射，避免每帧 mmap/munmap（WebKit 复用一个
// 小 buffer 池，映射可以跟随 buffer 生命周期）。
struct WPEBufferDRMUserData {
    DRMScanoutBuffer* scanoutBuffer { nullptr }; // owned
    void* sourceMapping { nullptr };
    size_t sourceMappingSize { 0 };
    uint32_t sourceMappingStride { 0 };
    void resetSourceMapping();
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
        SHMDumb,
        ChromeDumb
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

    static std::unique_ptr<DRMScanoutBuffer> createRotatedDMABufDumb(struct gbm_device* device, int fd, WPEBuffer* buffer, OutputRotation rotation, GError** error)
    {
        auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);
        if (wpe_buffer_dma_buf_get_n_planes(dmaBuffer) != 1) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render rotated buffer: dmabuf requires one plane");
            return nullptr;
        }
        auto sourceFormat = wpe_buffer_dma_buf_get_format(dmaBuffer);
        if (sourceFormat != DRM_FORMAT_ARGB8888 && sourceFormat != DRM_FORMAT_XRGB8888) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render rotated buffer: expected ARGB8888 or XRGB8888, got 0x%x", sourceFormat);
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
        if (!scanoutBuffer->copyRotatedFromDMABuf(device, buffer, rotation, error))
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

    static std::unique_ptr<DRMScanoutBuffer> createChromeDumb(int fd, uint32_t width, uint32_t height, GError** error)
    {
        auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::ChromeDumb));
        if (!scanoutBuffer->initializeDumb(fd, width, height, error))
            return nullptr;
        return scanoutBuffer;
    }

    ~DRMScanoutBuffer()
    {
        finishRGAAccess();
        if (m_rgaPrimeFD >= 0)
            close(m_rgaPrimeFD);

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

    bool copyRotatedFromDMABuf(struct gbm_device* device, WPEBuffer* buffer, OutputRotation rotation, GError** error)
    {
        finishRGAAccess();
        m_lastCopyUsedRGA = false;
        m_lastCopyFellBackFromRGA = false;
        m_lastRGADurationUS = 0;
        auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);
        auto sourceFormat = wpe_buffer_dma_buf_get_format(dmaBuffer);
        if (sourceFormat != DRM_FORMAT_ARGB8888 && sourceFormat != DRM_FORMAT_XRGB8888) {
            g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate dmabuf: expected ARGB8888 or XRGB8888, got 0x%x", sourceFormat);
            return false;
        }
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
        auto panel = scanoutPanelForSource(sourceWidth, sourceHeight);
        bool rgaGeometrySupported = panel.width == sourceWidth
            && panel.height == sourceHeight;
        auto& rga = RockchipRGA::singleton();
        if (rgaGeometrySupported && rga.shouldAttempt()) {
            if (m_rgaPrimeFD < 0) {
                if (drmPrimeHandleToFD(m_fd, m_dumbHandle, DRM_CLOEXEC | DRM_RDWR,
                        &m_rgaPrimeFD)) {
                    g_warning("WPEViewDRM rga_destination_export_failed handle=%u error=%s",
                        m_dumbHandle, strerror(errno));
                    m_rgaPrimeFD = -1;
                }
            }
            if (m_rgaPrimeFD >= 0 && rga.rotate(sourceFD, sourceWidth, sourceHeight,
                    sourceStride, sourceFormat, m_rgaPrimeFD, m_width, m_height,
                    m_pitch, rotation, m_lastRGADurationUS)) {
                struct dma_buf_sync syncStart = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
                if (ioctl(m_rgaPrimeFD, DMA_BUF_IOCTL_SYNC, &syncStart) < 0)
                    g_warning("WPEViewDRM rga destination sync start failed: %s", strerror(errno));
                else
                    m_rgaCPUAccessActive = true;
                m_lastCopyUsedRGA = true;
                m_lastCopyWasPartial = false;
                m_lastDestDamage.clear();
                m_pendingDamage.clear();
                m_pendingFullRepaint = false;
                m_lastTopInset = 0;
                m_lastChromeHash = chromeStateCache().hash;
                return true;
            }
            m_lastCopyFellBackFromRGA = true;
            if (rga.required()) {
                g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
                    "Failed to rotate dmabuf: required RGA path unavailable");
                return false;
            }
        } else if (rga.required()) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
                "Failed to rotate dmabuf: required RGA path does not support current geometry");
            return false;
        }

        auto* userData = ensureBufferUserData(buffer);
        if (userData->sourceMapping && userData->sourceMappingSize != mapSize)
            userData->resetSourceMapping();
        if (!userData->sourceMapping) {
            void* mappedSource = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, sourceFD, 0);
            if (mappedSource == MAP_FAILED) {
                int mmapError = errno;
                if (sourceOffset) {
                    g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate dmabuf: mmap failed (%s) and GBM map does not support source offset %u", strerror(mmapError), sourceOffset);
                    return false;
                }

                struct gbm_import_fd_data importData = {
                    sourceFD,
                    sourceWidth,
                    sourceHeight,
                    sourceStride,
                    sourceFormat
                };
                struct gbm_bo* sourceBO = gbm_bo_import(device, GBM_BO_IMPORT_FD, &importData, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
                if (!sourceBO) {
                    g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate dmabuf: mmap failed (%s) and GBM import failed", strerror(mmapError));
                    return false;
                }

                uint32_t mapStride = 0;
                void* mapData = nullptr;
                void* sourceMapping = gbm_bo_map(sourceBO, 0, 0, sourceWidth, sourceHeight, GBM_BO_TRANSFER_READ, &mapStride, &mapData);
                if (!sourceMapping || mapStride < rowBytes) {
                    g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to rotate dmabuf: mmap failed (%s) and gbm_bo_map failed (stride %u)", strerror(mmapError), mapStride);
                    if (sourceMapping)
                        gbm_bo_unmap(sourceBO, mapData);
                    gbm_bo_destroy(sourceBO);
                    return false;
                }

                static bool loggedGBMMap = false;
                if (!loggedGBMMap) {
                    loggedGBMMap = true;
                    g_message("WPEViewDRM dmabuf_cpu_map=gbm_bo_map transient=1");
                }

                struct dma_buf_sync syncStart = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
                ioctl(sourceFD, DMA_BUF_IOCTL_SYNC, &syncStart);
                copyRotatedPixels(static_cast<const uint8_t*>(sourceMapping), sourceWidth, sourceHeight, mapStride, rotation);
                if (sourceFormat == DRM_FORMAT_XRGB8888) {
                    for (uint32_t y = 0; y < m_height; ++y) {
                        auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch);
                        for (uint32_t x = 0; x < m_width; ++x)
                            row[x] |= 0xff000000;
                    }
                }
                struct dma_buf_sync syncEnd = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
                ioctl(sourceFD, DMA_BUF_IOCTL_SYNC, &syncEnd);

                // The proprietary Mali GBM implementation opens DRM device handles
                // while importing/mapping. Keeping the BO on every transient WPEBuffer
                // leaks /dev/dri/card0 descriptors until IPC fd passing fails. The
                // rotated dumb target owns the copied pixels, so release the import now.
                gbm_bo_unmap(sourceBO, mapData);
                gbm_bo_destroy(sourceBO);
                return true;
            } else {
                userData->sourceMapping = mappedSource;
                userData->sourceMappingSize = mapSize;
                userData->sourceMappingStride = sourceStride;
            }
        }

        struct dma_buf_sync syncStart = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        ioctl(sourceFD, DMA_BUF_IOCTL_SYNC, &syncStart);

        const auto* source = static_cast<const uint8_t*>(userData->sourceMapping) + sourceOffset;
        copyRotatedPixels(source, sourceWidth, sourceHeight, userData->sourceMappingStride, rotation);
        if (sourceFormat == DRM_FORMAT_XRGB8888) {
            for (uint32_t y = 0; y < m_height; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch);
                for (uint32_t x = 0; x < m_width; ++x)
                    row[x] |= 0xff000000;
            }
        }

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

    bool lastCopyWasPartial() const { return m_lastCopyWasPartial; }
    bool lastCopyUsedRGA() const { return m_lastCopyUsedRGA; }
    bool lastCopyFellBackFromRGA() const { return m_lastCopyFellBackFromRGA; }
    gint64 lastRGADurationUS() const { return m_lastRGADurationUS; }

    void finishRGAAccess()
    {
        if (!m_rgaCPUAccessActive || m_rgaPrimeFD < 0)
            return;
        struct dma_buf_sync syncEnd = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW };
        if (ioctl(m_rgaPrimeFD, DMA_BUF_IOCTL_SYNC, &syncEnd) < 0)
            g_warning("WPEViewDRM rga destination sync end failed: %s", strerror(errno));
        m_rgaCPUAccessActive = false;
    }

    bool punchTransparentRect(int x1, int y1, int x2, int y2,
        uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset)
    {
        if (!m_mapping || m_format != DRM_FORMAT_ARGB8888)
            return false;

        auto rect = rotatedDestRect(x1, y1, x2, y2, panelWidth, panelHeight, rotation, topInset);
        rect.x1 = std::clamp<int>(rect.x1, 0, static_cast<int>(m_width));
        rect.y1 = std::clamp<int>(rect.y1, 0, static_cast<int>(m_height));
        rect.x2 = std::clamp<int>(rect.x2, rect.x1, static_cast<int>(m_width));
        rect.y2 = std::clamp<int>(rect.y2, rect.y1, static_cast<int>(m_height));
        if (rect.x2 <= rect.x1 || rect.y2 <= rect.y1)
            return false;

        for (int y = rect.y1; y < rect.y2; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_mapping)
                + static_cast<size_t>(y) * m_pitch);
            for (int x = rect.x1; x < rect.x2; ++x)
                row[x] &= 0x00ffffff;
        }

        return true;
    }

    bool copyBaseTo(Vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, uint32_t& pitch) const
    {
        if (!m_mapping || m_format != DRM_FORMAT_ARGB8888 || !m_width || !m_height)
            return false;
        width = m_width;
        height = m_height;
        pitch = m_width * 4;
        pixels.resize(static_cast<size_t>(pitch) * height);
        auto destination = pixels.mutableSpan();
        for (uint32_t y = 0; y < height; ++y)
            memcpy(destination.data() + static_cast<size_t>(y) * pitch,
                static_cast<const uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch, pitch);
        return true;
    }

    bool restoreBase(const Vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t pitch)
    {
        if (!m_mapping || m_format != DRM_FORMAT_ARGB8888
            || width != m_width || height != m_height || pitch < width * 4
            || pixels.size() < static_cast<size_t>(pitch) * height)
            return false;
        auto source = pixels.span();
        for (uint32_t y = 0; y < height; ++y)
            memcpy(static_cast<uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch,
                source.data() + static_cast<size_t>(y) * pitch, static_cast<size_t>(width) * 4);
        return true;
    }

    bool matchesDimensions(uint32_t width, uint32_t height) const
    {
        return m_mapping && m_format == DRM_FORMAT_ARGB8888
            && m_width == width && m_height == height;
    }

    void drawNativeChrome(uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation)
    {
        if (!m_mapping || m_format != DRM_FORMAT_ARGB8888)
            return;
        drawChromeOverlay(static_cast<uint8_t*>(m_mapping), m_pitch, m_width, m_height,
            panelWidth, panelHeight, rotation);
    }

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
        bool partial = partialCopyEnabled() && !m_pendingFullRepaint && !m_pendingDamage.isEmpty()
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
        }
        m_lastCopyWasPartial = partial;
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
    int m_rgaPrimeFD { -1 };
    bool m_rgaCPUAccessActive { false };
    bool m_lastCopyUsedRGA { false };
    bool m_lastCopyFellBackFromRGA { false };
    gint64 m_lastRGADurationUS { 0 };
    mutable UnixFileDescriptor m_fenceFD;
    Vector<drm_mode_rect> m_pendingDamage;
    bool m_pendingFullRepaint { true };
    uint32_t m_lastTopInset { 0 };
    guint m_lastChromeHash { 0 };
    Vector<drm_mode_rect> m_lastDestDamage;
    bool m_lastCopyWasPartial { false };
};

void WPEBufferDRMUserData::resetSourceMapping()
{
    if (sourceMapping)
        munmap(sourceMapping, sourceMappingSize);
    sourceMapping = nullptr;
    sourceMappingSize = 0;
    sourceMappingStride = 0;
}

WPEBufferDRMUserData::~WPEBufferDRMUserData()
{
    delete scanoutBuffer;
    resetSourceMapping();
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
    gint64 fenceWaitTotalUS { 0 };
    gint64 fenceWaitMaxUS { 0 };
    guint64 fenceWaitCount { 0 };
    guint64 fenceTimeoutCount { 0 };
    guint64 fullCopyCount { 0 };
    guint64 partialCopyCount { 0 };
    gint64 statsWindowStartUS { 0 };
    guint64 statsWindowFrameCount { 0 };
    gint64 copyWindowTotalUS { 0 };
    gint64 copyWindowMaxUS { 0 };
    guint64 copyWindowCount { 0 };
    gint64 commitWindowTotalUS { 0 };
    gint64 commitWindowMaxUS { 0 };
    guint64 commitWindowCount { 0 };
    guint64 queuedWindowCount { 0 };
    guint64 replacedQueuedWindowCount { 0 };
    guint64 droppedWindowCount { 0 };
    gint64 lastFrameCommitUS { 0 };
    gint64 frameThrottleIntervalUS { 0 };
    unsigned chromePollTick { 0 };
    uint32_t chromeMotionSequence { 0 };
    guint64 chromeMotionUpdates { 0 };
    guint64 chromeMotionCoalesced { 0 };
    guint64 chromeOverlayCommits { 0 };
    Vector<uint8_t> chromeBasePixels;
    uint32_t chromeBaseWidth { 0 };
    uint32_t chromeBaseHeight { 0 };
    uint32_t chromeBasePitch { 0 };
    guint64 chromeBaseGeneration { 0 };
    gint64 pageCopyWindowTotalUS { 0 };
    gint64 pageCopyWindowMaxUS { 0 };
    guint64 pageCopyWindowCount { 0 };
    gint64 rgaRotateWindowTotalUS { 0 };
    gint64 rgaRotateWindowMaxUS { 0 };
    guint64 rgaRotateWindowCount { 0 };
    guint64 rgaCPUFallbackWindowCount { 0 };
    gint64 uiOverlayWindowTotalUS { 0 };
    gint64 uiOverlayWindowMaxUS { 0 };
    guint64 uiOverlayWindowCount { 0 };
    guint64 retainedBaseFrameCount { 0 };
    gint64 chromeMotionStatsStartUS { 0 };
    bool chromeMotionPending { false };
    bool lastUpdateDroppedBuffer { false };
    bool forceFullDamageNextFrame { false };
    OutputRotation outputRotation { OutputRotation::Rotate0 };
    struct VideoOverlayState* videoOverlay { nullptr };
};
WEBKIT_DEFINE_FINAL_TYPE(WPEViewDRM, wpe_view_drm, WPE_TYPE_VIEW, WPEView)

static void wpeViewDRMDidPageFlip(WPEViewDRM*);
static gboolean wpeViewDRMRequestUpdate(WPEViewDRM*, GError**);
static void wpeViewDRMCompleteSynchronousCommitIfNeeded(WPEViewDRM*);
static void setDamageRects(Vector<drm_mode_rect>&, const WPERectangle*, guint);

static void wpeViewDRMFinishBufferCommit(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (!priv->pendingBuffer && !priv->pendingScanoutBuffer)
        return;

    bool committedWebFrame = !!priv->pendingBuffer;
    if (committedWebFrame) {
        if (priv->committedBuffer)
            wpe_view_buffer_released(WPE_VIEW(view), priv->committedBuffer.get());
        priv->committedBuffer = WTF::move(priv->pendingBuffer);
        wpe_view_buffer_rendered(WPE_VIEW(view), priv->committedBuffer.get());
    }
    if (priv->pendingScanoutBuffer) {
        priv->committedScanoutBuffer = priv->pendingScanoutBuffer;
        priv->pendingScanoutBuffer = nullptr;
    }

    // Chrome-only redraws now use the same page-flip lifecycle as WebKit frames,
    // but they must not be counted as newly rendered WebKit buffers.
    if (!committedWebFrame)
        return;
    priv->frameCount++;
    priv->statsWindowFrameCount++;

    gint64 nowUS = g_get_monotonic_time();
    gint64 elapsedUS = nowUS - priv->statsWindowStartUS;
    if (elapsedUS < 2 * G_USEC_PER_SEC)
        return;

    auto fps = static_cast<double>(priv->statsWindowFrameCount) * G_USEC_PER_SEC / elapsedUS;
    auto copyAverageMS = priv->copyWindowCount ? priv->copyWindowTotalUS / 1000. / priv->copyWindowCount : 0.;
    auto commitAverageMS = priv->commitWindowCount ? priv->commitWindowTotalUS / 1000. / priv->commitWindowCount : 0.;
    auto rgaAverageMS = priv->rgaRotateWindowCount
        ? priv->rgaRotateWindowTotalUS / 1000. / priv->rgaRotateWindowCount : 0.;
    g_message("WPEViewDRM window fps=%.1f page_copy_ms=%.2f page_copy_max_ms=%.2f rga_ms=%.2f rga_max_ms=%.2f rga_frames=%" G_GUINT64_FORMAT " rga_cpu_fallback=%" G_GUINT64_FORMAT " commit_avg_ms=%.2f commit_max_ms=%.2f base_generation=%" G_GUINT64_FORMAT " retained=%" G_GUINT64_FORMAT " queued=%" G_GUINT64_FORMAT " replaced=%" G_GUINT64_FORMAT " dropped=%" G_GUINT64_FORMAT " frames=%" G_GUINT64_FORMAT " total_frames=%" G_GUINT64_FORMAT " pageflips=%" G_GUINT64_FORMAT,
        fps, copyAverageMS, priv->copyWindowMaxUS / 1000., rgaAverageMS,
        priv->rgaRotateWindowMaxUS / 1000., priv->rgaRotateWindowCount,
        priv->rgaCPUFallbackWindowCount, commitAverageMS, priv->commitWindowMaxUS / 1000.,
        priv->chromeBaseGeneration, priv->retainedBaseFrameCount, priv->queuedWindowCount,
        priv->replacedQueuedWindowCount, priv->droppedWindowCount,
        priv->statsWindowFrameCount, priv->frameCount, priv->pageFlipCount);

    priv->statsWindowStartUS = nowUS;
    priv->statsWindowFrameCount = 0;
    priv->copyWindowTotalUS = 0;
    priv->copyWindowMaxUS = 0;
    priv->copyWindowCount = 0;
    priv->commitWindowTotalUS = 0;
    priv->commitWindowMaxUS = 0;
    priv->commitWindowCount = 0;
    priv->queuedWindowCount = 0;
    priv->replacedQueuedWindowCount = 0;
    priv->droppedWindowCount = 0;
    priv->pageCopyWindowTotalUS = 0;
    priv->pageCopyWindowMaxUS = 0;
    priv->pageCopyWindowCount = 0;
    priv->rgaRotateWindowTotalUS = 0;
    priv->rgaRotateWindowMaxUS = 0;
    priv->rgaRotateWindowCount = 0;
    priv->rgaCPUFallbackWindowCount = 0;
    priv->retainedBaseFrameCount = 0;
}

static void wpeViewDRMQueueBuffer(WPEViewDRM* view, WPEBuffer* buffer, const WPERectangle* damageRects, guint nDamageRects)
{
    auto* priv = view->priv;
    bool replacesQueuedBuffer = priv->queuedBuffer && priv->queuedBuffer.get() != buffer;
    priv->queuedWindowCount++;
    if (replacesQueuedBuffer)
        priv->replacedQueuedWindowCount++;
    if (replacesQueuedBuffer)
        wpe_view_buffer_released(WPE_VIEW(view), priv->queuedBuffer.get());
    priv->queuedBuffer = buffer;
    if (priv->forceFullDamageNextFrame || !nDamageRects || (replacesQueuedBuffer && priv->queuedDamageRects.isEmpty())) {
        priv->queuedDamageRects.clear();
        priv->forceFullDamageNextFrame = false;
    } else if (replacesQueuedBuffer) {
        if (priv->queuedDamageRects.size() + nDamageRects > 16)
            priv->queuedDamageRects.clear();
        else {
            for (unsigned i = 0; i < nDamageRects; ++i)
                priv->queuedDamageRects.append({ damageRects[i].x, damageRects[i].y, damageRects[i].x + damageRects[i].width, damageRects[i].y + damageRects[i].height });
        }
    } else
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
    priv->lastUpdateDroppedBuffer = false;
    if (wpeViewDRMRequestUpdate(view, error)) {
        if (priv->lastUpdateDroppedBuffer) {
            priv->lastUpdateDroppedBuffer = false;
            return TRUE;
        }
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

static void videoOverlayStart(WPEViewDRM*);
static void videoOverlayStop(WPEViewDRM*);
static void videoOverlayRecommit(WPEViewDRM*);

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
    priv->statsWindowStartUS = g_get_monotonic_time();
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
    priv->chromeMotionStatsStartUS = g_get_monotonic_time();
    priv->chromeSource = adoptGRef(g_timeout_source_new(16));
    g_source_set_name(priv->chromeSource.get(), "WPE DRM chrome state poll");
    g_source_set_callback(priv->chromeSource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(+[](gpointer userData) -> gboolean {
        auto* view = WPE_VIEW_DRM(userData);
        auto* priv = view->priv;
        bool stateChanged = !(++priv->chromePollTick % 2) && refreshChromeRenderState();
        bool animationActive = chromeAnimationActive();
        bool motionChanged = false;
        auto* motion = static_cast<const WPEChromeMotionState*>(
            g_object_get_data(G_OBJECT(view), WPE_CHROME_MOTION_DATA_KEY));
        if (motion && motion->version == WPE_CHROME_MOTION_VERSION) {
            if (motion->sequence != priv->chromeMotionSequence) {
                if (priv->chromeMotionSequence && motion->sequence > priv->chromeMotionSequence + 1)
                    priv->chromeMotionCoalesced += motion->sequence - priv->chromeMotionSequence - 1;
                priv->chromeMotionSequence = motion->sequence;
                priv->chromeMotionUpdates++;
                motionChanged = true;
            }
            auto& chrome = chromeStateCache().state;
            bool panelMatches = (motion->panel == WPE_CHROME_MOTION_PANEL_TABS
                    && !strcmp(chrome.panel, "tabs"))
                || (motion->panel == WPE_CHROME_MOTION_PANEL_MENU
                    && !strcmp(chrome.panel, "menu"))
                || (motion->panel == WPE_CHROME_MOTION_PANEL_LIST
                    && strcmp(chrome.panel, "none")
                    && strcmp(chrome.panel, "tabs")
                    && strcmp(chrome.panel, "menu"));
            if (panelMatches && std::abs(chrome.panelScrollOffset - motion->offset) > 0.01) {
                chrome.panelScrollOffset = std::max(0.0, motion->offset);
                motionChanged = true;
            }
            if (chrome.pressedRow != motion->pressed_row
                    || chrome.pressedSegment != motion->pressed_segment
                    || chrome.pressedControl != motion->pressed_control) {
                chrome.pressedRow = motion->pressed_row;
                chrome.pressedSegment = motion->pressed_segment;
                chrome.pressedControl = motion->pressed_control;
                motionChanged = true;
            }
        }
        if (stateChanged || animationActive || motionChanged)
            priv->chromeMotionPending = true;

        bool rotationChanged = false;
        // 旋转文件极少变化：与 chrome 轮询共用一个定时器，每 16 tick（约 256ms）读一次，
        // 取代原先独立的 250ms rotation GSource。
        if (!(priv->chromePollTick % 16)) {
            auto rotation = configuredOutputRotation();
            if (rotation != priv->outputRotation) {
                priv->outputRotation = rotation;
                g_message("WPEViewDRM output_rotation=%u", static_cast<unsigned>(rotation));
                rotationChanged = true;
                priv->chromeMotionPending = true;
            }
        }

        if (stateChanged || animationActive || rotationChanged)
            videoOverlayRecommit(view);
        if (priv->chromeMotionPending && priv->committedBuffer
            && !priv->updateFlags.contains(UpdateFlags::BufferUpdateRequested)) {
            if (wpeViewDRMRequestUpdate(view, nullptr)) {
                priv->updateFlags.add(UpdateFlags::BufferUpdateRequested);
                wpeViewDRMCompleteSynchronousCommitIfNeeded(view);
                priv->chromeMotionPending = false;
                priv->chromeOverlayCommits++;
            }
        }

        gint64 nowUS = g_get_monotonic_time();
        gint64 elapsedUS = nowUS - priv->chromeMotionStatsStartUS;
        if (elapsedUS >= 2 * G_USEC_PER_SEC) {
            double overlayFPS = static_cast<double>(priv->chromeOverlayCommits)
                * G_USEC_PER_SEC / elapsedUS;
            double overlayAverageMS = priv->uiOverlayWindowCount
                ? priv->uiOverlayWindowTotalUS / 1000. / priv->uiOverlayWindowCount : 0.;
            if (priv->chromeMotionUpdates || priv->chromeOverlayCommits)
                g_message("WPE chrome motion: updates=%" G_GUINT64_FORMAT
                    " coalesced=%" G_GUINT64_FORMAT " overlay_fps=%.1f ui_overlay_ms=%.2f"
                    " ui_overlay_max_ms=%.2f base_generation=%" G_GUINT64_FORMAT " pending=%d",
                    priv->chromeMotionUpdates, priv->chromeMotionCoalesced,
                    overlayFPS, overlayAverageMS, priv->uiOverlayWindowMaxUS / 1000.,
                    priv->chromeBaseGeneration, priv->chromeMotionPending);
            priv->chromeMotionUpdates = 0;
            priv->chromeMotionCoalesced = 0;
            priv->chromeOverlayCommits = 0;
            priv->uiOverlayWindowTotalUS = 0;
            priv->uiOverlayWindowMaxUS = 0;
            priv->uiOverlayWindowCount = 0;
            priv->chromeMotionStatsStartUS = nowUS;
        }
        return G_SOURCE_CONTINUE;
    })), object, nullptr);
    g_source_attach(priv->chromeSource.get(), g_main_context_get_thread_default());

    videoOverlayStart(WPE_VIEW_DRM(view));
}

static void wpeViewDRMDispose(GObject* object)
{
    auto* priv = WPE_VIEW_DRM(object)->priv;

    videoOverlayStop(WPE_VIEW_DRM(object));

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

struct VideoOverlayWireMessage;
static void videoOverlayPunchActiveHole(WPEViewDRM*, DRMScanoutBuffer*);

static bool cacheChromeBase(WPEViewDRM* view, DRMScanoutBuffer* buffer)
{
    auto* priv = view->priv;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    if (!buffer || !buffer->copyBaseTo(priv->chromeBasePixels, width, height, pitch))
        return false;
    priv->chromeBaseWidth = width;
    priv->chromeBaseHeight = height;
    priv->chromeBasePitch = pitch;
    priv->chromeBaseGeneration++;
    return true;
}

static void composeNativeChrome(WPEViewDRM* view, DRMScanoutBuffer* buffer)
{
    if (!buffer)
        return;
    auto panel = configuredPanelSize();
    videoOverlayPunchActiveHole(view, buffer);
    buffer->drawNativeChrome(panel.width, panel.height, view->priv->outputRotation);
}

static DRMScanoutBuffer* nextSHMDumbBuffer(WPEViewDRM* view, WPEBuffer* buffer, GError** error)
{
    auto* priv = view->priv;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto* device = wpe_display_drm_get_device(display);
    int fd = gbm_device_get_fd(device);

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

        cacheChromeBase(view, candidate.get());
        composeNativeChrome(view, candidate.get());
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
    auto* device = wpe_display_drm_get_device(display);
    int fd = gbm_device_get_fd(device);

    for (unsigned i = 0; i < priv->rotatedScanoutBuffers.size(); ++i) {
        auto index = (priv->nextRotatedScanoutBufferIndex + i) % priv->rotatedScanoutBuffers.size();
        auto& candidate = priv->rotatedScanoutBuffers[index];
        if (candidate
            && (candidate.get() == priv->committedScanoutBuffer
                || (priv->pendingBuffer && candidate.get() == priv->pendingScanoutBuffer)))
            continue;

        if (!candidate || !candidate->matchesRotatedDMABuf(buffer, rotation)) {
            candidate = DRMScanoutBuffer::createRotatedDMABufDumb(device, fd, buffer, rotation, error);
            if (!candidate)
                return nullptr;
        } else if (!candidate->copyRotatedFromDMABuf(device, buffer, rotation, error))
            return nullptr;

        cacheChromeBase(view, candidate.get());
        composeNativeChrome(view, candidate.get());
        candidate->finishRGAAccess();
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

        cacheChromeBase(view, candidate.get());
        composeNativeChrome(view, candidate.get());
        priv->nextRotatedScanoutBufferIndex = (index + 1) % priv->rotatedScanoutBuffers.size();
        logBufferPathOnce(DRMScanoutBuffer::Kind::SHMRotatedDumb);
        return candidate.get();
    }

    g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: no reusable rotated SHM framebuffer available while page flip is pending");
    return nullptr;
}

static DRMScanoutBuffer* nextChromeOverlayBuffer(WPEViewDRM* view, GError** error)
{
    auto* priv = view->priv;
    if (priv->chromeBasePixels.isEmpty() || !priv->chromeBaseWidth || !priv->chromeBaseHeight) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
            "Failed to redraw chrome: complete page base is unavailable");
        return nullptr;
    }

    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    auto& buffers = priv->outputRotation == OutputRotation::Rotate0
        ? priv->shmScanoutBuffers : priv->rotatedScanoutBuffers;
    unsigned& nextIndex = priv->outputRotation == OutputRotation::Rotate0
        ? priv->nextSHMScanoutBufferIndex : priv->nextRotatedScanoutBufferIndex;

    for (unsigned i = 0; i < buffers.size(); ++i) {
        auto index = (nextIndex + i) % buffers.size();
        auto& candidate = buffers[index];
        if (candidate && (candidate.get() == priv->committedScanoutBuffer
            || candidate.get() == priv->pendingScanoutBuffer))
            continue;
        if (!candidate || !candidate->matchesDimensions(priv->chromeBaseWidth, priv->chromeBaseHeight)) {
            candidate = DRMScanoutBuffer::createChromeDumb(fd, priv->chromeBaseWidth,
                priv->chromeBaseHeight, error);
            if (!candidate)
                return nullptr;
        }
        if (!candidate->restoreBase(priv->chromeBasePixels, priv->chromeBaseWidth,
            priv->chromeBaseHeight, priv->chromeBasePitch)) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
                "Failed to redraw chrome: page base restore failed");
            return nullptr;
        }
        composeNativeChrome(view, candidate.get());
        nextIndex = (index + 1) % buffers.size();
        return candidate.get();
    }

    g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
        "Failed to redraw chrome: no reusable framebuffer available");
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
    static bool forceSynchronousCPUCommit = []() {
        const char* value = g_getenv("WPE_DRM_SYNC_CPU_COMMIT");
        return value && (!strcmp(value, "1") || !g_ascii_strcasecmp(value, "true")
            || !g_ascii_strcasecmp(value, "yes") || !g_ascii_strcasecmp(value, "on"));
    }();
    if (!forceSynchronousCPUCommit)
        return false;
    return buffer->kind() == DRMScanoutBuffer::Kind::SHMDumb
        || buffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb
        || buffer->kind() == DRMScanoutBuffer::Kind::DMABufRotatedDumb
        || buffer->kind() == DRMScanoutBuffer::Kind::ChromeDumb;
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

// ===== 视频 overlay 直出（hole-punch）=====
// WebProcess 端 GStreamerHolePunchQuirkRockchip 的 sink 通过 unix seqpacket
// socket 把解码后的 NV12 dmabuf（mppvideodec+RGA 已按面板方向预旋转）逐帧
// 发来；这里导入成 FB 放到空闲 overlay plane，zpos 压到 primary 之下，页面
// 合成器在视频区域画的透明洞（per-pixel alpha）让视频透出。线格式必须与
// Source/WebCore/platform/gstreamer/GStreamerHolePunchQuirkRockchip.cpp 中的
// 定义保持一字不差。

enum : uint32_t {
    VideoOverlayMessageFrame = 1,
    VideoOverlayMessageRect = 2,
    VideoOverlayMessageHide = 3,
    VideoOverlayMessageRelease = 4,
};

struct VideoOverlayWireMessage {
    uint32_t version;
    uint32_t type;
    uint64_t sequence;
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint32_t planeCount;
    uint32_t strides[3];
    uint32_t offsets[3];
    int32_t rectX;
    int32_t rectY;
    int32_t rectWidth;
    int32_t rectHeight;
    uint64_t modifier;
};

static constexpr uint32_t videoOverlayProtocolVersion = 2;

struct VideoOverlayFB {
    uint32_t fbID { 0 };
    uint32_t handles[3] { 0, 0, 0 };
    uint64_t sequence { 0 };
};

enum class VideoOverlayFit : uint8_t {
    Contain,
    Cover,
    Stretch,
};

static constexpr const char* videoOverlayInputGeometryKey = "wpe-video-overlay-input-geometry";

static VideoOverlayFit configuredVideoOverlayFit()
{
    static auto fit = [] {
        const char* value = getenv("WPE_VIDEO_OVERLAY_FIT");
        if (value && !g_ascii_strcasecmp(value, "cover"))
            return VideoOverlayFit::Cover;
        if (value && !g_ascii_strcasecmp(value, "stretch"))
            return VideoOverlayFit::Stretch;
        return VideoOverlayFit::Contain;
    }();
    return fit;
}

static const char* videoOverlayFitName(VideoOverlayFit fit)
{
    switch (fit) {
    case VideoOverlayFit::Contain:
        return "contain";
    case VideoOverlayFit::Cover:
        return "cover";
    case VideoOverlayFit::Stretch:
        return "stretch";
    }
    return "contain";
}

struct VideoOverlayState {
    WPEViewDRM* view { nullptr };
    int listenFD { -1 };
    int clientFD { -1 };
    GRefPtr<GSource> listenSource;
    GRefPtr<GSource> clientSource;
    const WPE::DRM::Plane* plane { nullptr };
    VideoOverlayFB current;
    VideoOverlayWireMessage lastFrame;
    uint64_t primaryZposRestore { 0 };
    bool haveFrame { false };
    bool planeEnabled { false };
    bool zposUnsupported { false };
    unsigned commitFailLogCount { 0 };
    uint32_t loggedFrameWidth { 0 };
    uint32_t loggedFrameHeight { 0 };
    int32_t loggedCrtcWidth { 0 };
    int32_t loggedCrtcHeight { 0 };
    VideoOverlayFit loggedFit { VideoOverlayFit::Stretch };
    uint64_t receivedFrames { 0 };
    uint64_t committedFrames { 0 };
    uint64_t coalescedFrames { 0 };
    uint64_t failedFrames { 0 };
    uint64_t ackedFrames { 0 };
    uint64_t ackFailedFrames { 0 };
    uint64_t commitTotalUS { 0 };
    uint64_t commitMaxUS { 0 };
    uint64_t nextStatsFrame { 120 };
};

static bool videoOverlayPunchHole(WPEViewDRM* view, DRMScanoutBuffer* scanoutBuffer,
    const VideoOverlayWireMessage& frame)
{
    if (!scanoutBuffer || frame.rectWidth <= 0 || frame.rectHeight <= 0)
        return false;

    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto* screen = WPE_SCREEN_DRM(wpeDisplayDRMGetScreen(display));
    auto* mode = wpeScreenDRMGetMode(screen);
    auto panel = configuredPanelSize();
    uint32_t panelWidth = panel.width ? panel.width : mode->hdisplay;
    uint32_t panelHeight = panel.height ? panel.height : mode->vdisplay;
    uint32_t topInset = chromeReservedTopInset(panelHeight);
    bool punched = scanoutBuffer->punchTransparentRect(
        frame.rectX, frame.rectY,
        frame.rectX + frame.rectWidth, frame.rectY + frame.rectHeight,
        panelWidth, panelHeight, view->priv->outputRotation, topInset);

    static bool loggedSuccess;
    static bool loggedUnsupported;
    if (punched && !loggedSuccess) {
        loggedSuccess = true;
        g_message("WPEViewDRM video hole: source=native-scanout-alpha rect=%dx%d+%d+%d",
            frame.rectWidth, frame.rectHeight, frame.rectX, frame.rectY);
    } else if (!punched && !loggedUnsupported) {
        loggedUnsupported = true;
        g_warning("WPEViewDRM video hole: scanout buffer is not CPU-mappable ARGB; "
            "video overlay may be covered by the primary plane");
    }
    return punched;
}

static void videoOverlayPunchActiveHole(WPEViewDRM* view, DRMScanoutBuffer* scanoutBuffer)
{
    auto* overlay = view->priv->videoOverlay;
    if (!overlay || !overlay->haveFrame)
        return;
    videoOverlayPunchHole(view, scanoutBuffer, overlay->lastFrame);
}

static void videoOverlayPublishInputGeometry(WPEViewDRM* view, const VideoOverlayWireMessage& frame,
    VideoOverlayFit fit, OutputRotation rotation)
{
    int32_t domX = frame.rectX;
    int32_t domY = frame.rectY;
    int32_t domW = frame.rectWidth;
    int32_t domH = frame.rectHeight;
    if (domW <= 0 || domH <= 0) {
        g_object_set_data(G_OBJECT(view), videoOverlayInputGeometryKey, nullptr);
        return;
    }

    int32_t visibleX = domX;
    int32_t visibleY = domY;
    int32_t visibleW = domW;
    int32_t visibleH = domH;
    if (fit == VideoOverlayFit::Contain) {
        uint32_t sourceW = frame.width;
        uint32_t sourceH = frame.height;
        if (rotation == OutputRotation::Rotate90 || rotation == OutputRotation::Rotate270)
            std::swap(sourceW, sourceH);
        if (sourceW && sourceH) {
            if (static_cast<uint64_t>(sourceW) * domH > static_cast<uint64_t>(sourceH) * domW) {
                visibleH = std::max<int32_t>(1, static_cast<int64_t>(domW) * sourceH / sourceW);
                visibleY += (domH - visibleH) / 2;
            } else {
                visibleW = std::max<int32_t>(1, static_cast<int64_t>(domH) * sourceW / sourceH);
                visibleX += (domW - visibleW) / 2;
            }
        }
    }

    auto* serialized = g_strdup_printf("1,%s,%d,%d,%d,%d,%d,%d,%d,%d",
        videoOverlayFitName(fit), domX, domY, domW, domH,
        visibleX, visibleY, visibleW, visibleH);
    const char* previous = static_cast<const char*>(g_object_get_data(G_OBJECT(view), videoOverlayInputGeometryKey));
    if (previous && !strcmp(previous, serialized)) {
        g_free(serialized);
        return;
    }
    g_object_set_data_full(G_OBJECT(view), videoOverlayInputGeometryKey, serialized, g_free);
    g_message("WPEViewDRM video input geometry: %s", serialized);
}

static const char* videoOverlaySocketPath()
{
    static const char* path = []() -> const char* {
        const char* configured = getenv("WPE_VIDEO_OVERLAY_SOCKET");
        if (configured && *configured)
            return g_strdup(configured);
        const char* varDir = getenv("WPE_VAR_DIR");
        if (varDir && *varDir)
            return g_strdup_printf("%s/wpe-video-overlay.sock", varDir);
        return g_strdup_printf("/tmp/wpe-video-overlay-%u.sock", static_cast<unsigned>(getuid()));
    }();
    return path;
}

static void videoOverlayReleaseFB(int fd, VideoOverlayFB& fb)
{
    if (fb.fbID)
        drmModeRmFB(fd, fb.fbID);
    for (unsigned i = 0; i < 3; ++i) {
        if (!fb.handles[i])
            continue;
        // 同一 dmabuf 多平面共享一个 GEM handle，只 close 一次。
        bool alreadyClosed = false;
        for (unsigned j = 0; j < i; ++j) {
            if (fb.handles[j] == fb.handles[i])
                alreadyClosed = true;
        }
        if (!alreadyClosed) {
            struct drm_gem_close close = { fb.handles[i], 0 };
            ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
        }
        fb.handles[i] = 0;
    }
    fb.fbID = 0;
    fb.sequence = 0;
}

static void videoOverlaySendRelease(VideoOverlayState* overlay, uint64_t sequence)
{
    if (!overlay || overlay->clientFD < 0 || !sequence)
        return;
    VideoOverlayWireMessage message = { };
    message.version = videoOverlayProtocolVersion;
    message.type = VideoOverlayMessageRelease;
    message.sequence = sequence;
    ssize_t result;
    do {
        result = send(overlay->clientFD, &message, sizeof(message), MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    if (result == sizeof(message))
        overlay->ackedFrames++;
    else {
        overlay->ackFailedFrames++;
        if (overlay->commitFailLogCount++ < 8)
            g_warning("WPEViewDRM video overlay: release ACK failed sequence=%" G_GUINT64_FORMAT ": %s",
                sequence, strerror(errno));
    }
}

static void videoOverlayRecordCommit(VideoOverlayState* overlay, int64_t durationUS)
{
    if (durationUS < 0)
        return;
    overlay->commitTotalUS += durationUS;
    overlay->commitMaxUS = std::max<uint64_t>(overlay->commitMaxUS, durationUS);
}

static void videoOverlayLogStats(VideoOverlayState* overlay)
{
    if (overlay->receivedFrames < overlay->nextStatsFrame)
        return;
    double averageMS = overlay->committedFrames
        ? static_cast<double>(overlay->commitTotalUS) / overlay->committedFrames / 1000.0 : 0;
    g_message("WPEViewDRM video stats: received=%" G_GUINT64_FORMAT
        " committed=%" G_GUINT64_FORMAT " coalesced=%" G_GUINT64_FORMAT
        " failed=%" G_GUINT64_FORMAT " acked=%" G_GUINT64_FORMAT
        " ack_failed=%" G_GUINT64_FORMAT " inflight=%u commit_avg_ms=%.2f commit_max_ms=%.2f current_fb=%u sequence=%" G_GUINT64_FORMAT,
        overlay->receivedFrames, overlay->committedFrames, overlay->coalescedFrames,
        overlay->failedFrames, overlay->ackedFrames, overlay->ackFailedFrames,
        overlay->current.sequence ? 1 : 0, averageMS, overlay->commitMaxUS / 1000.0,
        overlay->current.fbID, overlay->current.sequence);
    while (overlay->nextStatsFrame <= overlay->receivedFrames)
        overlay->nextStatsFrame += 120;
}

static bool videoOverlayCommit(WPEViewDRM* view, VideoOverlayState* overlay, uint32_t fbID,
    const VideoOverlayWireMessage& frame, int64_t* durationUS = nullptr)
{
    int64_t startUS = g_get_monotonic_time();
    auto finish = [&](bool result) {
        if (durationUS)
            *durationUS = g_get_monotonic_time() - startUS;
        return result;
    };
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto* screen = WPE_SCREEN_DRM(wpeDisplayDRMGetScreen(display));
    auto& crtc = wpeScreenDRMGetCrtc(screen);
    auto* mode = wpeScreenDRMGetMode(screen);
    auto fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    auto* priv = view->priv;

    auto rotation = priv->outputRotation;
    auto panel = configuredPanelSize();
    uint32_t panelW = panel.width ? panel.width : mode->hdisplay;
    uint32_t panelH = panel.height ? panel.height : mode->vdisplay;
    uint32_t topInset = chromeReservedTopInset(panelH);

    // 视图坐标矩形 → 旋转后 framebuffer 坐标（与 CPU 旋转拷贝同一套数学）。
    auto rect = rotatedDestRect(frame.rectX, frame.rectY, frame.rectX + frame.rectWidth, frame.rectY + frame.rectHeight,
        panelW, panelH, rotation, topInset);

    // framebuffer 在 mode 里的放置偏移，与 destinationRectForBuffer 的
    // panel-native 分支一致。视频最终要裁剪进图形内容条带区（panel-native
    // 下面板只露出 fb 那一条，例如 480x960 mode 里 x∈[107,373)，出界部分
    // 在挡板后且没有图形层打洞配合，不能让视频漏出去）。
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    int32_t contentX1 = 0;
    int32_t contentY1 = 0;
    int32_t contentX2 = mode->hdisplay;
    int32_t contentY2 = mode->vdisplay;
    if (shouldUsePanelNativeFit()) {
        uint32_t fbW = std::min<uint32_t>(rotatedWidth(panelW, panelH, rotation), mode->hdisplay);
        uint32_t fbH = std::min<uint32_t>(rotatedHeight(panelW, panelH, rotation), mode->vdisplay);
        uint32_t maxX = mode->hdisplay > fbW ? mode->hdisplay - fbW : 0;
        if (auto configuredX = configuredRotatedXOffset(maxX))
            offsetX = configuredX.value();
        else
            offsetX = maxX / 2;
        offsetY = mode->vdisplay > fbH ? (mode->vdisplay - fbH) / 2 : 0;
        contentX1 = offsetX;
        contentY1 = offsetY;
        contentX2 = offsetX + fbW;
        contentY2 = offsetY + fbH;
    }

    int32_t dstX1 = rect.x1 + offsetX;
    int32_t dstY1 = rect.y1 + offsetY;
    int32_t dstX2 = rect.x2 + offsetX;
    int32_t dstY2 = rect.y2 + offsetY;
    if (dstX2 <= dstX1 || dstY2 <= dstY1)
        return finish(false);

    // 解码器已按输出方向预旋转。先按源帧纵横比适配完整目标矩形，再裁剪到
    // panel-native 内容条带。contain 保留完整画面，cover 保留目标尺寸并居中
    // 裁剪源帧，stretch 仅作为旧行为回滚开关。
    auto fit = configuredVideoOverlayFit();
    videoOverlayPublishInputGeometry(view, frame, fit, rotation);
    int32_t fullW = dstX2 - dstX1;
    int32_t fullH = dstY2 - dstY1;
    uint64_t srcBaseX = 0;
    uint64_t srcBaseY = 0;
    uint64_t srcBaseW = static_cast<uint64_t>(frame.width) << 16;
    uint64_t srcBaseH = static_cast<uint64_t>(frame.height) << 16;
    if (fit == VideoOverlayFit::Contain) {
        if (static_cast<uint64_t>(frame.width) * fullH > static_cast<uint64_t>(frame.height) * fullW) {
            int32_t fittedH = std::max<int32_t>(1, static_cast<int64_t>(fullW) * frame.height / frame.width);
            dstY1 += (fullH - fittedH) / 2;
            dstY2 = dstY1 + fittedH;
        } else {
            int32_t fittedW = std::max<int32_t>(1, static_cast<int64_t>(fullH) * frame.width / frame.height);
            dstX1 += (fullW - fittedW) / 2;
            dstX2 = dstX1 + fittedW;
        }
        fullW = dstX2 - dstX1;
        fullH = dstY2 - dstY1;
    } else if (fit == VideoOverlayFit::Cover) {
        if (static_cast<uint64_t>(frame.width) * fullH > static_cast<uint64_t>(frame.height) * fullW) {
            uint64_t croppedWidth = static_cast<uint64_t>(frame.height) * fullW / fullH;
            srcBaseX = ((static_cast<uint64_t>(frame.width) - croppedWidth) << 15);
            srcBaseW = croppedWidth << 16;
        } else {
            uint64_t croppedHeight = static_cast<uint64_t>(frame.width) * fullH / fullW;
            srcBaseY = ((static_cast<uint64_t>(frame.height) - croppedHeight) << 15);
            srcBaseH = croppedHeight << 16;
        }
    }

    // 裁剪到内容条带区，源矩形按比例跟进（16.16 定点）。
    int32_t clipX1 = std::max<int32_t>(dstX1, contentX1);
    int32_t clipY1 = std::max<int32_t>(dstY1, contentY1);
    int32_t clipX2 = std::min<int32_t>(dstX2, contentX2);
    int32_t clipY2 = std::min<int32_t>(dstY2, contentY2);
    if (clipX2 <= clipX1 || clipY2 <= clipY1) {
        // 完全滚出屏幕：藏 plane 但保留帧，等矩形回来再显示。
        if (overlay->planeEnabled) {
            WPE::DRM::UniquePtr<drmModeAtomicReq> request(drmModeAtomicAlloc());
            if (!addPlaneProperties(request.get(), *overlay->plane, emptyPlaneProperties(*overlay->plane))
                || drmModeAtomicCommit(fd, request.get(), 0, nullptr))
                return finish(false);
            overlay->planeEnabled = false;
        }
        return finish(true);
    }

    uint64_t srcX = srcBaseX + srcBaseW * (clipX1 - dstX1) / fullW;
    uint64_t srcY = srcBaseY + srcBaseH * (clipY1 - dstY1) / fullH;
    uint64_t srcW = srcBaseW * (clipX2 - clipX1) / fullW;
    uint64_t srcH = srcBaseH * (clipY2 - clipY1) / fullH;

    if (overlay->loggedFrameWidth != frame.width || overlay->loggedFrameHeight != frame.height
        || overlay->loggedCrtcWidth != clipX2 - clipX1 || overlay->loggedCrtcHeight != clipY2 - clipY1
        || overlay->loggedFit != fit) {
        g_message("WPEViewDRM video overlay geometry: fit=%s frame=%ux%u rect=%dx%d+%d+%d crtc=%dx%d+%d+%d src=%" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT "+%" G_GUINT64_FORMAT "+%" G_GUINT64_FORMAT " rotation=%u",
            videoOverlayFitName(fit), frame.width, frame.height, frame.rectWidth, frame.rectHeight, frame.rectX, frame.rectY,
            clipX2 - clipX1, clipY2 - clipY1, clipX1, clipY1,
            srcW >> 16, srcH >> 16, srcX >> 16, srcY >> 16, static_cast<unsigned>(rotation));
        overlay->loggedFrameWidth = frame.width;
        overlay->loggedFrameHeight = frame.height;
        overlay->loggedCrtcWidth = clipX2 - clipX1;
        overlay->loggedCrtcHeight = clipY2 - clipY1;
        overlay->loggedFit = fit;
    }

    auto properties = overlay->plane->properties();
    properties.crtcID.second = crtc.id();
    properties.crtcX.second = clipX1;
    properties.crtcY.second = clipY1;
    properties.crtcW.second = clipX2 - clipX1;
    properties.crtcH.second = clipY2 - clipY1;
    properties.fbID.second = fbID;
    properties.srcX.second = srcX;
    properties.srcY.second = srcY;
    properties.srcW.second = srcW;
    properties.srcH.second = srcH;
    properties.rotation.second = drmPlaneRotate0Value();

    const auto& primaryPlane = wpeDisplayDRMGetPrimaryPlane(display);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool withZpos = !overlay->zposUnsupported && !attempt
            && properties.zpos.first && primaryPlane.properties().zpos.first;
        WPE::DRM::UniquePtr<drmModeAtomicReq> request(drmModeAtomicAlloc());
        if (!addPlaneProperties(request.get(), *overlay->plane, WPE::DRM::Plane::Properties(properties)))
            return finish(false);
        if (withZpos) {
            // 视频压到 primary 之下，primary 靠透明洞的 per-pixel alpha 透出视频。
            drmModeAtomicAddProperty(request.get(), overlay->plane->id(), properties.zpos.first, 0);
            drmModeAtomicAddProperty(request.get(), primaryPlane.id(), primaryPlane.properties().zpos.first, 1);
        }
        int commitResult = -1;
        for (unsigned busyRetry = 0; busyRetry <= 20; ++busyRetry) {
            commitResult = drmModeAtomicCommit(fd, request.get(), 0, nullptr);
            if (!commitResult || errno != EBUSY)
                break;
            g_usleep(1000);
        }
        if (!commitResult) {
            overlay->planeEnabled = true;
            return finish(true);
        }
        if (errno == EBUSY) {
            if (overlay->commitFailLogCount++ < 8)
                g_warning("WPEViewDRM video overlay: atomic commit remained busy after 20ms");
            return finish(false);
        }
        if (withZpos) {
            g_warning("WPEViewDRM video overlay: commit with zpos failed (%s), falling back to no-zpos (video above page)", strerror(errno));
            overlay->zposUnsupported = true;
            continue;
        }
        if (overlay->commitFailLogCount++ < 8)
            g_warning("WPEViewDRM video overlay: atomic commit failed: %s", strerror(errno));
        return finish(false);
    }
    return finish(false);
}

static void videoOverlayDisable(WPEViewDRM* view)
{
    auto* overlay = view->priv->videoOverlay;
    if (!overlay || !overlay->plane)
        return;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    if (overlay->planeEnabled) {
        WPE::DRM::UniquePtr<drmModeAtomicReq> request(drmModeAtomicAlloc());
        bool success = addPlaneProperties(request.get(), *overlay->plane, emptyPlaneProperties(*overlay->plane));
        const auto& primaryPlane = wpeDisplayDRMGetPrimaryPlane(display);
        if (!overlay->zposUnsupported && primaryPlane.properties().zpos.first)
            drmModeAtomicAddProperty(request.get(), primaryPlane.id(), primaryPlane.properties().zpos.first, overlay->primaryZposRestore);
        if (success && drmModeAtomicCommit(fd, request.get(), 0, nullptr))
            g_warning("WPEViewDRM video overlay: disable commit failed: %s", strerror(errno));
        overlay->planeEnabled = false;
    }
    uint64_t releasedSequence = overlay->current.sequence;
    videoOverlayReleaseFB(fd, overlay->current);
    videoOverlaySendRelease(overlay, releasedSequence);
    overlay->haveFrame = false;
    view->priv->chromeMotionPending = true;
    g_object_set_data(G_OBJECT(view), videoOverlayInputGeometryKey, nullptr);
}

static bool videoOverlayHandleFrame(WPEViewDRM* view, const VideoOverlayWireMessage& frame, int* fds, unsigned fdCount)
{
    auto* overlay = view->priv->videoOverlay;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto fd = gbm_device_get_fd(wpe_display_drm_get_device(display));

    if (!fdCount || frame.planeCount < 1 || frame.planeCount > 3 || !frame.width || !frame.height)
        return false;
    if (frame.fourcc != DRM_FORMAT_NV12 || !overlay->plane->supportsFormat(DRM_FORMAT_NV12, DRM_FORMAT_MOD_INVALID)) {
        static bool warned;
        if (!warned) {
            g_warning("WPEViewDRM video overlay: unsupported fourcc %.4s", reinterpret_cast<const char*>(&frame.fourcc));
            warned = true;
        }
        return false;
    }

    VideoOverlayFB fb;
    fb.sequence = frame.sequence;
    uint32_t pitches[4] = { 0, };
    uint32_t offsets[4] = { 0, };
    uint32_t handles[4] = { 0, };
    for (unsigned i = 0; i < frame.planeCount; ++i) {
        int dmabufFD = i < fdCount ? fds[i] : fds[fdCount - 1];
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(fd, dmabufFD, &handle)) {
            if (overlay->commitFailLogCount++ < 8)
                g_warning("WPEViewDRM video overlay: drmPrimeFDToHandle failed: %s", strerror(errno));
            videoOverlayReleaseFB(fd, fb);
            return false;
        }
        fb.handles[i] = handle;
        handles[i] = handle;
        pitches[i] = frame.strides[i];
        offsets[i] = frame.offsets[i];
    }

    if (drmModeAddFB2(fd, frame.width, frame.height, frame.fourcc, handles, pitches, offsets, &fb.fbID, 0)) {
        if (overlay->commitFailLogCount++ < 8)
            g_warning("WPEViewDRM video overlay: drmModeAddFB2 %ux%u failed: %s", frame.width, frame.height, strerror(errno));
        videoOverlayReleaseFB(fd, fb);
        return false;
    }

    // Recompose primary from the clean CPU base when hole geometry changes.
    // Never mutate a framebuffer that may currently be scanned out.
    bool holeGeometryChanged = !overlay->haveFrame
        || frame.rectX != overlay->lastFrame.rectX
        || frame.rectY != overlay->lastFrame.rectY
        || frame.rectWidth != overlay->lastFrame.rectWidth
        || frame.rectHeight != overlay->lastFrame.rectHeight;
    if (holeGeometryChanged)
        view->priv->chromeMotionPending = true;

    int64_t commitUS = 0;
    if (!videoOverlayCommit(view, overlay, fb.fbID, frame, &commitUS)) {
        videoOverlayReleaseFB(fd, fb);
        return false;
    }

    VideoOverlayFB previous = overlay->current;
    overlay->current = fb;
    overlay->lastFrame = frame;
    overlay->haveFrame = true;
    uint64_t releasedSequence = previous.sequence;
    videoOverlayReleaseFB(fd, previous);
    videoOverlaySendRelease(overlay, releasedSequence);
    videoOverlayRecordCommit(overlay, commitUS);
    return true;
}

// 布局变化（工具栏动画/旋转/矩形更新）后按最新状态重新提交当前帧。
static void videoOverlayRecommit(WPEViewDRM* view)
{
    auto* overlay = view->priv->videoOverlay;
    if (!overlay || !overlay->haveFrame || !overlay->current.fbID)
        return;
    videoOverlayCommit(view, overlay, overlay->current.fbID, overlay->lastFrame);
}

static void videoOverlayDropClient(WPEViewDRM* view)
{
    auto* overlay = view->priv->videoOverlay;
    if (overlay->clientSource) {
        g_source_destroy(overlay->clientSource.get());
        overlay->clientSource = nullptr;
    }
    videoOverlayDisable(view);
    if (overlay->clientFD >= 0) {
        close(overlay->clientFD);
        overlay->clientFD = -1;
    }
}

static gboolean videoOverlayClientEvent(int socketFD, GIOCondition condition, gpointer userData)
{
    auto* view = WPE_VIEW_DRM(userData);
    auto* overlay = view->priv->videoOverlay;

    if (condition & (G_IO_ERR | G_IO_HUP)) {
        videoOverlayDropClient(view);
        return G_SOURCE_REMOVE;
    }

    VideoOverlayWireMessage pendingFrame { };
    int pendingFDs[3] = { -1, -1, -1 };
    unsigned pendingFDCount = 0;
    bool havePendingFrame = false;

    auto closePacketFDs = [](int* fds, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            if (fds[i] >= 0)
                close(fds[i]);
            fds[i] = -1;
        }
    };
    auto flushPendingFrame = [&]() {
        if (!havePendingFrame)
            return;
        if (videoOverlayHandleFrame(view, pendingFrame, pendingFDs, pendingFDCount))
            overlay->committedFrames++;
        else {
            overlay->failedFrames++;
            videoOverlaySendRelease(overlay, pendingFrame.sequence);
        }
        closePacketFDs(pendingFDs, pendingFDCount);
        pendingFDCount = 0;
        havePendingFrame = false;
    };

    while (true) {
        VideoOverlayWireMessage message { };
        int fds[3] = { -1, -1, -1 };
        unsigned fdCount = 0;
        struct iovec vec = { &message, sizeof(message) };
        char control[CMSG_SPACE(sizeof(fds))];
        struct msghdr msg = { };
        msg.msg_iov = &vec;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        ssize_t received = recvmsg(socketFD, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (received <= 0) {
            if (received < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            flushPendingFrame();
            videoOverlayDropClient(view);
            return G_SOURCE_REMOVE;
        }

        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                continue;
            unsigned count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (unsigned i = 0; i < count && fdCount < 3; ++i)
                fds[fdCount++] = reinterpret_cast<int*>(CMSG_DATA(cmsg))[i];
        }

        if (received != sizeof(message) || message.version != videoOverlayProtocolVersion) {
            closePacketFDs(fds, fdCount);
            continue;
        }

        if (message.type == VideoOverlayMessageFrame) {
            overlay->receivedFrames++;
            if (havePendingFrame) {
                closePacketFDs(pendingFDs, pendingFDCount);
                overlay->coalescedFrames++;
                videoOverlaySendRelease(overlay, pendingFrame.sequence);
            }
            pendingFrame = message;
            memcpy(pendingFDs, fds, sizeof(fds));
            pendingFDCount = fdCount;
            havePendingFrame = true;
            continue;
        }

        flushPendingFrame();
        closePacketFDs(fds, fdCount);
        switch (message.type) {
        case VideoOverlayMessageRect:
            if (overlay->haveFrame) {
                overlay->lastFrame.rectX = message.rectX;
                overlay->lastFrame.rectY = message.rectY;
                overlay->lastFrame.rectWidth = message.rectWidth;
                overlay->lastFrame.rectHeight = message.rectHeight;
                view->priv->chromeMotionPending = true;
                videoOverlayRecommit(view);
            }
            break;
        case VideoOverlayMessageHide:
            videoOverlayDisable(view);
            break;
        default:
            break;
        }
    }

    flushPendingFrame();
    videoOverlayLogStats(overlay);
    return G_SOURCE_CONTINUE;
}

static gboolean videoOverlayAccept(int listenFD, GIOCondition condition, gpointer userData)
{
    auto* view = WPE_VIEW_DRM(userData);
    auto* overlay = view->priv->videoOverlay;

    if (condition & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_REMOVE;

    int clientFD = accept4(listenFD, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (clientFD < 0)
        return G_SOURCE_CONTINUE;

    if (overlay->clientFD >= 0)
        videoOverlayDropClient(view);

    overlay->clientFD = clientFD;
    overlay->clientSource = adoptGRef(g_unix_fd_source_new(clientFD, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP)));
    g_source_set_name(overlay->clientSource.get(), "WPE DRM video overlay client");
    g_source_set_callback(overlay->clientSource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(videoOverlayClientEvent)), view, nullptr);
    g_source_attach(overlay->clientSource.get(), g_main_context_get_thread_default());
    g_message("WPEViewDRM video overlay: client connected");
    return G_SOURCE_CONTINUE;
}

static void videoOverlayStart(WPEViewDRM* view)
{
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    const auto* plane = wpeDisplayDRMGetVideoOverlayPlane(display);
    if (!plane)
        return;

    const char* path = videoOverlaySocketPath();
    struct sockaddr_un address = { };
    if (strlen(path) >= sizeof(address.sun_path)) {
        g_warning("WPEViewDRM video overlay: socket path too long: %s", path);
        return;
    }

    int listenFD = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFD < 0)
        return;

    unlink(path);
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    if (bind(listenFD, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) || listen(listenFD, 1)) {
        g_warning("WPEViewDRM video overlay: failed to listen on %s: %s", path, strerror(errno));
        close(listenFD);
        return;
    }

    auto* overlay = new VideoOverlayState();
    overlay->view = view;
    overlay->listenFD = listenFD;
    overlay->plane = plane;
    overlay->primaryZposRestore = wpeDisplayDRMGetPrimaryPlane(display).properties().zpos.second;
    view->priv->videoOverlay = overlay;

    overlay->listenSource = adoptGRef(g_unix_fd_source_new(listenFD, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP)));
    g_source_set_name(overlay->listenSource.get(), "WPE DRM video overlay listener");
    g_source_set_callback(overlay->listenSource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(videoOverlayAccept)), view, nullptr);
    g_source_attach(overlay->listenSource.get(), g_main_context_get_thread_default());
    g_message("WPEViewDRM video overlay: listening on %s (plane %u)", path, plane->id());
}

static void videoOverlayStop(WPEViewDRM* view)
{
    auto* overlay = view->priv->videoOverlay;
    if (!overlay)
        return;
    videoOverlayDropClient(view);
    if (overlay->listenSource) {
        g_source_destroy(overlay->listenSource.get());
        overlay->listenSource = nullptr;
    }
    if (overlay->listenFD >= 0) {
        close(overlay->listenFD);
        overlay->listenFD = -1;
    }
    unlink(videoOverlaySocketPath());
    delete overlay;
    view->priv->videoOverlay = nullptr;
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

static bool bufferNeedsCPURead(WPEBuffer* buffer, OutputRotation rotation)
{
    return WPE_IS_BUFFER_SHM(buffer) || (WPE_IS_BUFFER_DMA_BUF(buffer) && rotation != OutputRotation::Rotate0);
}

static bool waitForRenderingFence(WPEViewDRM* view, const UnixFileDescriptor& fence)
{
    if (!fence)
        return true;

    auto* priv = view->priv;
    struct pollfd descriptor = { fence.value(), POLLIN, 0 };
    gint64 startUS = g_get_monotonic_time();
    int result;
    do {
        result = poll(&descriptor, 1, renderingFenceTimeoutMS());
    } while (result < 0 && errno == EINTR);
    gint64 elapsedUS = g_get_monotonic_time() - startUS;
    priv->fenceWaitCount++;
    priv->fenceWaitTotalUS += elapsedUS;
    priv->fenceWaitMaxUS = std::max(priv->fenceWaitMaxUS, elapsedUS);
    if (result > 0)
        return true;

    priv->fenceTimeoutCount++;
    g_warning("WPEViewDRM rendering fence %s after %.2fms fd=%d timeout_ms=%d; dropping incomplete frame",
        result == 0 ? "timed out" : "wait failed", elapsedUS / 1000., fence.value(), renderingFenceTimeoutMS());
    return false;
}

static void dropPendingFrame(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (priv->pendingBuffer)
        wpe_view_buffer_released(WPE_VIEW(view), priv->pendingBuffer.get());
    priv->pendingBuffer = nullptr;
    priv->pendingScanoutBuffer = nullptr;
    priv->damageRects.clear();
    priv->lastUpdateDroppedBuffer = true;
    priv->forceFullDamageNextFrame = true;
    priv->droppedWindowCount++;
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
                || priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb
                || priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::SHMDumb
                || priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::ChromeDumb)));
    bool uiOnlyRedraw = !priv->pendingBuffer && needsScanoutRebuild
        && !priv->chromeBasePixels.isEmpty();
    if (priv->pendingBuffer || needsScanoutRebuild) {
        UnixFileDescriptor renderingFence;
        bool pendingNeedsCPURead = priv->pendingBuffer && bufferNeedsCPURead(buffer, rotation);
        if (priv->pendingBuffer)
            renderingFence = UnixFileDescriptor { wpe_buffer_take_rendering_fence(buffer), UnixFileDescriptor::Adopt };
        if (pendingNeedsCPURead && !waitForRenderingFence(view, renderingFence)) {
            priv->retainedBaseFrameCount++;
            dropPendingFrame(view);
            return TRUE;
        }

        gint64 copyStartUS = g_get_monotonic_time();
        if (rotation != OutputRotation::Rotate0 && priv->pendingBuffer) {
            for (auto& candidate : priv->rotatedScanoutBuffers) {
                if (candidate)
                    candidate->accumulateSourceDamage(priv->damageRects);
            }
        }
        drmBuffer = uiOnlyRedraw
            ? nextChromeOverlayBuffer(view, error)
            : drmScanoutBufferForRender(view, buffer, rotation, error);
        if (!drmBuffer)
            return FALSE;

        if (uiOnlyRedraw) {
            auto durationUS = g_get_monotonic_time() - copyStartUS;
            priv->uiOverlayWindowTotalUS += durationUS;
            priv->uiOverlayWindowMaxUS = std::max(priv->uiOverlayWindowMaxUS, durationUS);
            priv->uiOverlayWindowCount++;
        } else if (drmBuffer->kind() == DRMScanoutBuffer::Kind::SHMDumb
            || drmBuffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb
            || drmBuffer->kind() == DRMScanoutBuffer::Kind::DMABufRotatedDumb) {
            auto copyDurationUS = g_get_monotonic_time() - copyStartUS;
            priv->copyTotalUS += copyDurationUS;
            priv->copyWindowTotalUS += copyDurationUS;
            priv->copyWindowMaxUS = std::max(priv->copyWindowMaxUS, copyDurationUS);
            priv->copyWindowCount++;
            priv->pageCopyWindowTotalUS += copyDurationUS;
            priv->pageCopyWindowMaxUS = std::max(priv->pageCopyWindowMaxUS, copyDurationUS);
            priv->pageCopyWindowCount++;
            if (drmBuffer->lastCopyUsedRGA()) {
                priv->rgaRotateWindowTotalUS += drmBuffer->lastRGADurationUS();
                priv->rgaRotateWindowMaxUS = std::max(priv->rgaRotateWindowMaxUS,
                    drmBuffer->lastRGADurationUS());
                priv->rgaRotateWindowCount++;
            } else if (drmBuffer->lastCopyFellBackFromRGA())
                priv->rgaCPUFallbackWindowCount++;
            if (drmBuffer->lastCopyWasPartial())
                priv->partialCopyCount++;
            else
                priv->fullCopyCount++;
        }
        if (!uiOnlyRedraw && rotation != OutputRotation::Rotate0)
            priv->damageRects = drmBuffer->takeDestDamage();
        else if (uiOnlyRedraw)
            priv->damageRects.clear();

        if (priv->pendingBuffer) {
            if (!pendingNeedsCPURead)
                drmBuffer->setFenceFD(WTF::move(renderingFence));
            priv->pendingScanoutBuffer = drmBuffer;
        } else
            // UI-only redraws must not make the old scanout reusable until the
            // page-flip event confirms that hardware stopped scanning it out.
            priv->pendingScanoutBuffer = drmBuffer;
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
        auto commitDurationUS = g_get_monotonic_time() - commitStartUS;
        priv->commitTotalUS += commitDurationUS;
        priv->commitWindowTotalUS += commitDurationUS;
        priv->commitWindowMaxUS = std::max(priv->commitWindowMaxUS, commitDurationUS);
        priv->commitWindowCount++;
        if (result && drmBuffer)
            priv->lastFrameCommitUS = commitStartUS;
        priv->lastCommitWasSynchronous = result && bufferUsesSynchronousCommit(drmBuffer);
        if (!result && !priv->pendingBuffer)
            priv->pendingScanoutBuffer = nullptr;
        return result;
    }

    if (!drmBuffer) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: no DRM buffer to commit");
        return FALSE;
    }

    auto result = wpeViewDRMCommitLegacy(WPE_VIEW_DRM(view), *drmBuffer, error);
    auto commitDurationUS = g_get_monotonic_time() - commitStartUS;
    priv->commitTotalUS += commitDurationUS;
    priv->commitWindowTotalUS += commitDurationUS;
    priv->commitWindowMaxUS = std::max(priv->commitWindowMaxUS, commitDurationUS);
    priv->commitWindowCount++;
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
    if (priv->forceFullDamageNextFrame) {
        priv->damageRects.clear();
        priv->forceFullDamageNextFrame = false;
    } else
        setDamageRects(priv->damageRects, damageRects, nDamageRects);
    priv->lastUpdateDroppedBuffer = false;

    if (priv->cursorUpdateTimer)
        priv->cursorUpdateTimer->stop();
    if (wpeViewDRMRequestUpdate(WPE_VIEW_DRM(view), error)) {
        if (priv->lastUpdateDroppedBuffer) {
            priv->lastUpdateDroppedBuffer = false;
            return TRUE;
        }
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
