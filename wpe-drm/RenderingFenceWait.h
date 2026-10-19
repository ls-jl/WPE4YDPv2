/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib-unix.h>
#include <poll.h>
#include <cerrno>

namespace WPE { namespace DRM {

enum class FenceResult { Ready, Pending, Failed, TimedOut };

inline FenceResult probeRenderingFence(int fd)
{
    if (fd < 0)
        return FenceResult::Ready;
    pollfd descriptor { fd, POLLIN, 0 };
    int result = poll(&descriptor, 1, 0);
    if (result < 0)
        return errno == EINTR ? FenceResult::Pending : FenceResult::Failed;
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        return FenceResult::Failed;
    return result > 0 && (descriptor.revents & POLLIN)
        ? FenceResult::Ready : FenceResult::Pending;
}

// The caller retains the FD and cancels before releasing it. The same source
// watches readiness and an absolute deadline, without blocking the UI loop.
class RenderingFenceWait {
public:
    using Callback = void (*)(FenceResult, gpointer);
    ~RenderingFenceWait() { cancel(); }
    RenderingFenceWait() = default;
    RenderingFenceWait(const RenderingFenceWait&) = delete;
    RenderingFenceWait& operator=(const RenderingFenceWait&) = delete;

    bool active() const { return m_source; }
    void cancel()
    {
        if (!m_source)
            return;
        g_source_destroy(m_source);
        g_source_unref(m_source);
        m_source = nullptr;
    }

    void start(int fd, int timeoutMS, Callback callback, gpointer data, GMainContext* context)
    {
        cancel();
        m_callback = callback;
        m_data = data;
        m_deadline = g_get_monotonic_time() + static_cast<gint64>(timeoutMS) * 1000;
        m_source = g_unix_fd_source_new(fd, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL));
        g_source_set_name(m_source, "WPE rendering fence");
        g_source_set_ready_time(m_source, m_deadline);
        g_source_set_callback(m_source, G_SOURCE_FUNC(dispatch), this, nullptr);
        g_source_attach(m_source, context);
    }

private:
    static gboolean dispatch(gint fd, GIOCondition, gpointer data)
    {
        auto* self = static_cast<RenderingFenceWait*>(data);
        auto result = probeRenderingFence(fd);
        if (result == FenceResult::Pending) {
            if (g_get_monotonic_time() < self->m_deadline)
                return G_SOURCE_CONTINUE;
            result = FenceResult::TimedOut;
        }
        auto callback = self->m_callback;
        auto client = self->m_data;
        self->cancel();
        callback(result, client);
        return G_SOURCE_REMOVE;
    }

    GSource* m_source { nullptr };
    gint64 m_deadline { 0 };
    Callback m_callback { nullptr };
    gpointer m_data { nullptr };
};

} } // namespace WPE::DRM
