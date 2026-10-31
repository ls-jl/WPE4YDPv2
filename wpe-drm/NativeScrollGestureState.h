/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>

namespace WPE {

class NativeScrollGestureState {
public:
    void begin(uint32_t sequence, uint32_t firstTime, uint32_t latestTime)
    {
        m_previous = m_current;
        m_current = { sequence, firstTime, latestTime, true };
        m_active = true;
    }
    bool active(uint32_t sequence) const { return m_active && m_current.sequence == sequence; }
    void advance(uint32_t sequence, uint32_t time)
    {
        if (active(sequence))
            m_current.last = time;
    }
    void finish(uint32_t sequence, uint32_t time)
    {
        advance(sequence, time);
        if (active(sequence))
            m_active = false;
    }
    bool consumesCompletion(uint32_t sequence, uint32_t time) const
    {
        return contains(m_current, sequence, time) || contains(m_previous, sequence, time);
    }

private:
    struct Range {
        uint32_t sequence { 0 }, first { 0 }, last { 0 };
        bool valid { false };
        Range() = default;
        Range(uint32_t sequence, uint32_t first, uint32_t last, bool valid)
            : sequence(sequence), first(first), last(last), valid(valid) { }
    };
    static bool contains(const Range& range, uint32_t sequence, uint32_t time)
    {
        // evdev milliseconds wrap after 49 days. Gesture spans are short.
        return range.valid && sequence == range.sequence
            && static_cast<uint32_t>(time - range.first)
                <= static_cast<uint32_t>(range.last - range.first);
    }
    Range m_current, m_previous;
    bool m_active { false };
};

} // namespace WPE
