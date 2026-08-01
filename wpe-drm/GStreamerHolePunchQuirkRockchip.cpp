/*
 * Copyright (C) 2026 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "GStreamerHolePunchQuirkRockchip.h"

#if USE(GSTREAMER)

#include "GStreamerCommon.h"
#include "IntRect.h"
#include <algorithm>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/GUniquePtr.h>

namespace WebCore {

// 线格式必须与 Source/WebKit/WPEPlatform/wpe/drm/WPEViewDRM.cpp 中的定义
// 保持一字不差。
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

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e // 'NV12' little-endian，避免引 drm_fourcc.h
#endif

static constexpr size_t maximumInFlightSamples = 4;

struct VideoOverlayInFlightSample {
    uint64_t sequence { 0 };
    GRefPtr<GstSample> sample;
    gint64 sentUS { 0 };
};

struct RockchipVideoOverlaySinkContext {
    Lock lock;
    IntRect rect;
    bool loggedOwnerBusy { false };
};

class RockchipVideoOverlayChannel {
public:
    static RockchipVideoOverlayChannel& singleton()
    {
        static RockchipVideoOverlayChannel channel;
        return channel;
    }

    void release(RockchipVideoOverlaySinkContext* owner)
    {
        Locker locker { m_lock };
        if (m_owner != owner)
            return;
        sendSimpleMessageLocked(VideoOverlayMessageHide);
        waitForReleaseAcksLocked(100);
        m_inFlightSamples.clear();
        closeLocked();
        m_owner = nullptr;
        g_message("Rockchip video overlay: owner=%p released", owner);
    }

    void setRectangle(RockchipVideoOverlaySinkContext* owner, const IntRect& rect)
    {
        Locker locker { m_lock };
        if (m_owner != owner)
            return;
        if (m_rect == rect)
            return;
        m_rect = rect;
        if (m_socket < 0)
            return;
        VideoOverlayWireMessage message = { };
        message.version = videoOverlayProtocolVersion;
        message.type = VideoOverlayMessageRect;
        fillRectLocked(message);
        sendLocked(message, -1);
    }

    void sendFrame(RockchipVideoOverlaySinkContext* owner, const IntRect& rect, GstSample* sample)
    {
        auto* buffer = gst_sample_get_buffer(sample);
        auto* caps = gst_sample_get_caps(sample);
        if (!buffer || !caps)
            return;

        if (gst_buffer_n_memory(buffer) != 1) {
            Locker locker { m_lock };
            warnOnceLocked("multi-memory buffers not supported");
            return;
        }
        auto* memory = gst_buffer_peek_memory(buffer, 0);
        if (!gst_is_dmabuf_memory(memory)) {
            Locker locker { m_lock };
            warnOnceLocked("non-dmabuf frame, video overlay inactive");
            return;
        }

        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12)
            return;

        Locker locker { m_lock };
        if (m_owner != owner) {
            if (m_owner) {
                if (!owner->loggedOwnerBusy) {
                    owner->loggedOwnerBusy = true;
                    g_message("Rockchip video overlay: contender=%p blocked by owner=%p", owner, m_owner);
                }
                return;
            }
            g_message("Rockchip video overlay: owner=%p acquired frame=%ux%u rect=%dx%d+%d+%d",
                owner, GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                rect.width(), rect.height(), rect.x(), rect.y());
            m_owner = owner;
            m_rect = rect;
        } else if (m_rect != rect)
            m_rect = rect;

        if (!ensureConnectedLocked())
            return;
        drainReleaseAcksLocked();
        if (m_inFlightSamples.size() >= maximumInFlightSamples) {
            m_droppedInFlight++;
            logStatsLocked();
            return;
        }

        VideoOverlayWireMessage message = { };
        message.version = videoOverlayProtocolVersion;
        message.type = VideoOverlayMessageFrame;
        message.sequence = m_nextSequence++;
        message.fourcc = DRM_FORMAT_NV12;
        message.width = GST_VIDEO_INFO_WIDTH(&info);
        message.height = GST_VIDEO_INFO_HEIGHT(&info);
        message.planeCount = GST_VIDEO_INFO_N_PLANES(&info);
        auto* videoMeta = gst_buffer_get_video_meta(buffer);
        for (uint32_t i = 0; i < message.planeCount && i < 3; ++i) {
            message.strides[i] = videoMeta ? videoMeta->stride[i] : GST_VIDEO_INFO_PLANE_STRIDE(&info, i);
            message.offsets[i] = videoMeta ? videoMeta->offset[i] : GST_VIDEO_INFO_PLANE_OFFSET(&info, i);
        }
        fillRectLocked(message);

        if (!sendLocked(message, gst_dmabuf_memory_get_fd(memory)))
            return;

        VideoOverlayInFlightSample retained;
        retained.sequence = message.sequence;
        retained.sample = GRefPtr<GstSample>(sample);
        retained.sentUS = g_get_monotonic_time();
        m_inFlightSamples.append(WTF::move(retained));
        m_sentFrames++;
        logStatsLocked();
    }

private:
    void fillRectLocked(VideoOverlayWireMessage& message)
    {
        message.rectX = m_rect.x();
        message.rectY = m_rect.y();
        message.rectWidth = m_rect.width();
        message.rectHeight = m_rect.height();
    }

    void warnOnceLocked(const char* what)
    {
        if (m_warned)
            return;
        m_warned = true;
        g_warning("Rockchip video overlay: %s", what);
    }

    bool ensureConnectedLocked()
    {
        if (m_socket >= 0)
            return true;
        // 连接失败退避 5 秒，避免逐帧重连刷日志。
        gint64 now = g_get_monotonic_time();
        if (m_lastConnectAttemptUS && now - m_lastConnectAttemptUS < 5 * G_USEC_PER_SEC)
            return false;
        m_lastConnectAttemptUS = now;

        const char* path = g_getenv("WPE_VIDEO_OVERLAY_SOCKET");
        GUniquePtr<char> fallbackPath;
        if (!path || !*path) {
            const char* varDir = g_getenv("WPE_VAR_DIR");
            if (varDir && *varDir)
                fallbackPath.reset(g_strdup_printf("%s/wpe-video-overlay.sock", varDir));
            else
                fallbackPath.reset(g_strdup_printf("/tmp/wpe-video-overlay-%u.sock", static_cast<unsigned>(getuid())));
            path = fallbackPath.get();
        }

        struct sockaddr_un address = { };
        if (strlen(path) >= sizeof(address.sun_path))
            return false;
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return false;
        address.sun_family = AF_UNIX;
        strcpy(address.sun_path, path);
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address))) {
            g_warning("Rockchip video overlay: connect(%s) failed: %s", path, g_strerror(errno));
            close(fd);
            return false;
        }
        m_socket = fd;
        g_message("Rockchip video overlay: connected to %s", path);
        return true;
    }

    bool sendLocked(const VideoOverlayWireMessage& message, int dmabufFD)
    {
        if (m_socket < 0)
            return false;

        struct iovec vec = { const_cast<VideoOverlayWireMessage*>(&message), sizeof(message) };
        char control[CMSG_SPACE(sizeof(int))];
        struct msghdr msg = { };
        msg.msg_iov = &vec;
        msg.msg_iovlen = 1;
        if (dmabufFD >= 0) {
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &dmabufFD, sizeof(int));
        }
        if (sendmsg(m_socket, &msg, MSG_NOSIGNAL) < 0) {
            g_warning("Rockchip video overlay: sendmsg failed: %s", g_strerror(errno));
            closeLocked();
            return false;
        }
        return true;
    }

    void drainReleaseAcksLocked()
    {
        if (m_socket < 0)
            return;
        while (true) {
            VideoOverlayWireMessage message = { };
            ssize_t received = recv(m_socket, &message, sizeof(message), MSG_DONTWAIT);
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            if (received != sizeof(message)) {
                if (!received)
                    closeLocked();
                return;
            }
            if (message.version != videoOverlayProtocolVersion
                || message.type != VideoOverlayMessageRelease || !message.sequence)
                continue;
            for (size_t index = 0; index < m_inFlightSamples.size(); ++index) {
                if (m_inFlightSamples[index].sequence != message.sequence)
                    continue;
                gint64 latencyUS = g_get_monotonic_time() - m_inFlightSamples[index].sentUS;
                m_ackLatencyTotalUS += std::max<gint64>(0, latencyUS);
                m_ackLatencyMaxUS = std::max(m_ackLatencyMaxUS, latencyUS);
                m_inFlightSamples.removeAt(index);
                m_ackedFrames++;
                break;
            }
        }
    }

    void waitForReleaseAcksLocked(int timeoutMS)
    {
        if (m_socket < 0 || m_inFlightSamples.isEmpty())
            return;
        gint64 deadlineUS = g_get_monotonic_time() + timeoutMS * 1000;
        while (!m_inFlightSamples.isEmpty() && g_get_monotonic_time() < deadlineUS) {
            struct pollfd descriptor = { m_socket, POLLIN, 0 };
            int remainingMS = std::max(1, static_cast<int>((deadlineUS - g_get_monotonic_time()) / 1000));
            if (poll(&descriptor, 1, remainingMS) <= 0)
                break;
            drainReleaseAcksLocked();
        }
    }

    void logStatsLocked()
    {
        if (m_sentFrames < m_nextStatsFrame)
            return;
        double averageACKMS = m_ackedFrames ? m_ackLatencyTotalUS / 1000. / m_ackedFrames : 0.;
        g_message("Rockchip video overlay v2: sent=%" G_GUINT64_FORMAT
            " acked=%" G_GUINT64_FORMAT " inflight=%zu dropped=%" G_GUINT64_FORMAT
            " ack_avg_ms=%.2f ack_max_ms=%.2f",
            m_sentFrames, m_ackedFrames, m_inFlightSamples.size(), m_droppedInFlight,
            averageACKMS, m_ackLatencyMaxUS / 1000.);
        m_nextStatsFrame += 120;
    }

    void sendSimpleMessageLocked(uint32_t type)
    {
        VideoOverlayWireMessage message = { };
        message.version = videoOverlayProtocolVersion;
        message.type = type;
        sendLocked(message, -1);
    }

    void closeLocked()
    {
        if (m_socket >= 0) {
            close(m_socket);
            m_socket = -1;
        }
        // No release ACK can arrive after disconnect. Clearing retained
        // samples also gives a later connection a fresh in-flight window.
        m_inFlightSamples.clear();
    }

    Lock m_lock;
    int m_socket { -1 };
    RockchipVideoOverlaySinkContext* m_owner { nullptr };
    bool m_warned { false };
    gint64 m_lastConnectAttemptUS { 0 };
    IntRect m_rect;
    Vector<VideoOverlayInFlightSample> m_inFlightSamples;
    uint64_t m_nextSequence { 1 };
    uint64_t m_sentFrames { 0 };
    uint64_t m_ackedFrames { 0 };
    uint64_t m_droppedInFlight { 0 };
    uint64_t m_nextStatsFrame { 120 };
    gint64 m_ackLatencyTotalUS { 0 };
    gint64 m_ackLatencyMaxUS { 0 };
};

static IntRect rockchipOverlayRectangle(RockchipVideoOverlaySinkContext* context)
{
    Locker locker { context->lock };
    return context->rect;
}

static GstFlowReturn rockchipOverlayNewPreroll(GstAppSink* appsink, gpointer userData)
{
    auto sample = adoptGRef(gst_app_sink_pull_preroll(appsink));
    if (sample)
        RockchipVideoOverlayChannel::singleton().sendFrame(
            static_cast<RockchipVideoOverlaySinkContext*>(userData),
            rockchipOverlayRectangle(static_cast<RockchipVideoOverlaySinkContext*>(userData)),
            sample.get());
    return GST_FLOW_OK;
}

static GstFlowReturn rockchipOverlayNewSample(GstAppSink* appsink, gpointer userData)
{
    auto sample = adoptGRef(gst_app_sink_pull_sample(appsink));
    if (sample)
        RockchipVideoOverlayChannel::singleton().sendFrame(
            static_cast<RockchipVideoOverlaySinkContext*>(userData),
            rockchipOverlayRectangle(static_cast<RockchipVideoOverlaySinkContext*>(userData)),
            sample.get());
    return GST_FLOW_OK;
}

GstElement* GStreamerHolePunchQuirkRockchip::createHolePunchVideoSink(bool, const MediaPlayer*)
{
    auto* sink = makeGStreamerElement("appsink"_s);
    if (!sink)
        return nullptr;

    auto* context = new RockchipVideoOverlaySinkContext;

    // 不声明 memory:DMABuf caps feature：rockchip mppvideodec 不一定在 caps
    // 上标注 feature，但底层分配的就是 dmabuf，运行期用 gst_is_dmabuf_memory 判断。
    auto caps = adoptGRef(gst_caps_from_string("video/x-raw(memory:DMABuf), format=(string)NV12; video/x-raw, format=(string)NV12"));
    g_object_set(sink, "caps", caps.get(), "sync", TRUE, "max-buffers", 3u, "drop", TRUE, "enable-last-sample", FALSE, nullptr);

    static GstAppSinkCallbacks callbacks = {
        nullptr, // eos：保留最后一帧在屏上，与软件路径行为一致
        rockchipOverlayNewPreroll,
        rockchipOverlayNewSample,
#if GST_CHECK_VERSION(1, 20, 0)
        nullptr, // new_event
#endif
#if GST_CHECK_VERSION(1, 24, 0)
        nullptr, // propose_allocation
#endif
        { nullptr }
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, context, [](gpointer userData) {
        auto* context = static_cast<RockchipVideoOverlaySinkContext*>(userData);
        RockchipVideoOverlayChannel::singleton().release(context);
        delete context;
    });
    g_object_set_data(G_OBJECT(sink), "wpe-rockchip-overlay-context", context);

    // 面板旋转角：MediaPlayerPrivateGStreamer::configureElement 检测到本管线
    // 挂了 hole-punch sink 时，把它设给 mppvideodec 的 rotation（RGA 预旋转）。
    const char* rotation = g_getenv("WPE_VIDEO_OVERLAY_ROTATION");
    if (!rotation || !*rotation)
        rotation = g_getenv("WPE_DRM_ROTATION");
    if (rotation && *rotation && g_strcmp0(rotation, "0"))
        g_object_set_data_full(G_OBJECT(sink), "wpe-holepunch-rotation", g_strdup(rotation), g_free);

    g_message("Rockchip hole-punch sink created; overlay ownership deferred until first frame");
    return sink;
}

bool GStreamerHolePunchQuirkRockchip::setHolePunchVideoRectangle(GstElement* sink, const IntRect& rect)
{
    auto* context = static_cast<RockchipVideoOverlaySinkContext*>(g_object_get_data(G_OBJECT(sink), "wpe-rockchip-overlay-context"));
    if (!context)
        return false;
    {
        Locker locker { context->lock };
        context->rect = rect;
    }
    RockchipVideoOverlayChannel::singleton().setRectangle(context, rect);
    return true;
}

} // namespace WebCore

#endif // USE(GSTREAMER)
