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
#include "DRMScanoutBuffer.h"
#include "ChromeRenderer.h"

std::unique_ptr<DRMScanoutBuffer> DRMScanoutBuffer::createDMABuf(std::unique_ptr<WPE::DRM::Buffer>&& buffer)
{
    auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::DMABufGBM));
    scanoutBuffer->m_drmBuffer = WTF::move(buffer);
    return scanoutBuffer;
}

std::unique_ptr<DRMScanoutBuffer> DRMScanoutBuffer::createDMABufDirect(int fd, WPEBuffer* buffer, GError** error)
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

std::unique_ptr<DRMScanoutBuffer> DRMScanoutBuffer::createRotatedDMABufDumb(struct gbm_device* device, int fd, WPEBuffer* buffer, OutputRotation rotation, GError** error)
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

std::unique_ptr<DRMScanoutBuffer> DRMScanoutBuffer::createSHMDumb(int fd, WPEBuffer* buffer, GError** error)
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

std::unique_ptr<DRMScanoutBuffer> DRMScanoutBuffer::createRotatedSHMDumb(int fd, WPEBuffer* buffer, OutputRotation rotation, GError** error)
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

std::unique_ptr<DRMScanoutBuffer> DRMScanoutBuffer::createChromeDumb(int fd, uint32_t width, uint32_t height, GError** error)
{
    auto scanoutBuffer = std::unique_ptr<DRMScanoutBuffer>(new DRMScanoutBuffer(Kind::ChromeDumb));
    if (!scanoutBuffer->initializeDumb(fd, width, height, error))
        return nullptr;
    return scanoutBuffer;
}

DRMScanoutBuffer::~DRMScanoutBuffer()
{
    finishRGAAccess();
    RockchipRGA::singleton().releaseCachedHandle(m_rgaDestinationHandle);
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

uint32_t DRMScanoutBuffer::frameBufferID() const
{
    return m_drmBuffer ? m_drmBuffer->frameBufferID() : m_frameBufferID;
}

uint32_t DRMScanoutBuffer::width() const
{
    return m_drmBuffer ? gbm_bo_get_width(m_drmBuffer->bufferObject()) : m_width;
}

uint32_t DRMScanoutBuffer::height() const
{
    return m_drmBuffer ? gbm_bo_get_height(m_drmBuffer->bufferObject()) : m_height;
}

bool DRMScanoutBuffer::matchesSHMBuffer(WPEBuffer* buffer) const
{
    return m_kind == Kind::SHMDumb
        && m_width == static_cast<uint32_t>(wpe_buffer_get_width(buffer))
        && m_height == static_cast<uint32_t>(wpe_buffer_get_height(buffer))
        && m_format == DRM_FORMAT_ARGB8888;
}

bool DRMScanoutBuffer::matchesRotatedDMABuf(WPEBuffer* buffer, OutputRotation rotation) const
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

bool DRMScanoutBuffer::matchesRotatedSHM(WPEBuffer* buffer, OutputRotation rotation) const
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

bool DRMScanoutBuffer::initializeDumb(int fd, uint32_t width, uint32_t height, GError** error)
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

bool DRMScanoutBuffer::copyFromSHM(WPEBuffer* buffer, GError** error)
{
    if (!beginCPUAccess(DMA_BUF_SYNC_RW, error))
        return false;
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

bool DRMScanoutBuffer::copyRotatedFromSHM(WPEBuffer* buffer, OutputRotation rotation, GError** error)
{
    if (!beginCPUAccess(DMA_BUF_SYNC_RW, error))
        return false;
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

bool DRMScanoutBuffer::copyRotatedFromDMABuf(struct gbm_device* device, WPEBuffer* buffer, OutputRotation rotation, GError** error)
{
    if (m_scanoutOwned) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
            "Refusing RGA write to submitted or presented scanout");
        return false;
    }
    if (!finishRGAAccess(error))
        return false;
    m_lastCopyUsedRGA = false;
    m_lastCopyFellBackFromRGA = false;
    m_lastRGADurationUS = 0;
    auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer);
    auto modifier = wpe_buffer_dma_buf_get_modifier(dmaBuffer);
    if (wpe_buffer_dma_buf_get_n_planes(dmaBuffer) != 1
        || (modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID)) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
            "Failed to copy dmabuf: CPU/RGA path requires one linear plane");
        return false;
    }
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
    auto topInset = chromeFrameTopInset(panel.height, sourceHeight);
    uint32_t availableHeight = panel.height > topInset
        ? panel.height - topInset : 0;
    uint32_t copyWidth = std::min(sourceWidth, panel.width);
    uint32_t copyHeight = std::min(sourceHeight, availableHeight);
    auto destinationDamage = rotatedDestRect(0, 0, copyWidth, copyHeight,
        panel.width, panel.height, rotation, topInset);
    RGARect destinationRect {
        destinationDamage.x1,
        destinationDamage.y1,
        destinationDamage.x2 - destinationDamage.x1,
        destinationDamage.y2 - destinationDamage.y1,
    };
    bool rgaGeometrySupported = !sourceOffset && copyWidth == sourceWidth
        && copyHeight == sourceHeight && destinationRect.width > 0
        && destinationRect.height > 0;
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
        bool needsBackgroundClear = topInset || copyWidth != panel.width
            || copyHeight != availableHeight;
        if (m_rgaPrimeFD >= 0 && needsBackgroundClear) {
            if (!beginCPUAccess(DMA_BUF_SYNC_RW, error))
                return false;
            WPE::DRM::clearOutsideARGB8888(static_cast<uint8_t*>(m_mapping), m_pitch,
                m_width, m_height, destinationRect.x, destinationRect.y,
                destinationRect.width, destinationRect.height, 0xff000000);
            if (!finishRGAAccess(error))
                return false;
        }
        auto* userData = ensureBufferUserData(buffer);
        if (m_rgaPrimeFD >= 0 && rga.rotate(sourceFD, sourceWidth, sourceHeight,
                sourceStride, sourceFormat, m_rgaPrimeFD, m_width, m_height,
                m_pitch, rotation, m_lastRGADurationUS, destinationRect,
                &userData->rgaSourceHandle, &m_rgaDestinationHandle)) {
            if (!beginCPUAccess(DMA_BUF_SYNC_RW, error))
                return false;
            m_lastCopyUsedRGA = true;
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

    if (!beginCPUAccess(DMA_BUF_SYNC_RW, error))
        return false;
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

            if (!syncDMABuf(sourceFD, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ, error)) {
                gbm_bo_unmap(sourceBO, mapData);
                gbm_bo_destroy(sourceBO);
                return false;
            }
            copyRotatedPixels(static_cast<const uint8_t*>(sourceMapping), sourceWidth, sourceHeight, mapStride, rotation);
            if (sourceFormat == DRM_FORMAT_XRGB8888) {
                for (uint32_t y = 0; y < m_height; ++y) {
                    auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch);
                    for (uint32_t x = 0; x < m_width; ++x)
                        row[x] |= 0xff000000;
                }
            }
            bool synced = syncDMABuf(sourceFD, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ, error);

            // The proprietary Mali GBM implementation opens DRM device handles
            // while importing/mapping. Keeping the BO on every transient WPEBuffer
            // leaks /dev/dri/card0 descriptors until IPC fd passing fails. The
            // rotated dumb target owns the copied pixels, so release the import now.
            gbm_bo_unmap(sourceBO, mapData);
            gbm_bo_destroy(sourceBO);
            return synced;
        } else {
            userData->sourceMapping = mappedSource;
            userData->sourceMappingSize = mapSize;
            userData->sourceMappingStride = sourceStride;
        }
    }

    if (!syncDMABuf(sourceFD, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ, error))
        return false;

    const auto* source = static_cast<const uint8_t*>(userData->sourceMapping) + sourceOffset;
    copyRotatedPixels(source, sourceWidth, sourceHeight, userData->sourceMappingStride, rotation);
    if (sourceFormat == DRM_FORMAT_XRGB8888) {
        for (uint32_t y = 0; y < m_height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch);
            for (uint32_t x = 0; x < m_width; ++x)
                row[x] |= 0xff000000;
        }
    }

    return syncDMABuf(sourceFD, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ, error);
}

bool DRMScanoutBuffer::finishRGAAccess(GError** error)
{
    if (!m_cpuAccess.finish()) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
            "Failed to finish scanout CPU access: %s", strerror(m_cpuAccess.error()));
        return false;
    }
    return true;
}

bool DRMScanoutBuffer::beginCPUAccess(uint64_t direction, GError** error) const
{
    if (m_scanoutOwned && (direction & DMA_BUF_SYNC_WRITE)) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
            "Refusing CPU write to submitted or presented scanout");
        return false;
    }
    if (!m_cpuAccess.begin(m_rgaPrimeFD, direction)) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
            "Failed to begin scanout CPU access: %s", strerror(m_cpuAccess.error()));
        return false;
    }
    return true;
}

bool DRMScanoutBuffer::punchTransparentRect(int x1, int y1, int x2, int y2,
        uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset)
{
    if (!m_mapping || m_format != DRM_FORMAT_ARGB8888)
        return false;
    if (!beginCPUAccess(DMA_BUF_SYNC_RW))
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

bool DRMScanoutBuffer::copyBaseTo(Vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, uint32_t& pitch) const
{
    if (!m_mapping || m_format != DRM_FORMAT_ARGB8888 || !m_width || !m_height)
        return false;
    if (!beginCPUAccess(DMA_BUF_SYNC_READ))
        return false;
    width = m_width;
    height = m_height;
    pitch = m_width * 4;
    pixels.resize(static_cast<size_t>(pitch) * height);
    auto destination = pixels.mutableSpan();
    for (uint32_t y = 0; y < height; ++y)
        memcpy(destination.data() + static_cast<size_t>(y) * pitch,
            static_cast<const uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch, pitch);
    return m_cpuAccess.finish();
}

bool DRMScanoutBuffer::restoreBase(const Vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t pitch)
{
    if (!m_mapping || m_format != DRM_FORMAT_ARGB8888
        || width != m_width || height != m_height || pitch < width * 4
        || pixels.size() < static_cast<size_t>(pitch) * height)
        return false;
    if (!beginCPUAccess(DMA_BUF_SYNC_RW))
        return false;
    auto source = pixels.span();
    for (uint32_t y = 0; y < height; ++y)
        memcpy(static_cast<uint8_t*>(m_mapping) + static_cast<size_t>(y) * m_pitch,
            source.data() + static_cast<size_t>(y) * pitch, static_cast<size_t>(width) * 4);
    return true;
}

bool DRMScanoutBuffer::matchesDimensions(uint32_t width, uint32_t height) const
{
    return m_mapping && m_format == DRM_FORMAT_ARGB8888
        && m_width == width && m_height == height;
}

bool DRMScanoutBuffer::drawNativeChrome(uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation)
{
    if (!m_mapping || m_format != DRM_FORMAT_ARGB8888)
        return false;
    if (!beginCPUAccess(DMA_BUF_SYNC_RW))
        return false;
    drawChromeOverlay(static_cast<uint8_t*>(m_mapping), m_pitch, m_width, m_height,
        panelWidth, panelHeight, rotation);
    return true;
}

void DRMScanoutBuffer::copyRotatedPixels(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t sourceStride, OutputRotation rotation)
{
    auto* destination = static_cast<uint8_t*>(m_mapping);
    auto panel = scanoutPanelForSource(sourceWidth, sourceHeight);
    copyRotatedARGB8888(source, sourceWidth, sourceHeight, sourceStride, destination,
        m_pitch, panel.width, panel.height, rotation);
}

void DRMScanoutBuffer::setFenceFD(UnixFileDescriptor&& fenceFD)
{
    if (m_drmBuffer)
        m_drmBuffer->setFenceFD(WTF::move(fenceFD));
    else
        m_fenceFD = WTF::move(fenceFD);
}

const UnixFileDescriptor& DRMScanoutBuffer::fenceFD() const LIFETIME_BOUND
{
    return m_drmBuffer ? m_drmBuffer->fenceFD() : m_fenceFD;
}

DRMScanoutBuffer::DRMScanoutBuffer(Kind kind)
        : m_kind(kind)
{
}

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
    RockchipRGA::singleton().releaseCachedHandle(rgaSourceHandle);
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
