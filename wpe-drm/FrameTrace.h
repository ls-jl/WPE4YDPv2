/* SPDX-License-Identifier: MIT */
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <glib.h>

namespace WPE { namespace DRM {

enum class FrameStage : uint8_t { Received, Queued, FenceReady, Prepared, Submitted, Presented, Released, Discarded };
struct FrameTracePoint {
    uint64_t serial;
    uintptr_t buffer;
    uint64_t pageGeneration;
    uint64_t geometryEpoch;
    uint32_t chromeSequence;
    FrameStage stage;
    gint64 timestampUS;
};

class FrameTrace {
public:
    void record(uint64_t serial, uintptr_t buffer, uint64_t generation, uint64_t epoch, uint32_t chrome, FrameStage stage)
    {
        if (!enabled())
            return;
        m_points[m_sequence++ % capacity] = { serial, buffer, generation, epoch, chrome, stage, g_get_monotonic_time() };
    }
    void dump() const
    {
        auto begin = m_sequence > capacity ? m_sequence - capacity : 0;
        for (auto index = begin; index < m_sequence; ++index) {
            const auto& point = m_points[index % capacity];
            g_message("WPE frame trace serial=%" G_GUINT64_FORMAT " buffer=%" G_GUINTPTR_FORMAT
                " page=%" G_GUINT64_FORMAT " geometry=%" G_GUINT64_FORMAT
                " chrome=%u stage=%u time_us=%" G_GINT64_FORMAT,
                point.serial, point.buffer, point.pageGeneration, point.geometryEpoch,
                point.chromeSequence, static_cast<unsigned>(point.stage), point.timestampUS);
        }
    }
    static bool enabled()
    {
        static bool enabled = [] { const char* value = getenv("WPE_DRM_FRAME_TRACE"); return value && value[0] == '1'; }();
        return enabled;
    }
private:
    static constexpr size_t capacity = 256;
    std::array<FrameTracePoint, capacity> m_points { };
    uint64_t m_sequence { 0 };
};

} }
