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

void wpeViewDRMFinishBufferCommit(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (!priv->pendingBuffer && !priv->pendingScanoutBuffer)
        return;

    bool committedWebFrame = !!priv->pendingBuffer;
    if (committedWebFrame) {
        RELEASE_ASSERT(priv->pendingTicket.advance(WPE::DRM::FramePhase::Presented));
        priv->frameTrace.record(priv->pendingTicket.serial, priv->pendingTicket.bufferID,
            priv->committedFrameGeneration + 1, priv->geometryEpoch, priv->chromeMotionSequence,
            WPE::DRM::FrameStage::Presented);
        if (priv->committedBuffer)
            wpe_view_buffer_released(WPE_VIEW(view), priv->committedBuffer.get());
        priv->committedBuffer = WTF::move(priv->pendingBuffer);
        if (priv->pendingTicket.complete(priv->pendingTicket.serial))
            wpe_view_buffer_rendered(WPE_VIEW(view), priv->committedBuffer.get());
        priv->committedTicket = priv->pendingTicket;
        priv->pendingTicket = { };
    }
    if (priv->pendingScanoutBuffer) {
        if (priv->committedScanoutBuffer
            && priv->committedScanoutBuffer != priv->pendingScanoutBuffer)
            priv->committedScanoutBuffer->setScanoutOwned(false);
        priv->committedScanoutBuffer = priv->pendingScanoutBuffer;
        priv->pendingScanoutBuffer = nullptr;
    }
    priv->chromeMotionPending = priv->composedChromeSequence != priv->chromeCache.snapshotSequence
        || priv->composedMotionSequence != priv->chromeMotionSequence;

    if (committedWebFrame) {
        priv->committedFrameGeneration++;
        if (priv->preparedBaseGeneration) {
            // Swap the storage instead of moving it one way. The previous
            // committed base becomes the next prepared buffer, so animated
            // pages do not allocate a full-panel vector every frame.
            std::swap(priv->chromeBasePixels, priv->preparedChromeBasePixels);
            priv->chromeBaseWidth = priv->preparedChromeBaseWidth;
            priv->chromeBaseHeight = priv->preparedChromeBaseHeight;
            priv->chromeBasePitch = priv->preparedChromeBasePitch;
            priv->preparedChromeBaseWidth = 0;
            priv->preparedChromeBaseHeight = 0;
            priv->preparedChromeBasePitch = 0;
            priv->chromeBaseGeneration = priv->committedFrameGeneration;
            priv->preparedBaseGeneration = 0;
        }
    }

    // Chrome-only redraws now use the same page-flip lifecycle as WebKit frames,
    // but they must not be counted as newly rendered WebKit buffers.
    if (!committedWebFrame)
        return;
    priv->frameCount++;
    priv->statsWindowFrameCount++;

    gint64 nowUS = g_get_monotonic_time();
    auto* inputClock = static_cast<const gint64*>(g_object_get_data(G_OBJECT(view), WPE_LAST_INPUT_DATA_KEY));
    static bool benchmarkActive = []() {
        const char* value = getenv("WPE_DRM_BENCHMARK_ACTIVE");
        return value && value[0] == '1';
    }();
    priv->frameTimings.presented(nowUS, inputClock ? *inputClock : 0, benchmarkActive);
    gint64 elapsedUS = nowUS - priv->statsWindowStartUS;
    if (elapsedUS < 2 * G_USEC_PER_SEC)
        return;

    auto fps = static_cast<double>(priv->statsWindowFrameCount) * G_USEC_PER_SEC / elapsedUS;
    auto copyAverageMS = priv->copyWindowCount ? priv->copyWindowTotalUS / 1000. / priv->copyWindowCount : 0.;
    auto commitAverageMS = priv->commitWindowCount ? priv->commitWindowTotalUS / 1000. / priv->commitWindowCount : 0.;
    auto rgaAverageMS = priv->rgaRotateWindowCount
        ? priv->rgaRotateWindowTotalUS / 1000. / priv->rgaRotateWindowCount : 0.;
    auto& rga = RockchipRGA::singleton();
    g_message("WPE frame timings gap_p50_ms=%.2f gap_p95_ms=%.2f gap_p99_ms=%.2f latest_input_p50_ms=%.2f latest_input_p95_ms=%.2f latest_input_p99_ms=%.2f benchmark_active=%d",
        priv->frameTimings.percentile(nowUS, 50, false), priv->frameTimings.percentile(nowUS, 95, false),
        priv->frameTimings.percentile(nowUS, 99, false), priv->frameTimings.percentile(nowUS, 50, true),
        priv->frameTimings.percentile(nowUS, 95, true), priv->frameTimings.percentile(nowUS, 99, true), benchmarkActive);
    g_message("WPEViewDRM window fps=%.1f page_copy_ms=%.2f page_copy_max_ms=%.2f rga_ms=%.2f rga_max_ms=%.2f rga_frames=%" G_GUINT64_FORMAT " rga_cpu_fallback=%" G_GUINT64_FORMAT " rga_imports=%" G_GUINT64_FORMAT " rga_reuses=%" G_GUINT64_FORMAT " commit_avg_ms=%.2f commit_max_ms=%.2f frame_generation=%" G_GUINT64_FORMAT " chrome_base_generation=%" G_GUINT64_FORMAT " stale_redraw=%" G_GUINT64_FORMAT " retained=%" G_GUINT64_FORMAT " queued=%" G_GUINT64_FORMAT " replaced=%" G_GUINT64_FORMAT " dropped=%" G_GUINT64_FORMAT " frames=%" G_GUINT64_FORMAT " total_frames=%" G_GUINT64_FORMAT " pageflips=%" G_GUINT64_FORMAT,
        fps, copyAverageMS, priv->copyWindowMaxUS / 1000., rgaAverageMS,
        priv->rgaRotateWindowMaxUS / 1000., priv->rgaRotateWindowCount,
        priv->rgaCPUFallbackWindowCount, rga.importCount(), rga.reuseCount(),
        commitAverageMS, priv->commitWindowMaxUS / 1000.,
        priv->committedFrameGeneration, priv->chromeBaseGeneration,
        priv->staleChromeRedrawCount, priv->retainedBaseFrameCount, priv->queuedWindowCount,
        priv->replacedQueuedWindowCount, priv->droppedWindowCount,
        priv->statsWindowFrameCount, priv->frameCount, priv->pageFlipCount);
    if (priv->fenceWaitCount || priv->fenceTimeoutCount)
        g_message("WPEViewDRM fence waits=%" G_GUINT64_FORMAT " failed=%" G_GUINT64_FORMAT
            " avg_ms=%.2f max_ms=%.2f main_loop_blocking=0",
            priv->fenceWaitCount, priv->fenceTimeoutCount,
            priv->fenceWaitCount ? priv->fenceWaitTotalUS / 1000. / priv->fenceWaitCount : 0.,
            priv->fenceWaitMaxUS / 1000.);

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
    priv->rgaRotateWindowTotalUS = 0;
    priv->rgaRotateWindowMaxUS = 0;
    priv->rgaRotateWindowCount = 0;
    priv->rgaCPUFallbackWindowCount = 0;
    priv->retainedBaseFrameCount = 0;
    priv->staleChromeRedrawCount = 0;
}

struct DiscardedGPUFrame {
    WPEViewDRM* view;
    GRefPtr<WPEBuffer> buffer;
};

void retireDiscardedBuffer(WPEViewDRM* view, WPEBuffer* buffer,
    UnixFileDescriptor&& fence)
{
    if (!buffer)
        return;
    if (!fence)
        fence = UnixFileDescriptor { wpe_buffer_take_rendering_fence(buffer), UnixFileDescriptor::Adopt };
    if (WPE::DRM::probeRenderingFence(fence.value()) == WPE::DRM::FenceResult::Ready) {
        wpe_view_buffer_released(WPE_VIEW(view), buffer);
        return;
    }
    auto* retained = new DiscardedGPUFrame { view, buffer };
    int fd = fence.release();
    if (!view->priv->discardedRenderingFences.retain(fd, retained,
            +[](void* data) {
                auto* retained = static_cast<DiscardedGPUFrame*>(data);
                wpe_view_buffer_released(WPE_VIEW(retained->view), retained->buffer.get());
            }, +[](void* data) { delete static_cast<DiscardedGPUFrame*>(data); },
            g_main_context_get_thread_default())) {
        // The producer has a four-buffer swapchain. More unfinished sources
        // indicate a broken channel, not permission to recycle a GPU target.
        delete retained;
        close(fd);
        g_error("WPE GPU frame quarantine exhausted; terminating unsafe session");
    }
    g_warning("WPEViewDRM source quarantined: buffer=%p retained=%zu",
        buffer, view->priv->discardedRenderingFences.size());
}

void wpeViewDRMQueueBuffer(WPEViewDRM* view, WPEBuffer* buffer, const WPERectangle* damageRects, guint nDamageRects)
{
    auto* priv = view->priv;
    bool replacesQueuedBuffer = priv->queuedBuffer && priv->queuedBuffer.get() != buffer;
    priv->queuedWindowCount++;
    if (replacesQueuedBuffer)
        priv->replacedQueuedWindowCount++;
    if (replacesQueuedBuffer)
        retireDiscardedBuffer(view, priv->queuedBuffer.get(), { });
    priv->queuedBuffer = buffer;
    if (!priv->queuedTicket.serial || replacesQueuedBuffer)
        priv->queuedTicket.accept(++priv->nextFrameSerial,
            reinterpret_cast<uintptr_t>(buffer), priv->geometryEpoch);
    priv->frameTrace.record(priv->queuedTicket.serial, priv->queuedTicket.bufferID,
        priv->committedFrameGeneration, priv->geometryEpoch, priv->chromeMotionSequence,
        WPE::DRM::FrameStage::Queued);
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

bool wpeViewDRMThrottleDelay(WPEViewDRM* view, gint64& delayUS)
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

gboolean wpeViewDRMCommitQueuedBuffer(WPEViewDRM* view, GError** error)
{
    auto* priv = view->priv;
    if (!priv->queuedBuffer)
        return TRUE;
    if (priv->updateFlags.containsAny({ UpdateFlags::BufferUpdateRequested, UpdateFlags::CursorUpdateRequested }))
        return TRUE;

    priv->updateFlags.remove(UpdateFlags::BufferUpdatePending);
    priv->pendingBuffer = WTF::move(priv->queuedBuffer);
    priv->pendingTicket = std::exchange(priv->queuedTicket, WPE::DRM::FrameTicket { });
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

void wpeViewDRMScheduleQueuedCommit(WPEViewDRM* view, gint64 delayUS)
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

void wpeViewDRMCompleteSynchronousCommitIfNeeded(WPEViewDRM* view)
{
    auto* priv = view->priv;
    if (!std::exchange(priv->lastCommitWasSynchronous, false))
        return;

    priv->updateFlags.remove(UpdateFlags::BufferUpdateRequested);
    wpeViewDRMFinishBufferCommit(view);
}
