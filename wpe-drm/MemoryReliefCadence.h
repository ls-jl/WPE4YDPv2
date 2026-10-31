/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>

namespace WTF {
class MemoryReliefCadence {
public:
    bool shouldReclaim(unsigned policy, uint64_t footprint, uint64_t nowUS, bool systemPressure) const
    {
        if (!policy)
            return false;
        return !m_valid || systemPressure || policy > m_policy
            || nowUS < m_lastUS || nowUS - m_lastUS >= 10000000
            || (footprint > m_footprint && footprint - m_footprint >= 32 * 1024 * 1024);
    }
    void didReclaim(unsigned policy, uint64_t footprint, uint64_t nowUS)
    {
        m_valid = true;
        m_policy = policy;
        m_footprint = footprint;
        m_lastUS = nowUS;
    }
private:
    bool m_valid { false };
    unsigned m_policy { 0 };
    uint64_t m_footprint { 0 }, m_lastUS { 0 };
};
}
