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
#include "DRMGeometry.h"
#include "ChromeRenderer.h"

bool parseSize(const char* value, uint32_t& width, uint32_t& height)
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
PanelSize configuredPanelSize()
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

int renderingFenceTimeoutMS()
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

PanelSize scanoutPanelForSource(uint32_t sourceWidth, uint32_t sourceHeight)
{
    PanelSize sourcePanel { sourceWidth, sourceHeight };
    auto panel = configuredPanelSize();
    if (panel.width >= sourceWidth && panel.height >= sourceHeight)
        return panel;
    return sourcePanel;
}

const char* drmFitMode()
{
    static const char* mode = []() {
        const char* fit = getenv("WPE_DRM_FIT");
        return fit && *fit ? fit : "panel-native";
    }();
    return mode;
}

uint32_t rotatedWidth(uint32_t width, uint32_t height, OutputRotation rotation)
{
    return rotation == OutputRotation::Rotate90 || rotation == OutputRotation::Rotate270 ? height : width;
}

uint32_t rotatedHeight(uint32_t width, uint32_t height, OutputRotation rotation)
{
    return rotation == OutputRotation::Rotate90 || rotation == OutputRotation::Rotate270 ? width : height;
}

bool parseRotation(const char* value, OutputRotation& rotation)
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

OutputRotation configuredOutputRotation()
{
    OutputRotation rotation = OutputRotation::Rotate0;
    if (const char* value = getenv("WPE_DRM_ROTATION"))
        parseRotation(value, rotation);
    return rotation;
}

uint32_t configuredMaxFPS()
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

uint64_t drmPlaneRotate0Value()
{
#ifdef DRM_MODE_ROTATE_0
    return DRM_MODE_ROTATE_0;
#else
    return 1;
#endif
}


void fillARGB8888(uint8_t* destination, uint32_t destinationPitch, uint32_t width, uint32_t height, uint32_t color)
{
    for (uint32_t y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(destination + static_cast<size_t>(y) * destinationPitch);
        std::fill(row, row + width, color);
    }
}

bool syncDMABuf(int fd, uint64_t flags, GError** error)
{
    struct dma_buf_sync sync = { flags };
    int result;
    do {
        result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result < 0 && errno == EINTR);
    if (!result)
        return true;
    g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
        "DMA-BUF cache sync failed: fd=%d flags=%llu error=%s",
        fd, static_cast<unsigned long long>(flags), strerror(errno));
    return false;
}

// 把一行 panel 坐标系的像素写入旋转后的目标缓冲。调用方保证
// panelY < panelHeight 且 x0+width <= panelWidth，因此无需逐像素做边界检查。
void writeRotatedPanelRow(const uint32_t* sourceRow, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t panelY, uint32_t x0, uint32_t width)
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
drm_mode_rect rotatedDestRect(int x1, int y1, int x2, int y2, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset)
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
void fillRotatedPanelRect(uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color)
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
void rotateRegionARGB8888(const uint8_t* source, uint32_t sourceStride, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height)
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

void copyRotatedARGB8888(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t sourceStride, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation)
{
    auto destinationWidth = rotatedWidth(panelWidth, panelHeight, rotation);
    auto destinationHeight = rotatedHeight(panelWidth, panelHeight, rotation);
    auto topInset = chromeFrameTopInset(panelHeight, sourceHeight);
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
