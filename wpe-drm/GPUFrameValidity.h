#pragma once

#include <cstdint>

namespace WebKit {

class GPUFrameValidity {
public:
    bool requiresFullRepaint(int width, int height, bool accumulatedDamageKnown, uint64_t generation) const
    {
        return !m_initialized
            || width <= 0
            || height <= 0
            || width != m_width
            || height != m_height
            || !accumulatedDamageKnown
            || generation <= m_lastRenderedGeneration;
    }

    void didRender(int width, int height, uint64_t generation, bool wasFullRepaint)
    {
        m_initialized = width > 0 && height > 0;
        m_width = width;
        m_height = height;
        m_lastRenderedGeneration = generation;
        if (wasFullRepaint)
            m_lastFullGeneration = generation;
    }

    void invalidate()
    {
        m_initialized = false;
        m_width = 0;
        m_height = 0;
        m_lastRenderedGeneration = 0;
        m_lastFullGeneration = 0;
    }

    bool initialized() const { return m_initialized; }
    uint64_t lastRenderedGeneration() const { return m_lastRenderedGeneration; }
    uint64_t lastFullGeneration() const { return m_lastFullGeneration; }

private:
    bool m_initialized { false };
    int m_width { 0 };
    int m_height { 0 };
    uint64_t m_lastRenderedGeneration { 0 };
    uint64_t m_lastFullGeneration { 0 };
};

} // namespace WebKit
