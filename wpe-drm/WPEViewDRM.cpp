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
#include "DRMFrameQueue.h"

_WEBKIT_DEFINE_TYPE_EXTENDED(WPEViewDRM, wpe_view_drm, WPE_TYPE_VIEW, G_TYPE_FLAG_FINAL, { })

static void wpeViewDRMDidPageFlip(WPEViewDRM*);
gboolean wpeViewDRMRequestUpdate(WPEViewDRM*, GError**);
static bool captureCommittedChromeBase(WPEViewDRM*);
static DRMScanoutBuffer* drmScanoutBufferForRender(WPEViewDRM*, WPEBuffer*, OutputRotation, GError**);

static bool refreshContentGeometry(WPEViewDRM* view)
{
    auto* priv = view->priv;
    auto panel = configuredPanelSize();
    int width = wpe_view_get_width(WPE_VIEW(view));
    int height = wpe_view_get_height(WPE_VIEW(view));
    auto sourceHeight = priv->committedBuffer
        ? static_cast<uint32_t>(wpe_buffer_get_height(priv->committedBuffer.get()))
        : static_cast<uint32_t>(std::max(0, height));
    uint32_t inset = chromeFrameTopInset(panel.height, sourceHeight);
    if (width == priv->geometryViewWidth && height == priv->geometryViewHeight
        && inset == priv->geometryInset)
        return false;
    if (priv->geometryViewWidth >= 0)
        ++priv->geometryEpoch;
    priv->geometryViewWidth = width;
    priv->geometryViewHeight = height;
    priv->geometryInset = inset;
    priv->chromeBaseGeneration = 0;
    priv->preparedBaseGeneration = 0;
    return true;
}

static guint64 wpeViewDRMCacheBytes(WPEViewDRM* view)
{
    auto* priv = view->priv;
    return priv->chromeBasePixels.capacity()
        + priv->preparedChromeBasePixels.capacity()
        + priv->damageRects.capacity() * sizeof(drm_mode_rect)
        + priv->queuedDamageRects.capacity() * sizeof(drm_mode_rect)
        + chromeFontCacheBytes();
}

guint64 wpe_view_drm_get_cache_bytes(WPEViewDRM* view)
{
    g_return_val_if_fail(WPE_IS_VIEW_DRM(view), 0);
    return wpeViewDRMCacheBytes(view);
}

void wpe_view_drm_trim_caches(WPEViewDRM* view, gboolean critical)
{
    g_return_if_fail(WPE_IS_VIEW_DRM(view));
    auto* priv = view->priv;
    ChromeRenderScope scope(priv->chromeCache);
    guint64 before = wpeViewDRMCacheBytes(view);
    bool frameInFlight = priv->pendingBuffer || priv->pendingScanoutBuffer
        || priv->queuedBuffer || priv->updateFlags.contains(UpdateFlags::BufferUpdateRequested);

    if (!frameInFlight) {
        priv->preparedChromeBasePixels.clear();
        priv->preparedChromeBasePixels.shrinkToFit();
        priv->preparedChromeBaseWidth = 0;
        priv->preparedChromeBaseHeight = 0;
        priv->preparedChromeBasePitch = 0;
        priv->preparedBaseGeneration = 0;
        priv->damageRects.clear();
        priv->damageRects.shrinkToFit();
        priv->queuedDamageRects.clear();
        priv->queuedDamageRects.shrinkToFit();
        const auto& chrome = cachedChromeRenderState();
        bool baseNeeded = priv->chromeMotionPending || chromeAnimationActive()
            || strcmp(chrome.panel, "none");
        if (critical && !baseNeeded) {
            priv->chromeBasePixels.clear();
            priv->chromeBasePixels.shrinkToFit();
            priv->chromeBaseWidth = 0;
            priv->chromeBaseHeight = 0;
            priv->chromeBasePitch = 0;
            priv->chromeBaseGeneration = 0;
        }
    }
    trimChromeFontCaches(critical);
    guint64 after = wpeViewDRMCacheBytes(view);
    g_message("WPEViewDRM memory trim: level=%s frame_in_flight=%d before=%" G_GUINT64_FORMAT " after=%" G_GUINT64_FORMAT " released=%" G_GUINT64_FORMAT,
        critical ? "critical" : "warning", frameInFlight, before, after,
        before > after ? before - after : 0);
}

static gboolean wpeViewDRMChromeTick(gpointer userData)
{
    auto* view = WPE_VIEW_DRM(userData);
    auto* priv = view->priv;
    ChromeRenderScope scope(priv->chromeCache);
    bool stateChanged = std::exchange(priv->chromeSnapshotChanged, false);
    stateChanged |= refreshChromeRenderState(WPE_VIEW(view));
    stateChanged |= refreshContentGeometry(view);
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

    if (stateChanged || animationActive)
        videoOverlayRecommit(view);
    bool webFrameWaiting = priv->pendingBuffer || priv->queuedBuffer
        || priv->frameThrottleSource
        || priv->updateFlags.containsAny({ UpdateFlags::BufferUpdateRequested,
            UpdateFlags::BufferUpdatePending, UpdateFlags::CursorUpdateRequested });
    if (priv->chromeMotionPending && priv->committedBuffer && !webFrameWaiting) {
        bool baseReady = priv->chromeBaseGeneration
                == priv->committedFrameGeneration
            || captureCommittedChromeBase(view);
        if (baseReady && wpeViewDRMRequestUpdate(view, nullptr)) {
            priv->updateFlags.add(UpdateFlags::BufferUpdateRequested);
            wpeViewDRMCompleteSynchronousCommitIfNeeded(view);
            priv->chromeMotionPending = false;
            priv->chromeOverlayCommits++;
        } else if (!baseReady)
            priv->staleChromeRedrawCount++;
    } else if (priv->chromeMotionPending && webFrameWaiting)
        priv->staleChromeRedrawCount++;

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
    if (animationActive || (priv->chromeMotionPending && webFrameWaiting))
        return G_SOURCE_CONTINUE;
    priv->chromeSource = nullptr;
    return G_SOURCE_REMOVE;
}

void wpe_view_drm_request_chrome_redraw(WPEViewDRM* view)
{
    g_return_if_fail(WPE_IS_VIEW_DRM(view));
    auto* priv = view->priv;
    ChromeRenderScope scope(priv->chromeCache);
    priv->chromeSnapshotChanged |= refreshChromeRenderState(WPE_VIEW(view));
    priv->chromeSnapshotChanged |= refreshContentGeometry(view);
    priv->chromeMotionPending = true;
    if (priv->chromeSource)
        return;
    priv->chromeSource = adoptGRef(g_timeout_source_new(16));
    g_source_set_name(priv->chromeSource.get(), "WPE requested chrome redraw");
    g_source_set_callback(priv->chromeSource.get(), wpeViewDRMChromeTick, view, nullptr);
    g_source_attach(priv->chromeSource.get(), g_main_context_get_thread_default());
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

    ChromeRenderScope chromeScope(priv->chromeCache);
    refreshChromeRenderState(view);
    priv->chromeMotionStatsStartUS = g_get_monotonic_time();

    videoOverlayStart(WPE_VIEW_DRM(view));
}

static void wpeViewDRMDispose(GObject* object)
{
    auto* view = WPE_VIEW_DRM(object);
    auto* priv = view->priv;
    bool pendingGPUComplete = WPE::DRM::probeRenderingFence(priv->pendingRenderingFence.value())
        == WPE::DRM::FenceResult::Ready;
    priv->renderingFenceWait.cancel();
    priv->pendingRenderingFence = { };
    priv->pendingFenceTaken = false;
    priv->discardedRenderingFences.clear();
    priv->frameTrace.dump();

    videoOverlayStop(WPE_VIEW_DRM(object));

    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    int fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    bool scanoutDisabled = false;
    if (wpe_display_drm_supports_atomic(display)) {
        WPE::DRM::UniquePtr<drmModeAtomicReq> request(drmModeAtomicAlloc());
        const auto& plane = wpeDisplayDRMGetPrimaryPlane(display);
        scanoutDisabled = addPlaneProperties(request.get(), plane, emptyPlaneProperties(plane))
            && !drmModeAtomicCommit(fd, request.get(), 0, nullptr);
    } else {
        const auto& plane = wpeDisplayDRMGetPrimaryPlane(display);
        scanoutDisabled = !drmModeSetPlane(fd, plane.id(), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }
    if (!scanoutDisabled)
        g_warning("WPEViewDRM shutdown: scanout disable failed; suppressing producer reuse until channel teardown");

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

    // Every buffer accepted from WPE must receive exactly one release, even
    // when the view is destroyed while a page flip or throttled frame is
    // pending. Avoid duplicate callbacks if a producer reuses an object.
    std::array<WPEBuffer*, 3> released { nullptr, nullptr, nullptr };
    size_t releasedCount = 0;
    auto releaseBuffer = [&](GRefPtr<WPEBuffer>& buffer) {
        if (!buffer)
            return;
        WPEBuffer* raw = buffer.get();
        bool duplicate = std::find(released.begin(), released.begin() + releasedCount, raw)
            != released.begin() + releasedCount;
        if (!duplicate) {
            auto fence = UnixFileDescriptor { wpe_buffer_take_rendering_fence(raw), UnixFileDescriptor::Adopt };
            bool complete = raw == priv->pendingBuffer.get() ? pendingGPUComplete
                : WPE::DRM::probeRenderingFence(fence.value()) == WPE::DRM::FenceResult::Ready;
            if (scanoutDisabled && complete)
                wpe_view_buffer_released(WPE_VIEW(view), raw);
            released[releasedCount++] = raw;
        }
        buffer = nullptr;
    };
    releaseBuffer(priv->queuedBuffer);
    releaseBuffer(priv->pendingBuffer);
    releaseBuffer(priv->committedBuffer);
    priv->pendingScanoutBuffer = nullptr;
    priv->committedScanoutBuffer = nullptr;
    priv->updateFlags = { };

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

static bool captureCommittedChromeBase(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (!priv->committedBuffer || priv->pendingBuffer || priv->atomicInFlight)
        return false;
    // The retained WPE source has completed rendering and has not been
    // released to its producer. Rebuild in a spare target, never read the
    // decorated framebuffer currently being scanned out.
    priv->captureBaseRequested = true;
    GError* error = nullptr;
    auto* scratch = drmScanoutBufferForRender(view, priv->committedBuffer.get(),
        priv->outputRotation, &error);
    priv->captureBaseRequested = false;
    if (!scratch || !priv->preparedBaseGeneration) {
        if (error)
            g_warning("WPE clean base reconstruction failed: %s", error->message);
        g_clear_error(&error);
        return false;
    }
    std::swap(priv->chromeBasePixels, priv->preparedChromeBasePixels);
    priv->chromeBaseWidth = priv->preparedChromeBaseWidth;
    priv->chromeBaseHeight = priv->preparedChromeBaseHeight;
    priv->chromeBasePitch = priv->preparedChromeBasePitch;
    priv->chromeBaseGeneration = priv->committedFrameGeneration;
    priv->preparedBaseGeneration = 0;
    priv->preparedChromeBaseWidth = 0;
    priv->preparedChromeBaseHeight = 0;
    priv->preparedChromeBasePitch = 0;
    return true;
}

static bool prepareChromeBaseIfNeeded(WPEViewDRM* view,
                                      DRMScanoutBuffer* buffer)
{
    auto* priv = view->priv;
    const auto& chrome = cachedChromeRenderState();
    bool panelOpen = chrome.panel[0] && strcmp(chrome.panel, "none");
    if (!priv->captureBaseRequested && !panelOpen && !chromeAnimationActive()) {
        priv->preparedBaseGeneration = 0;
        return false;
    }

    priv->preparedChromeBaseWidth = 0;
    priv->preparedChromeBaseHeight = 0;
    priv->preparedChromeBasePitch = 0;
    if (!buffer || !buffer->copyBaseTo(priv->preparedChromeBasePixels,
            priv->preparedChromeBaseWidth, priv->preparedChromeBaseHeight,
            priv->preparedChromeBasePitch)) {
        priv->preparedBaseGeneration = 0;
        return false;
    }
    priv->preparedBaseGeneration = priv->committedFrameGeneration + 1;
    return true;
}

static bool composeNativeChrome(WPEViewDRM* view, DRMScanoutBuffer* buffer)
{
    if (!buffer)
        return false;
    if (!buffer->beginCPUAccess(DMA_BUF_SYNC_RW))
        return false;
    auto panel = configuredPanelSize();
    videoOverlayPunchActiveHole(view, buffer);
    view->priv->composedChromeSequence = view->priv->chromeCache.snapshotSequence;
    view->priv->composedMotionSequence = view->priv->chromeMotionSequence;
    return buffer->drawNativeChrome(panel.width, panel.height, view->priv->outputRotation);
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
                || candidate.get() == priv->pendingScanoutBuffer))
            continue;

        if (!candidate || !candidate->matchesSHMBuffer(buffer)) {
            candidate = DRMScanoutBuffer::createSHMDumb(fd, buffer, error);
            if (!candidate)
                return nullptr;
        } else if (!candidate->copyFromSHM(buffer, error))
            return nullptr;

        prepareChromeBaseIfNeeded(view, candidate.get());
        if (!composeNativeChrome(view, candidate.get()) || !candidate->finishRGAAccess(error))
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
    auto* device = wpe_display_drm_get_device(display);
    int fd = gbm_device_get_fd(device);

    for (unsigned i = 0; i < priv->rotatedScanoutBuffers.size(); ++i) {
        auto index = (priv->nextRotatedScanoutBufferIndex + i) % priv->rotatedScanoutBuffers.size();
        auto& candidate = priv->rotatedScanoutBuffers[index];
        if (candidate
            && (candidate.get() == priv->committedScanoutBuffer
                || candidate.get() == priv->pendingScanoutBuffer))
            continue;

        if (!candidate || !candidate->matchesRotatedDMABuf(buffer, rotation)) {
            candidate = DRMScanoutBuffer::createRotatedDMABufDumb(device, fd, buffer, rotation, error);
            if (!candidate)
                return nullptr;
        } else if (!candidate->copyRotatedFromDMABuf(device, buffer, rotation, error))
            return nullptr;

        prepareChromeBaseIfNeeded(view, candidate.get());
        if (!composeNativeChrome(view, candidate.get()))
            return nullptr;
        if (!candidate->finishRGAAccess(error))
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
                || candidate.get() == priv->pendingScanoutBuffer))
            continue;

        if (!candidate || !candidate->matchesRotatedSHM(buffer, rotation)) {
            candidate = DRMScanoutBuffer::createRotatedSHMDumb(fd, buffer, rotation, error);
            if (!candidate)
                return nullptr;
        } else if (!candidate->copyRotatedFromSHM(buffer, rotation, error))
            return nullptr;

        prepareChromeBaseIfNeeded(view, candidate.get());
        if (!composeNativeChrome(view, candidate.get()) || !candidate->finishRGAAccess(error))
            return nullptr;
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
        if (!composeNativeChrome(view, candidate.get()) || !candidate->finishRGAAccess(error))
            return nullptr;
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
        if (rotation != OutputRotation::Rotate0 || nativeChromeCompositionRequired())
            return nextRotatedDMABufBuffer(view, buffer, rotation, error);

        // Native chrome is disabled, so an unrotated DMA-BUF can remain on the
        // zero-copy scanout path.
        auto* userData = static_cast<WPEBufferDRMUserData*>(wpe_buffer_get_user_data(buffer));
        auto* scanoutBuffer = userData ? userData->scanoutBuffer : nullptr;
        if (!scanoutBuffer)
            scanoutBuffer = drmBufferCreateDMABuf(WPE_VIEW(view), buffer, wpe_display_drm_supports_modifiers(display), error);
        return scanoutBuffer;
    }

    if (WPE_IS_BUFFER_SHM(buffer)) {
        if (rotation != OutputRotation::Rotate0 || nativeChromeCompositionRequired())
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

bool shouldUsePanelNativeFit()
{
    return !strcmp(drmFitMode(), "panel-native");
}

std::optional<uint32_t> configuredRotatedXOffset(uint32_t maxOffset)
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
        // panel-native is a 1:1 pixel policy. The DRM mode is only the scanout
        // envelope and must never silently scale a panel or content buffer.
        // Oversized buffers are clipped; smaller buffers are centered (with an
        // optional calibrated X offset for panels exposed through a wider mode).
        width = std::min<uint32_t>(buffer.width(), mode->hdisplay);
        height = std::min<uint32_t>(buffer.height(), mode->vdisplay);
        uint32_t maxX = mode->hdisplay > width ? mode->hdisplay - width : 0;
        if (auto configuredX = configuredRotatedXOffset(maxX))
            x = configuredX.value();
        else
            x = maxX / 2;
        y = mode->vdisplay > height ? (mode->vdisplay - height) / 2 : 0;
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
        properties.inFenceFD.second = static_cast<uint64_t>(-1);
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

bool addPlaneProperties(drmModeAtomicReq* request, const WPE::DRM::Plane& plane, WPE::DRM::Plane::Properties&& properties)
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
    auto inFence = plane.properties().inFenceFD;
    if (inFence.first) {
        inFence.second = buffer && buffer->fenceFD()
            ? buffer->fenceFD().value() : static_cast<uint64_t>(-1);
        if (!drmAtomicAddProperty(request.get(), plane.id(), inFence)) {
            g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
                "Failed to set DRM rendering fence");
            return false;
        }
    }
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
    if (buffer)
        buffer->setScanoutOwned(true);
    view->priv->atomicInFlight = !synchronousCommit;

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

void setDamageRects(Vector<drm_mode_rect>& destination, const WPERectangle* damageRects, guint nDamageRects)
{
    destination.clear();
    destination.reserveInitialCapacity(nDamageRects);
    for (unsigned i = 0; i < nDamageRects; ++i)
        destination.append({ damageRects[i].x, damageRects[i].y, damageRects[i].x + damageRects[i].width, damageRects[i].y + damageRects[i].height });
}

static bool bufferNeedsCPURead(WPEBuffer* buffer, OutputRotation rotation)
{
    return WPE_IS_BUFFER_SHM(buffer)
        || (WPE_IS_BUFFER_DMA_BUF(buffer)
            && (rotation != OutputRotation::Rotate0 || nativeChromeCompositionRequired()));
}

static bool abortPendingCommit(WPEViewDRM* view, const char* reason)
{
    auto* priv = view->priv;
    priv->renderingFenceWait.cancel();
    priv->pendingFenceTaken = false;
    bool droppedWebFrame = !!priv->pendingBuffer;
    if (priv->pendingBuffer) {
        // FrameDone and ReleaseBuffer are separate IPC messages. A discarded
        // frame must also unblock the compositor, without advancing scanout.
        priv->frameTrace.record(priv->pendingTicket.serial, priv->pendingTicket.bufferID,
            priv->committedFrameGeneration, priv->geometryEpoch, priv->chromeMotionSequence,
            WPE::DRM::FrameStage::Discarded);
        if (priv->pendingTicket.complete(priv->pendingTicket.serial))
            wpe_view_buffer_rendered(WPE_VIEW(view), priv->pendingBuffer.get());
        retireDiscardedBuffer(view, priv->pendingBuffer.get(), WTF::move(priv->pendingRenderingFence));
    }
    priv->pendingRenderingFence = { };
    priv->pendingBuffer = nullptr;
    priv->pendingTicket = { };
    priv->pendingScanoutBuffer = nullptr;
    priv->preparedChromeBasePixels.clear();
    priv->preparedChromeBaseWidth = 0;
    priv->preparedChromeBaseHeight = 0;
    priv->preparedChromeBasePitch = 0;
    priv->preparedBaseGeneration = 0;
    priv->damageRects.clear();
    priv->lastUpdateDroppedBuffer = droppedWebFrame;
    priv->forceFullDamageNextFrame = true;
    if (droppedWebFrame)
        priv->droppedWindowCount++;
    g_warning("WPEViewDRM pending commit aborted: reason=%s web_frame=%d",
        reason ? reason : "unknown", droppedWebFrame);
    return droppedWebFrame;
}

static void renderingFenceFinished(WPE::DRM::FenceResult result, gpointer data)
{
    auto* view = WPE_VIEW_DRM(data);
    auto* priv = view->priv;
    auto elapsedUS = g_get_monotonic_time() - priv->pendingFenceStartUS;
    priv->fenceWaitTotalUS += elapsedUS;
    priv->fenceWaitMaxUS = std::max(priv->fenceWaitMaxUS, elapsedUS);
    priv->updateFlags.remove(UpdateFlags::BufferUpdateRequested);
    if (result != WPE::DRM::FenceResult::Ready) {
        priv->fenceTimeoutCount++;
        priv->retainedBaseFrameCount++;
        abortPendingCommit(view, result == WPE::DRM::FenceResult::TimedOut
            ? "fence-timeout" : "fence-error");
    } else {
        GError* error = nullptr;
        if (wpeViewDRMRequestUpdate(view, &error) && !priv->lastUpdateDroppedBuffer) {
            priv->updateFlags.add(UpdateFlags::BufferUpdateRequested);
            wpeViewDRMCompleteSynchronousCommitIfNeeded(view);
        } else {
            if (error)
                g_warning("WPEViewDRM fence-ready commit failed: %s", error->message);
            g_clear_error(&error);
        }
    }
    // A failed frame has no page-flip event to drain the latest queued frame.
    if (!priv->updateFlags.contains(UpdateFlags::BufferUpdateRequested) && priv->queuedBuffer)
        wpeViewDRMScheduleQueuedCommit(view, 0);
}

gboolean wpeViewDRMRequestUpdate(WPEViewDRM* view, GError** error)
{
    auto* priv = view->priv;
    ChromeRenderScope scope(priv->chromeCache);
    auto* buffer = priv->pendingBuffer ? priv->pendingBuffer.get() : priv->committedBuffer.get();
    if (priv->pendingBuffer && priv->pendingTicket.geometryEpoch != priv->geometryEpoch) {
        abortPendingCommit(view, "obsolete-geometry");
        return TRUE;
    }
    DRMScanoutBuffer* drmBuffer = nullptr;
    auto rotation = priv->outputRotation;
    bool needsScanoutRebuild = buffer && (rotation != OutputRotation::Rotate0
        || (priv->committedScanoutBuffer
            && (priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::DMABufRotatedDumb
                || priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb
                || priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::SHMDumb
                || priv->committedScanoutBuffer->kind() == DRMScanoutBuffer::Kind::ChromeDumb)));
    bool webFrameWaiting = priv->pendingBuffer || priv->queuedBuffer
        || priv->frameThrottleSource
        || priv->updateFlags.contains(UpdateFlags::BufferUpdatePending);
    if (!priv->pendingBuffer && webFrameWaiting)
        needsScanoutRebuild = false;
    bool uiOnlyRedraw = !webFrameWaiting && needsScanoutRebuild
        && !priv->chromeBasePixels.isEmpty();
    if (priv->pendingBuffer || needsScanoutRebuild) {
        UnixFileDescriptor renderingFence;
        bool pendingNeedsCPURead = priv->pendingBuffer && bufferNeedsCPURead(buffer, rotation);
        if (priv->pendingBuffer) {
            if (!priv->pendingFenceTaken) {
                priv->pendingRenderingFence = UnixFileDescriptor { wpe_buffer_take_rendering_fence(buffer), UnixFileDescriptor::Adopt };
                priv->pendingFenceTaken = true;
            }
            auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
            bool canSubmitFence = wpe_display_drm_supports_atomic(display)
                && wpeDisplayDRMGetPrimaryPlane(display).properties().inFenceFD.first;
            if (priv->pendingRenderingFence && (pendingNeedsCPURead || !canSubmitFence)) {
                auto result = WPE::DRM::probeRenderingFence(priv->pendingRenderingFence.value());
                if (result == WPE::DRM::FenceResult::Pending) {
                    if (!priv->renderingFenceWait.active()) {
                        RELEASE_ASSERT(priv->pendingTicket.advance(WPE::DRM::FramePhase::WaitingFence));
                        priv->pendingFenceStartUS = g_get_monotonic_time();
                        priv->fenceWaitCount++;
                        priv->renderingFenceWait.start(priv->pendingRenderingFence.value(),
                            renderingFenceTimeoutMS(), renderingFenceFinished, view,
                            g_main_context_get_thread_default());
                    }
                    priv->lastCommitWasSynchronous = false;
                    return TRUE;
                }
                if (result != WPE::DRM::FenceResult::Ready) {
                    priv->fenceTimeoutCount++;
                    priv->retainedBaseFrameCount++;
                    abortPendingCommit(view, "fence-error");
                    return TRUE;
                }
            }
            renderingFence = WTF::move(priv->pendingRenderingFence);
            priv->pendingFenceTaken = false;
            RELEASE_ASSERT(priv->pendingTicket.advance(WPE::DRM::FramePhase::Preparing));
            priv->frameTrace.record(priv->pendingTicket.serial, priv->pendingTicket.bufferID,
                priv->committedFrameGeneration, priv->geometryEpoch, priv->chromeMotionSequence,
                WPE::DRM::FrameStage::FenceReady);
        }

        gint64 copyStartUS = g_get_monotonic_time();
        drmBuffer = uiOnlyRedraw
            ? nextChromeOverlayBuffer(view, error)
            : drmScanoutBufferForRender(view, buffer, rotation, error);
        if (!drmBuffer) {
            if (abortPendingCommit(view, "scanout-import")) {
                if (error)
                    g_clear_error(error);
                return TRUE;
            }
            return FALSE;
        }

        if (uiOnlyRedraw) {
            auto durationUS = g_get_monotonic_time() - copyStartUS;
            priv->uiOverlayWindowTotalUS += durationUS;
            priv->uiOverlayWindowMaxUS = std::max(priv->uiOverlayWindowMaxUS, durationUS);
            priv->uiOverlayWindowCount++;
        } else if (drmBuffer->kind() == DRMScanoutBuffer::Kind::SHMDumb
            || drmBuffer->kind() == DRMScanoutBuffer::Kind::SHMRotatedDumb
            || drmBuffer->kind() == DRMScanoutBuffer::Kind::DMABufRotatedDumb) {
            auto copyDurationUS = g_get_monotonic_time() - copyStartUS;
            priv->copyWindowTotalUS += copyDurationUS;
            priv->copyWindowMaxUS = std::max(priv->copyWindowMaxUS, copyDurationUS);
            priv->copyWindowCount++;
            if (drmBuffer->lastCopyUsedRGA()) {
                priv->rgaRotateWindowTotalUS += drmBuffer->lastRGADurationUS();
                priv->rgaRotateWindowMaxUS = std::max(priv->rgaRotateWindowMaxUS,
                    drmBuffer->lastRGADurationUS());
                priv->rgaRotateWindowCount++;
            } else if (drmBuffer->lastCopyFellBackFromRGA())
                priv->rgaCPUFallbackWindowCount++;
        }
        if (uiOnlyRedraw || pendingNeedsCPURead)
            priv->damageRects.clear();

        if (priv->pendingBuffer) {
            RELEASE_ASSERT(priv->pendingTicket.advance(WPE::DRM::FramePhase::Ready));
            priv->frameTrace.record(priv->pendingTicket.serial, priv->pendingTicket.bufferID,
                priv->committedFrameGeneration, priv->geometryEpoch, priv->chromeMotionSequence,
                WPE::DRM::FrameStage::Prepared);
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
        if (damageID.has_value() && !damageID.value()) {
            if (abortPendingCommit(view, "damage-blob")) {
                if (error)
                    g_clear_error(error);
                return TRUE;
            }
            return FALSE;
        }

        auto result = wpeViewDRMCommitAtomic(WPE_VIEW_DRM(view), drmBuffer, damageID, error);
        if (damageID)
            destroyDamageBlob(display, damageID.value());
        priv->damageRects.clear();
        auto commitDurationUS = g_get_monotonic_time() - commitStartUS;
        priv->commitWindowTotalUS += commitDurationUS;
        priv->commitWindowMaxUS = std::max(priv->commitWindowMaxUS, commitDurationUS);
        priv->commitWindowCount++;
        if (result && drmBuffer)
            priv->lastFrameCommitUS = commitStartUS;
        if (result && priv->pendingBuffer) {
            RELEASE_ASSERT(priv->pendingTicket.advance(WPE::DRM::FramePhase::Submitted));
            priv->frameTrace.record(priv->pendingTicket.serial, priv->pendingTicket.bufferID,
                priv->committedFrameGeneration, priv->geometryEpoch, priv->chromeMotionSequence,
                WPE::DRM::FrameStage::Submitted);
        }
        priv->lastCommitWasSynchronous = result && bufferUsesSynchronousCommit(drmBuffer);
        if (!result) {
            if (abortPendingCommit(view, "atomic-commit")) {
                if (error)
                    g_clear_error(error);
                return TRUE;
            }
        }
        return result;
    }

    if (!drmBuffer) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to render buffer: no DRM buffer to commit");
        return FALSE;
    }

    auto result = wpeViewDRMCommitLegacy(WPE_VIEW_DRM(view), *drmBuffer, error);
    auto commitDurationUS = g_get_monotonic_time() - commitStartUS;
    priv->commitWindowTotalUS += commitDurationUS;
    priv->commitWindowMaxUS = std::max(priv->commitWindowMaxUS, commitDurationUS);
    priv->commitWindowCount++;
    if (result)
        drmBuffer->setScanoutOwned(true);
    if (result && priv->pendingBuffer)
        RELEASE_ASSERT(priv->pendingTicket.advance(WPE::DRM::FramePhase::Submitted));
    if (result)
        priv->lastFrameCommitUS = commitStartUS;
    else if (abortPendingCommit(view, "legacy-commit")) {
        if (error)
            g_clear_error(error);
        return TRUE;
    }
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
    ChromeRenderScope scope(priv->chromeCache);
    refreshContentGeometry(WPE_VIEW_DRM(view));
    if (priv->updateFlags.contains(UpdateFlags::CursorUpdateRequested) && !priv->committedBuffer) {
        priv->updateFlags.remove(UpdateFlags::CursorUpdateRequested);
        priv->updateFlags.remove(UpdateFlags::CursorUpdatePending);
    }
    if (priv->updateFlags.containsAny({ UpdateFlags::BufferUpdateRequested, UpdateFlags::CursorUpdateRequested })) {
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
    priv->pendingTicket.accept(++priv->nextFrameSerial,
        reinterpret_cast<uintptr_t>(buffer), priv->geometryEpoch);
    priv->frameTrace.record(priv->pendingTicket.serial, priv->pendingTicket.bufferID,
        priv->committedFrameGeneration, priv->geometryEpoch, priv->chromeMotionSequence,
        WPE::DRM::FrameStage::Received);
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
            if (view->priv->updateFlags.containsAny({ UpdateFlags::BufferUpdateRequested, UpdateFlags::CursorUpdateRequested })) {
                view->priv->updateFlags.add(UpdateFlags::CursorUpdatePending);
                return;
            }
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
    ChromeRenderScope scope(priv->chromeCache);
    priv->atomicInFlight = false;
    auto updateFlags = std::exchange(priv->updateFlags, OptionSet<UpdateFlags> { });
    priv->pageFlipCount++;
    if (updateFlags.contains(UpdateFlags::BufferUpdateRequested))
        wpeViewDRMFinishBufferCommit(view);
    videoOverlayDrainPending(view);

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
