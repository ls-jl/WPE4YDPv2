/* SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace WPE { namespace DRM {

// RGA overwrites the page rectangle completely. Clear only the complementary
// margins so CPU stores do not touch those same pixels before every blit.
inline void clearOutsideARGB8888(uint8_t* pixels, uint32_t pitch,
    uint32_t width, uint32_t height, uint32_t x, uint32_t y,
    uint32_t pageWidth, uint32_t pageHeight, uint32_t color)
{
    auto x0 = std::min(x, width);
    auto y0 = std::min(y, height);
    auto x1 = x0 + std::min(pageWidth, width - x0);
    auto y1 = y0 + std::min(pageHeight, height - y0);
    for (uint32_t rowIndex = 0; rowIndex < height; ++rowIndex) {
        auto* row = reinterpret_cast<uint32_t*>(pixels + static_cast<size_t>(rowIndex) * pitch);
        if (rowIndex < y0 || rowIndex >= y1)
            std::fill(row, row + width, color);
        else {
            std::fill(row, row + x0, color);
            std::fill(row + x1, row + width, color);
        }
    }
}

} } // namespace WPE::DRM
