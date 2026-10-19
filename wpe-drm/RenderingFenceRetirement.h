/* SPDX-License-Identifier: MIT */
#pragma once

#include "RenderingFenceWait.h"
#include <algorithm>
#include <memory>
#include <vector>
#include <unistd.h>

namespace WPE { namespace DRM {

// A discarded frame can finish pacing before its GPU source can be recycled.
// The owner outlives this collection; teardown cancels callbacks without
// telling the producer to reuse unsynchronized buffers.
class RenderingFenceRetirement {
public:
    using Callback = void (*)(void*);
    static constexpr size_t maximumRetained = 4;
    ~RenderingFenceRetirement() { clear(); }

    bool retain(int fd, void* payload, Callback ready, Callback destroy, GMainContext* context)
    {
        reap();
        if (m_entries.size() >= maximumRetained)
            return false;
        auto entry = std::unique_ptr<Entry>(new Entry);
        entry->fd = fd;
        entry->payload = payload;
        entry->ready = ready;
        entry->destroy = destroy;
        auto result = probeRenderingFence(fd);
        if (result == FenceResult::Ready) {
            entry->done = true;
            ready(payload);
            return true;
        }
        if (result == FenceResult::Pending) {
            entry->source = g_unix_fd_source_new(fd,
                static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL));
            g_source_set_name(entry->source, "WPE discarded rendering fence");
            g_source_set_callback(entry->source, G_SOURCE_FUNC(dispatch), entry.get(), nullptr);
            g_source_attach(entry->source, context);
        }
        // Failed fences remain quarantined. Error readiness must not spin.
        m_entries.push_back(std::move(entry));
        return true;
    }

    size_t size() { reap(); return m_entries.size(); }
    void clear() { m_entries.clear(); }

    // Texture-pool references may only be dropped after their GL reads finish.
    // Used at compositor teardown, not in the UI/input loop.
    bool finish(unsigned timeoutMS)
    {
        gint64 deadline = g_get_monotonic_time() + static_cast<gint64>(timeoutMS) * 1000;
        for (auto& entry : m_entries) {
            if (entry->done)
                continue;
            auto result = probeRenderingFence(entry->fd);
            while (result == FenceResult::Pending) {
                gint64 remaining = deadline - g_get_monotonic_time();
                if (remaining <= 0)
                    return false;
                pollfd descriptor { entry->fd, POLLIN, 0 };
                int status = poll(&descriptor, 1, static_cast<int>((remaining + 999) / 1000));
                if (status < 0 && errno != EINTR)
                    return false;
                result = probeRenderingFence(entry->fd);
            }
            if (result != FenceResult::Ready)
                return false;
            entry->cancelSource();
            entry->done = true;
            entry->closeFD();
            entry->ready(entry->payload);
        }
        reap();
        return true;
    }

private:
    struct Entry {
        int fd { -1 };
        GSource* source { nullptr };
        void* payload { nullptr };
        Callback ready { nullptr };
        Callback destroy { nullptr };
        bool done { false };
        void cancelSource()
        {
            if (!source)
                return;
            g_source_destroy(source);
            g_source_unref(source);
            source = nullptr;
        }
        void closeFD()
        {
            if (fd >= 0)
                close(fd);
            fd = -1;
        }
        ~Entry()
        {
            cancelSource();
            closeFD();
            if (destroy)
                destroy(payload);
        }
    };

    static gboolean dispatch(int fd, GIOCondition, gpointer data)
    {
        auto* entry = static_cast<Entry*>(data);
        auto result = probeRenderingFence(fd);
        if (result == FenceResult::Pending)
            return G_SOURCE_CONTINUE;
        entry->cancelSource();
        if (result == FenceResult::Ready) {
            entry->done = true;
            entry->closeFD();
            auto callback = entry->ready;
            auto payload = entry->payload;
            callback(payload);
        }
        return G_SOURCE_REMOVE;
    }

    void reap()
    {
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
            [](const std::unique_ptr<Entry>& entry) { return entry->done; }), m_entries.end());
    }
    std::vector<std::unique_ptr<Entry>> m_entries;
};

} }
