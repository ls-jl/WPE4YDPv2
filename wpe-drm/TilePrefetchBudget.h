/* SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <cstdint>

namespace WebCore {

inline int boundedVerticalTilePrefetch(int width, int height, int x, int tileWidth,
    int tileHeight, bool memoryPressure)
{
    if (width <= 0 || height <= 0 || tileWidth <= 0 || tileHeight <= 0)
        return 0;
    int64_t offset = ((static_cast<int64_t>(x) % tileWidth) + tileWidth) % tileWidth;
    int64_t columns = (static_cast<int64_t>(offset) + width + tileWidth - 1) / tileWidth;
    int64_t budget = (memoryPressure ? 2 : 4) * 1024 * 1024;
    if (columns * tileWidth > budget / (static_cast<int64_t>(tileHeight) * 8))
        return 0;
    return std::min(height / 2, memoryPressure ? tileHeight / 2 : tileHeight);
}

}
