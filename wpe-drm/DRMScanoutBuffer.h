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

#pragma once

#include "DRMGeometry.h"
#include "RockchipRGA.h"
#include "DMABufCPUAccess.h"

class DRMScanoutBuffer;

// 挂在 WPEBuffer user_data 上的缓存：直扫路径缓存 scanout buffer（原有行为），
// 旋转路径缓存源 dmabuf 的 mmap 映射，避免每帧 mmap/munmap（WebKit 复用一个
// 小 buffer 池，映射可以跟随 buffer 生命周期）。
struct WPEBufferDRMUserData {
    DRMScanoutBuffer* scanoutBuffer { nullptr }; // owned
    void* sourceMapping { nullptr };
    size_t sourceMappingSize { 0 };
    uint32_t sourceMappingStride { 0 };
    RGACachedHandle rgaSourceHandle;
    void resetSourceMapping();
    ~WPEBufferDRMUserData();
};

inline WPEBufferDRMUserData* ensureBufferUserData(WPEBuffer* buffer)
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

    static std::unique_ptr<DRMScanoutBuffer> createDMABuf(std::unique_ptr<WPE::DRM::Buffer>&& buffer);

    static std::unique_ptr<DRMScanoutBuffer> createDMABufDirect(int fd, WPEBuffer* buffer, GError** error);

    static std::unique_ptr<DRMScanoutBuffer> createRotatedDMABufDumb(struct gbm_device* device, int fd, WPEBuffer* buffer, OutputRotation rotation, GError** error);

    static std::unique_ptr<DRMScanoutBuffer> createSHMDumb(int fd, WPEBuffer* buffer, GError** error);

    static std::unique_ptr<DRMScanoutBuffer> createRotatedSHMDumb(int fd, WPEBuffer* buffer, OutputRotation rotation, GError** error);

    static std::unique_ptr<DRMScanoutBuffer> createChromeDumb(int fd, uint32_t width, uint32_t height, GError** error);

    ~DRMScanoutBuffer();

    Kind kind() const { return m_kind; }

    uint32_t frameBufferID() const;

    uint32_t width() const;

    uint32_t height() const;

    bool matchesSHMBuffer(WPEBuffer* buffer) const;

    bool matchesRotatedDMABuf(WPEBuffer* buffer, OutputRotation rotation) const;

    bool matchesRotatedSHM(WPEBuffer* buffer, OutputRotation rotation) const;

    bool initializeDumb(int fd, uint32_t width, uint32_t height, GError** error);

    bool copyFromSHM(WPEBuffer* buffer, GError** error);

    bool copyRotatedFromSHM(WPEBuffer* buffer, OutputRotation rotation, GError** error);

    bool copyRotatedFromDMABuf(struct gbm_device* device, WPEBuffer* buffer, OutputRotation rotation, GError** error);

    bool lastCopyUsedRGA() const { return m_lastCopyUsedRGA; }
    bool lastCopyFellBackFromRGA() const { return m_lastCopyFellBackFromRGA; }
    gint64 lastRGADurationUS() const { return m_lastRGADurationUS; }

    bool finishRGAAccess(GError** error = nullptr);

    bool punchTransparentRect(int x1, int y1, int x2, int y2,
        uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset);

    bool copyBaseTo(Vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, uint32_t& pitch) const;

    bool restoreBase(const Vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t pitch);

    bool matchesDimensions(uint32_t width, uint32_t height) const;

    bool drawNativeChrome(uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation);

    bool beginCPUAccess(uint64_t direction, GError** error = nullptr) const;
    void setScanoutOwned(bool owned) { m_scanoutOwned = owned; }

    void copyRotatedPixels(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t sourceStride, OutputRotation rotation);

    void setFenceFD(UnixFileDescriptor&& fenceFD);

    const UnixFileDescriptor& fenceFD() const LIFETIME_BOUND;

private:
    explicit DRMScanoutBuffer(Kind kind);

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
    RGACachedHandle m_rgaDestinationHandle;
    mutable WPE::DRM::DMABufCPUAccess m_cpuAccess;
    bool m_scanoutOwned { false };
    bool m_lastCopyUsedRGA { false };
    bool m_lastCopyFellBackFromRGA { false };
    gint64 m_lastRGADurationUS { 0 };
    mutable UnixFileDescriptor m_fenceFD;
};
