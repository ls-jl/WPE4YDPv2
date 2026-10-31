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

#include "DRMScanoutBuffer.h"
#include "ChromeRenderer.h"
#include "RenderingFenceRetirement.h"
#include "DRMFrameLifecycle.h"
#include "FrameTrace.h"
#include "RollingFrameTimings.h"

enum class UpdateFlags : uint8_t {
    BufferUpdateRequested = 1 << 0,
    CursorUpdateRequested = 1 << 1,
    BufferUpdatePending = 1 << 2,
    CursorUpdatePending = 1 << 3
};

struct _WPEViewDRMPrivate {
    Seconds refreshDuration;
    std::optional<uint32_t> modeBlob;
    GRefPtr<WPEBuffer> pendingBuffer;
    GRefPtr<WPEBuffer> committedBuffer;
    GRefPtr<WPEBuffer> queuedBuffer;
    ChromeStateCache chromeCache;
    WPE::DRM::FrameTicket pendingTicket;
    WPE::DRM::FrameTicket committedTicket;
    WPE::DRM::FrameTicket queuedTicket;
    WPE::DRM::FrameTrace frameTrace;
    WPE::DRM::RollingFrameTimings frameTimings;
    uint64_t nextFrameSerial { 0 };
    uint64_t geometryEpoch { 1 };
    int geometryViewWidth { -1 };
    int geometryViewHeight { -1 };
    uint32_t geometryInset { UINT32_MAX };
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
    WPE::DRM::RenderingFenceWait renderingFenceWait;
    WPE::DRM::RenderingFenceRetirement discardedRenderingFences;
    UnixFileDescriptor pendingRenderingFence;
    bool pendingFenceTaken { false };
    gint64 pendingFenceStartUS { 0 };
    OptionSet<UpdateFlags> updateFlags;
    std::unique_ptr<RunLoop::Timer> cursorUpdateTimer;
    guint64 frameCount { 0 };
    guint64 pageFlipCount { 0 };
    bool lastCommitWasSynchronous { false };
    bool atomicInFlight { false };
    gint64 fenceWaitTotalUS { 0 };
    gint64 fenceWaitMaxUS { 0 };
    guint64 fenceWaitCount { 0 };
    guint64 fenceTimeoutCount { 0 };
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
    uint32_t chromeMotionSequence { 0 };
    guint64 chromeMotionUpdates { 0 };
    guint64 chromeMotionCoalesced { 0 };
    guint64 chromeOverlayCommits { 0 };
    Vector<uint8_t> chromeBasePixels;
    uint32_t chromeBaseWidth { 0 };
    uint32_t chromeBaseHeight { 0 };
    uint32_t chromeBasePitch { 0 };
    Vector<uint8_t> preparedChromeBasePixels;
    uint32_t preparedChromeBaseWidth { 0 };
    uint32_t preparedChromeBaseHeight { 0 };
    uint32_t preparedChromeBasePitch { 0 };
    guint64 preparedBaseGeneration { 0 };
    guint64 committedFrameGeneration { 0 };
    guint64 chromeBaseGeneration { 0 };
    guint64 staleChromeRedrawCount { 0 };
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
    bool chromeSnapshotChanged { false };
    guint64 composedChromeSequence { 0 };
    uint32_t composedMotionSequence { 0 };
    bool captureBaseRequested { false };
    bool lastUpdateDroppedBuffer { false };
    bool forceFullDamageNextFrame { false };
    OutputRotation outputRotation { OutputRotation::Rotate0 };
    struct VideoOverlayState* videoOverlay { nullptr };
};

typedef struct _WPEViewDRMPrivate WPEViewDRMPrivate;
struct _WPEViewDRM {
    WPEView parent;
    WPEViewDRMPrivate* priv;
};

bool addPlaneProperties(drmModeAtomicReq*, const WPE::DRM::Plane&, WPE::DRM::Plane::Properties&&);
WPE::DRM::Plane::Properties emptyPlaneProperties(const WPE::DRM::Plane&);
bool shouldUsePanelNativeFit();
std::optional<uint32_t> configuredRotatedXOffset(uint32_t);
void videoOverlayStart(WPEViewDRM*);
void videoOverlayStop(WPEViewDRM*);
void videoOverlayRecommit(WPEViewDRM*);
void videoOverlayPunchActiveHole(WPEViewDRM*, DRMScanoutBuffer*);
void videoOverlayDrainPending(WPEViewDRM*);

gboolean wpeViewDRMRequestUpdate(WPEViewDRM*, GError**);
void wpeViewDRMFinishBufferCommit(WPEViewDRM*);
void retireDiscardedBuffer(WPEViewDRM*, WPEBuffer*, UnixFileDescriptor&&);
void wpeViewDRMQueueBuffer(WPEViewDRM*, WPEBuffer*, const WPERectangle*, guint);
bool wpeViewDRMThrottleDelay(WPEViewDRM*, gint64&);
gboolean wpeViewDRMCommitQueuedBuffer(WPEViewDRM*, GError**);
void wpeViewDRMScheduleQueuedCommit(WPEViewDRM*, gint64);
void wpeViewDRMCompleteSynchronousCommitIfNeeded(WPEViewDRM*);
void setDamageRects(Vector<drm_mode_rect>&, const WPERectangle*, guint);
