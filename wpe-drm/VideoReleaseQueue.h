#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include "VideoOverlayWire.h"

namespace WPE {
namespace DRM {

class VideoReleaseQueue {
public:
    enum class AddResult {
        Added,
        Duplicate,
        Overflow,
    };

    explicit VideoReleaseQueue(size_t limit = 64)
        : m_limit(limit)
    {
    }

    AddResult add(uint64_t sequence, uint32_t type = VideoOverlayMessageRelease)
    {
        auto duplicate = std::find_if(m_sequences.begin(), m_sequences.end(),
            [sequence, type](const Message& message) { return message.sequence == sequence && message.type == type; });
        if (!sequence || duplicate != m_sequences.end())
            return AddResult::Duplicate;
        if (m_sequences.size() >= m_limit)
            return AddResult::Overflow;
        m_sequences.push_back({ sequence, type });
        return AddResult::Added;
    }

    bool empty() const { return m_sequences.empty(); }
    size_t size() const { return m_sequences.size(); }
    uint64_t front() const { return m_sequences.front().sequence; }
    uint32_t type() const { return m_sequences.front().type; }
    void pop() { m_sequences.pop_front(); }
    void clear() { m_sequences.clear(); }

private:
    struct Message { uint64_t sequence; uint32_t type; };
    size_t m_limit;
    std::deque<Message> m_sequences;
};

} // namespace DRM
} // namespace WPE
