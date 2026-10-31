/* SPDX-License-Identifier: MIT */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace WPE { namespace DRM {
class RollingFrameTimings {
public:
    void presented(int64_t now, int64_t latestInput, bool benchmarkActive)
    {
        int64_t gap = m_lastPresented ? now - m_lastPresented : 0;
        bool activeGap = gap > 0 && (benchmarkActive || gap <= 250000);
        int64_t inputDelay = latestInput > m_lastMeasuredInput && latestInput <= now
            && now - latestInput <= 2000000 ? now - latestInput : 0;
        if (inputDelay)
            m_lastMeasuredInput = latestInput;
        m_samples[m_sequence++ % capacity] = { now, activeGap ? gap : 0, inputDelay };
        m_lastPresented = now;
    }
    double percentile(int64_t now, unsigned percent, bool input) const
    {
        std::array<int64_t, capacity> values { };
        size_t count = 0;
        for (const auto& sample : m_samples) {
            int64_t value = input ? sample.input : sample.gap;
            if (sample.time > now - 2000000 && sample.time <= now && value > 0)
                values[count++] = value;
        }
        if (!count)
            return 0;
        std::sort(values.begin(), values.begin() + count);
        size_t rank = std::max<size_t>(1, (count * std::min(percent, 100u) + 99) / 100);
        size_t index = std::min(count - 1, rank - 1);
        return values[index] / 1000.;
    }
private:
    static constexpr size_t capacity = 128;
    struct Sample { int64_t time; int64_t gap; int64_t input; };
    std::array<Sample, capacity> m_samples { };
    uint64_t m_sequence { 0 };
    int64_t m_lastPresented { 0 };
    int64_t m_lastMeasuredInput { 0 };
};
} }
