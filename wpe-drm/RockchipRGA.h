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

#include "DRMRenderingIncludes.h"
#include "DRMGeometry.h"

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

struct RGACachedHandle {
    int fd { -1 };
    RGAHandleParameters parameters { };
    RGABufferHandle handle { 0 };
};

struct RGARect {
    int x;
    int y;
    int width;
    int height;
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
        uint32_t destinationStride, OutputRotation rotation, gint64& durationUS,
        RGARect destinationRect = { }, RGACachedHandle* sourceCache = nullptr,
        RGACachedHandle* destinationCache = nullptr)
    {
        durationUS = 0;
        if (!shouldAttempt())
            return false;

        int sourceRGAFormat = drmFormatToRGA(sourceFormat);
        int transform = rotationToRGA(rotation);
        if (sourceRGAFormat < 0 || (rotation != OutputRotation::Rotate0 && !transform))
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
        RGACachedHandle localSource;
        RGACachedHandle localDestination;
        auto& cachedSource = sourceCache ? *sourceCache : localSource;
        auto& cachedDestination = destinationCache ? *destinationCache : localDestination;
        RGABufferHandle sourceHandle = importHandle(sourceFD, sourceParameters, cachedSource);
        RGABufferHandle destinationHandle = importHandle(destinationFD,
            destinationParameters, cachedDestination);
        if (!sourceHandle || !destinationHandle) {
            if (!sourceCache)
                releaseCachedHandle(cachedSource);
            if (!destinationCache)
                releaseCachedHandle(cachedDestination);
            return fail("importbuffer_fd failed");
        }

        auto source = m_wrapBufferHandle(sourceHandle, sourceWidth, sourceHeight,
            sourceStride / 4, sourceHeight, sourceRGAFormat);
        auto destination = m_wrapBufferHandle(destinationHandle,
            destinationWidth, destinationHeight, destinationStride / 4,
            destinationHeight, rkFormatBGRA8888);
        if (destinationRect.width <= 0 || destinationRect.height <= 0)
            destinationRect = { 0, 0, static_cast<int>(destinationWidth),
                static_cast<int>(destinationHeight) };
        gint64 startUS = g_get_monotonic_time();
        int result = 0;
        bool useDestinationRect = destinationRect.x || destinationRect.y
            || destinationRect.width != static_cast<int>(destinationWidth)
            || destinationRect.height != static_cast<int>(destinationHeight);
        bool useProcess = m_process
            && (rotation == OutputRotation::Rotate0 || useDestinationRect);
        if (useProcess) {
            RGABuffer pattern { };
            RGARect sourceRect { 0, 0, static_cast<int>(sourceWidth),
                static_cast<int>(sourceHeight) };
            RGARect patternRect { };
            result = m_process(source, destination, pattern, sourceRect,
                destinationRect, patternRect, transform | (1 << 19)); // IM_SYNC
        } else if (!useDestinationRect && rotation != OutputRotation::Rotate0)
            result = m_rotate(source, destination, transform, 1);
        else
            result = 0;
        durationUS = g_get_monotonic_time() - startUS;
        if (!sourceCache)
            releaseCachedHandle(cachedSource);
        if (!destinationCache)
            releaseCachedHandle(cachedDestination);
        if (result <= 0)
            return fail(useProcess ? "improcess blit failed"
                : useDestinationRect ? "improcess inset rotation unavailable"
                                     : "imrotate_t failed", result);

        m_consecutiveFailures = 0;
        return true;
    }

    void releaseCachedHandle(RGACachedHandle& cache)
    {
        if (cache.handle && m_releaseBufferHandle)
            m_releaseBufferHandle(cache.handle);
        cache = { };
    }

    uint64_t importCount() const { return m_importCount; }
    uint64_t reuseCount() const { return m_reuseCount; }

private:
    using ImportBufferFDFunction = RGABufferHandle (*)(int, RGAHandleParameters*);
    using WrapBufferHandleFunction = RGABuffer (*)(RGABufferHandle, int, int, int, int, int);
    using ReleaseBufferHandleFunction = int (*)(RGABufferHandle);
    using RotateFunction = int (*)(const RGABuffer, RGABuffer, int, int);
    using ProcessFunction = int (*)(RGABuffer, RGABuffer, RGABuffer,
        RGARect, RGARect, RGARect, int);

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

    RGABufferHandle importHandle(int fd, const RGAHandleParameters& parameters,
        RGACachedHandle& cache)
    {
        bool matches = cache.handle && cache.fd == fd
            && cache.parameters.width == parameters.width
            && cache.parameters.height == parameters.height
            && cache.parameters.format == parameters.format;
        if (matches) {
            m_reuseCount++;
            return cache.handle;
        }
        releaseCachedHandle(cache);
        cache.fd = fd;
        cache.parameters = parameters;
        cache.handle = m_importBufferFD(fd, &cache.parameters);
        if (cache.handle)
            m_importCount++;
        else
            cache = { };
        return cache.handle;
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
        m_process = reinterpret_cast<ProcessFunction>(dlsym(m_library, "improcess"));
        if (!m_importBufferFD || !m_wrapBufferHandle || !m_releaseBufferHandle || !m_rotate) {
            g_warning("WPEViewDRM rga_rotation=unavailable path=%s error=missing-im2d-symbols", path);
            dlclose(m_library);
            m_library = nullptr;
            return false;
        }
        g_message("WPEViewDRM rga_rotation=available mode=%s inset=%d path=%s",
            mode() == Mode::Required ? "required" : "auto", !!m_process, path);
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
    ProcessFunction m_process { nullptr };
    uint64_t m_importCount { 0 };
    uint64_t m_reuseCount { 0 };
};

