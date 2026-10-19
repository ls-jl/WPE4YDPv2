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
#include "VideoOverlayWire.h"
#include <fcntl.h>
#include "DRMFrameQueue.h"

// ===== 视频 overlay 直出（hole-punch）=====
// WebProcess 端 GStreamerHolePunchQuirkRockchip 的 sink 通过 unix seqpacket
// socket 把解码后的 NV12 dmabuf（mppvideodec+RGA 已按面板方向预旋转）逐帧
// 发来；这里导入成 FB 放到空闲 overlay plane，zpos 压到 primary 之下，页面
// 合成器在视频区域画的透明洞（per-pixel alpha）让视频透出。线格式必须与
// Source/WebCore/platform/gstreamer/GStreamerHolePunchQuirkRockchip.cpp 中的
// 定义保持一字不差。



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
    GRefPtr<GSource> releaseSource;
    GRefPtr<GSource> retrySource;
    WPE::DRM::VideoReleaseQueue pendingReleaseAcks;
    const WPE::DRM::Plane* plane { nullptr };
    VideoOverlayFB current;
    VideoOverlayWireMessage lastFrame;
    uint64_t primaryZposRestore { 0 };
    bool haveFrame { false };
    bool planeEnabled { false };
    bool zposUnsupported { false };
    bool droppingClient { false };
    bool disableRequested { false };
    gint64 disableStartUS { 0 };
    bool closeAfterDisable { false };
    uint64_t closeSequence { 0 };
    bool havePendingFrame { false };
    bool geometryPending { false };
    VideoOverlayWireMessage pendingFrame;
    int pendingFDs[3] { -1, -1, -1 };
    unsigned pendingFDCount { 0 };
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
    uint64_t ackRetriedFrames { 0 };
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

void videoOverlayPunchActiveHole(WPEViewDRM* view, DRMScanoutBuffer* scanoutBuffer)
{
    ChromeRenderScope scope(view->priv->chromeCache);
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
    auto handles = std::array<uint32_t, 3> { fb.handles[0], fb.handles[1], fb.handles[2] };
    for (unsigned i = 0; i < 3; ++i) {
        if (!fb.handles[i])
            continue;
        // 同一 dmabuf 多平面共享一个 GEM handle，只 close 一次。
        bool alreadyClosed = false;
        for (unsigned j = 0; j < i; ++j) {
            if (handles[j] == handles[i])
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

static void videoOverlayDropClient(WPEViewDRM*);
static void videoOverlayScheduleDrain(WPEViewDRM*);

static bool videoOverlayFlushReleases(VideoOverlayState* overlay)
{
    if (!overlay || overlay->clientFD < 0)
        return false;
    while (!overlay->pendingReleaseAcks.empty()) {
        VideoOverlayWireMessage message = { };
        message.version = videoOverlayProtocolVersion;
        message.type = overlay->pendingReleaseAcks.type();
        message.sequence = overlay->pendingReleaseAcks.front();
        ssize_t result;
        do {
            result = send(overlay->clientFD, &message, sizeof(message), MSG_NOSIGNAL | MSG_DONTWAIT);
        } while (result < 0 && errno == EINTR);
        if (result == sizeof(message)) {
            overlay->ackedFrames++;
            overlay->pendingReleaseAcks.pop();
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return false;
        overlay->ackFailedFrames++;
        if (overlay->commitFailLogCount++ < 8)
            g_warning("WPEViewDRM video overlay: release ACK failed sequence=%" G_GUINT64_FORMAT ": %s",
                message.sequence, result < 0 ? strerror(errno) : "short write");
        videoOverlayDropClient(overlay->view);
        return false;
    }
    return true;
}

static gboolean videoOverlayReleaseWritable(int, GIOCondition condition, gpointer userData)
{
    auto* overlay = static_cast<VideoOverlayState*>(userData);
    if (!overlay || !overlay->view)
        return G_SOURCE_REMOVE;
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        videoOverlayDropClient(overlay->view);
        return G_SOURCE_REMOVE;
    }
    overlay->ackRetriedFrames += overlay->pendingReleaseAcks.size();
    if (!videoOverlayFlushReleases(overlay))
        return overlay->clientFD >= 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    overlay->releaseSource = nullptr;
    return G_SOURCE_REMOVE;
}

static void videoOverlaySendRelease(VideoOverlayState* overlay, uint64_t sequence,
    uint32_t type = VideoOverlayMessageRelease)
{
    if (!overlay || overlay->droppingClient || overlay->clientFD < 0 || !sequence)
        return;
    auto addResult = overlay->pendingReleaseAcks.add(sequence, type);
    if (addResult == WPE::DRM::VideoReleaseQueue::AddResult::Overflow) {
        g_warning("WPEViewDRM video overlay: release ACK backlog exceeded limit; reconnecting client");
        videoOverlayDropClient(overlay->view);
        return;
    }
    if (videoOverlayFlushReleases(overlay) || overlay->releaseSource || overlay->clientFD < 0)
        return;
    overlay->releaseSource = adoptGRef(g_unix_fd_source_new(overlay->clientFD,
        static_cast<GIOCondition>(G_IO_OUT | G_IO_ERR | G_IO_HUP | G_IO_NVAL)));
    g_source_set_name(overlay->releaseSource.get(), "WPE DRM video release ACK");
    g_source_set_callback(overlay->releaseSource.get(),
        reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(videoOverlayReleaseWritable)), overlay, nullptr);
    g_source_attach(overlay->releaseSource.get(), g_main_context_get_thread_default());
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
        " ack_failed=%" G_GUINT64_FORMAT " ack_retried=%" G_GUINT64_FORMAT " ack_pending=%zu inflight=%u commit_avg_ms=%.2f commit_max_ms=%.2f current_fb=%u sequence=%" G_GUINT64_FORMAT,
        overlay->receivedFrames, overlay->committedFrames, overlay->coalescedFrames,
        overlay->failedFrames, overlay->ackedFrames, overlay->ackFailedFrames,
        overlay->ackRetriedFrames, overlay->pendingReleaseAcks.size(),
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
    if (priv->atomicInFlight) {
        errno = EBUSY;
        return finish(false);
    }

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
            g_object_set_data(G_OBJECT(view), videoOverlayInputGeometryKey, nullptr);
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
        int commitResult = drmModeAtomicCommit(fd, request.get(), 0, nullptr);
        if (!commitResult) {
            overlay->planeEnabled = true;
            videoOverlayPublishInputGeometry(view, frame, fit, rotation);
            return finish(true);
        }
        if (errno == EBUSY) {
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

static bool videoOverlayDisable(WPEViewDRM* view)
{
    auto* overlay = view->priv->videoOverlay;
    if (!overlay || !overlay->plane)
        return true;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    if (overlay->planeEnabled) {
        if (view->priv->atomicInFlight)
            return false;
        WPE::DRM::UniquePtr<drmModeAtomicReq> request(drmModeAtomicAlloc());
        bool success = addPlaneProperties(request.get(), *overlay->plane, emptyPlaneProperties(*overlay->plane));
        const auto& primaryPlane = wpeDisplayDRMGetPrimaryPlane(display);
        if (!overlay->zposUnsupported && primaryPlane.properties().zpos.first)
            drmModeAtomicAddProperty(request.get(), primaryPlane.id(), primaryPlane.properties().zpos.first, overlay->primaryZposRestore);
        if (!success || drmModeAtomicCommit(fd, request.get(), 0, nullptr)) {
            if (overlay->commitFailLogCount++ < 8)
                g_warning("WPEViewDRM video overlay: disable failed; retaining scanout: %s", strerror(errno));
            return false;
        }
        overlay->planeEnabled = false;
    }
    uint64_t releasedSequence = overlay->current.sequence;
    videoOverlayReleaseFB(fd, overlay->current);
    videoOverlaySendRelease(overlay, releasedSequence);
    overlay->haveFrame = false;
    view->priv->chromeMotionPending = true;
    g_object_set_data(G_OBJECT(view), videoOverlayInputGeometryKey, nullptr);
    return true;
}

static bool videoOverlayHandleFrame(WPEViewDRM* view, const VideoOverlayWireMessage& frame, int* fds, unsigned fdCount)
{
    auto* overlay = view->priv->videoOverlay;
    auto* display = WPE_DISPLAY_DRM(wpe_view_get_display(WPE_VIEW(view)));
    auto fd = gbm_device_get_fd(wpe_display_drm_get_device(display));
    if (view->priv->atomicInFlight) {
        errno = EBUSY;
        return false;
    }

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
void videoOverlayRecommit(WPEViewDRM* view)
{
    ChromeRenderScope scope(view->priv->chromeCache);
    auto* overlay = view->priv->videoOverlay;
    if (!overlay || !overlay->haveFrame || !overlay->current.fbID)
        return;
    overlay->geometryPending = true;
    videoOverlayDrainPending(view);
}

static void videoOverlayClosePendingFrame(VideoOverlayState* overlay)
{
    for (unsigned i = 0; i < overlay->pendingFDCount; ++i) {
        if (overlay->pendingFDs[i] >= 0)
            close(overlay->pendingFDs[i]);
        overlay->pendingFDs[i] = -1;
    }
    overlay->pendingFDCount = 0;
    overlay->havePendingFrame = false;
}

static void videoOverlayDeferFrame(WPEViewDRM* view,
    const VideoOverlayWireMessage& frame, const int* fds, unsigned count)
{
    auto* overlay = view->priv->videoOverlay;
    if (overlay->havePendingFrame) {
        videoOverlaySendRelease(overlay, overlay->pendingFrame.sequence);
        videoOverlayClosePendingFrame(overlay);
        overlay->coalescedFrames++;
    }
    overlay->pendingFrame = frame;
    for (unsigned i = 0; i < count && i < 3; ++i) {
        overlay->pendingFDs[i] = fcntl(fds[i], F_DUPFD_CLOEXEC, 0);
        if (overlay->pendingFDs[i] < 0) {
            videoOverlayClosePendingFrame(overlay);
            videoOverlaySendRelease(overlay, frame.sequence);
            overlay->failedFrames++;
            return;
        }
        overlay->pendingFDCount++;
    }
    overlay->havePendingFrame = true;
    videoOverlayScheduleDrain(view);
}

static void videoOverlayFinishDrop(VideoOverlayState* overlay)
{
    if (overlay->clientFD >= 0)
        close(overlay->clientFD);
    overlay->clientFD = -1;
    overlay->pendingReleaseAcks.clear();
    overlay->droppingClient = false;
    overlay->closeAfterDisable = false;
}

void videoOverlayDrainPending(WPEViewDRM* view)
{
    ChromeRenderScope scope(view->priv->chromeCache);
    auto* overlay = view->priv->videoOverlay;
    if (!overlay)
        return;
    if (overlay->disableRequested) {
        if (!overlay->disableStartUS)
            overlay->disableStartUS = g_get_monotonic_time();
        if (overlay->havePendingFrame) {
            videoOverlaySendRelease(overlay, overlay->pendingFrame.sequence);
            videoOverlayClosePendingFrame(overlay);
        }
        if (!videoOverlayDisable(view)) {
            if (g_get_monotonic_time() - overlay->disableStartUS > 2 * G_USEC_PER_SEC)
                g_error("WPE video scanout could not be disabled; terminating without unsafe release");
            videoOverlayScheduleDrain(view);
            return;
        }
        overlay->disableRequested = false;
        overlay->disableStartUS = 0;
        overlay->geometryPending = false;
        if (overlay->closeSequence) {
            videoOverlaySendRelease(overlay, overlay->closeSequence, VideoOverlayMessageCloseComplete);
            overlay->closeSequence = 0;
        }
        if (overlay->closeAfterDisable)
            videoOverlayFinishDrop(overlay);
    }
    if (view->priv->atomicInFlight) {
        if (overlay->havePendingFrame || overlay->geometryPending)
            videoOverlayScheduleDrain(view);
        return;
    }
    if (overlay->havePendingFrame) {
        errno = 0;
        if (videoOverlayHandleFrame(view, overlay->pendingFrame,
                overlay->pendingFDs, overlay->pendingFDCount)) {
            overlay->committedFrames++;
            overlay->geometryPending = false;
        } else if (errno == EBUSY || errno == EAGAIN) {
            videoOverlayScheduleDrain(view);
            return;
        } else {
            overlay->failedFrames++;
            videoOverlaySendRelease(overlay, overlay->pendingFrame.sequence);
        }
        videoOverlayClosePendingFrame(overlay);
    } else if (overlay->geometryPending && overlay->haveFrame) {
        if (videoOverlayCommit(view, overlay, overlay->current.fbID, overlay->lastFrame))
            overlay->geometryPending = false;
        else
            videoOverlayScheduleDrain(view);
    }
}

static void videoOverlayScheduleDrain(WPEViewDRM* view)
{
    auto* overlay = view->priv->videoOverlay;
    if (!overlay || overlay->retrySource)
        return;
    overlay->retrySource = adoptGRef(g_timeout_source_new(16));
    g_source_set_name(overlay->retrySource.get(), "WPE deferred video commit");
    g_source_set_callback(overlay->retrySource.get(), +[](gpointer data) -> gboolean {
        auto* view = WPE_VIEW_DRM(data);
        if (view->priv->videoOverlay)
            view->priv->videoOverlay->retrySource = nullptr;
        videoOverlayDrainPending(view);
        return G_SOURCE_REMOVE;
    }, view, nullptr);
    g_source_attach(overlay->retrySource.get(), g_main_context_get_thread_default());
}

static void videoOverlayDropClient(WPEViewDRM* view)
{
    auto* overlay = view->priv->videoOverlay;
    if (!overlay || overlay->droppingClient)
        return;
    overlay->droppingClient = true;
    if (overlay->clientSource) {
        g_source_destroy(overlay->clientSource.get());
        overlay->clientSource = nullptr;
    }
    if (overlay->releaseSource) {
        g_source_destroy(overlay->releaseSource.get());
        overlay->releaseSource = nullptr;
    }
    overlay->closeAfterDisable = true;
    overlay->disableRequested = true;
    videoOverlayDrainPending(view);
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
        videoOverlayDeferFrame(view, pendingFrame, pendingFDs, pendingFDCount);
        videoOverlayDrainPending(view);
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

        if (!videoOverlayWireValid(message, received) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
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
            overlay->closeSequence = message.sequence;
            overlay->disableRequested = true;
            videoOverlayDrainPending(view);
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
    if (overlay->clientFD >= 0 || overlay->disableRequested) {
        close(clientFD);
        return G_SOURCE_CONTINUE;
    }

    overlay->clientFD = clientFD;
    overlay->clientSource = adoptGRef(g_unix_fd_source_new(clientFD, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP)));
    g_source_set_name(overlay->clientSource.get(), "WPE DRM video overlay client");
    g_source_set_callback(overlay->clientSource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(videoOverlayClientEvent)), view, nullptr);
    g_source_attach(overlay->clientSource.get(), g_main_context_get_thread_default());
    g_message("WPEViewDRM video overlay: client connected");
    return G_SOURCE_CONTINUE;
}

void videoOverlayStart(WPEViewDRM* view)
{
    ChromeRenderScope scope(view->priv->chromeCache);
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
    memcpy(address.sun_path, path, strlen(path) + 1);
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

void videoOverlayStop(WPEViewDRM* view)
{
    ChromeRenderScope scope(view->priv->chromeCache);
    auto* overlay = view->priv->videoOverlay;
    if (!overlay)
        return;
    // The main loop is leaving. Cancel deferred work and synchronously detach
    // this plane; failed detach keeps kernel references until display FD close.
    for (auto* source : { &overlay->clientSource, &overlay->releaseSource, &overlay->retrySource }) {
        if (*source) {
            g_source_destroy(source->get());
            *source = nullptr;
        }
    }
    videoOverlayClosePendingFrame(overlay);
    bool wasInFlight = std::exchange(view->priv->atomicInFlight, false);
    bool disabled = videoOverlayDisable(view);
    view->priv->atomicInFlight = wasInFlight;
    if (!disabled)
        g_warning("WPE video shutdown: preserving kernel framebuffer until DRM FD closes");
    if (overlay->clientFD >= 0)
        close(overlay->clientFD);
    overlay->clientFD = -1;
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
