#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

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

    AddResult add(uint64_t sequence)
    {
        if (!sequence || std::find(m_sequences.begin(), m_sequences.end(), sequence) != m_sequences.end())
            return AddResult::Duplicate;
        if (m_sequences.size() >= m_limit)
            return AddResult::Overflow;
        m_sequences.push_back(sequence);
        return AddResult::Added;
    }

    bool empty() const { return m_sequences.empty(); }
    size_t size() const { return m_sequences.size(); }
    uint64_t front() const { return m_sequences.front(); }
    void pop() { m_sequences.pop_front(); }
    void clear() { m_sequences.clear(); }

private:
    size_t m_limit;
    std::deque<uint64_t> m_sequences;
};

} // namespace DRM
} // namespace WPE
