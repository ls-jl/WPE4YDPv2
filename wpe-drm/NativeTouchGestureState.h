/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>

namespace WPE {

// DOM completions may arrive after lift-off or after another contact starts.
// Only the latest single-contact group may drive the native gesture detector.
class NativeTouchGestureState {
public:
    void begin(uint32_t sequence, bool hasOtherContacts)
    {
        if (hasOtherContacts) {
            m_multitouch = true;
            return;
        }
        m_sequence = sequence;
        m_valid = true;
        m_multitouch = false;
    }

    void cancel() { m_valid = false; }

    bool acceptsCompletion(uint32_t sequence) const
    {
        return m_valid && !m_multitouch && sequence == m_sequence;
    }

private:
    uint32_t m_sequence { 0 };
    bool m_valid { false };
    bool m_multitouch { false };
};

} // namespace WPE
