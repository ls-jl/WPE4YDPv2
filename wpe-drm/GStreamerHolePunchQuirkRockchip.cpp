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
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
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
};

struct VideoOverlayWireMessage {
    uint32_t type;
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

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e // 'NV12' little-endian，避免引 drm_fourcc.h
#endif

// UI 进程 NONBLOCK commit 后旧帧可能还有一个 vblank 在屏上，多押两级；
// 解码器缓冲池通常 16+ 个 buffer，扣 4 个不会饿到解码。
static constexpr size_t retainedSampleCount = 4;
static constexpr gint64 staleOwnerTimeoutUS = 750 * 1000;

struct RockchipVideoOverlaySinkContext {
    Lock lock;
    IntRect rect;
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
        m_retainedSamples.clear();
        closeLocked();
        m_owner = nullptr;
        m_lastFrameUS = 0;
        g_message("Rockchip video overlay: owner released");
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
        auto now = g_get_monotonic_time();
        if (m_owner != owner) {
            if (m_owner && now - m_lastFrameUS <= staleOwnerTimeoutUS)
                return;
            if (m_owner) {
                sendSimpleMessageLocked(VideoOverlayMessageHide);
                m_retainedSamples.clear();
                g_message("Rockchip video overlay: stale owner replaced after %.1f ms", (now - m_lastFrameUS) / 1000.0);
            } else
                g_message("Rockchip video overlay: owner acquired on first NV12 DMA-BUF frame");
            m_owner = owner;
            m_rect = rect;
        } else if (m_rect != rect)
            m_rect = rect;
        m_lastFrameUS = now;

        if (!ensureConnectedLocked())
            return;

        VideoOverlayWireMessage message = { };
        message.type = VideoOverlayMessageFrame;
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

        // 押住样本，防止解码器在 VOP 还在扫描时复写这块 dmabuf。
        if (m_retainedSamples.size() >= retainedSampleCount)
            m_retainedSamples.removeAt(0);
        m_retainedSamples.append(GRefPtr<GstSample>(sample));
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

    void sendSimpleMessageLocked(uint32_t type)
    {
        VideoOverlayWireMessage message = { };
        message.type = type;
        sendLocked(message, -1);
    }

    void closeLocked()
    {
        if (m_socket >= 0) {
            close(m_socket);
            m_socket = -1;
        }
    }

    Lock m_lock;
    int m_socket { -1 };
    RockchipVideoOverlaySinkContext* m_owner { nullptr };
    bool m_warned { false };
    gint64 m_lastConnectAttemptUS { 0 };
    gint64 m_lastFrameUS { 0 };
    IntRect m_rect;
    Vector<GRefPtr<GstSample>> m_retainedSamples;
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
