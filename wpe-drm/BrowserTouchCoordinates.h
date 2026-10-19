/* SPDX-License-Identifier: MIT */
#pragma once

// Capture at DOWN. A captured touch may move outside its original viewport;
// clamping it would lose displacement when the toolbar changes size.
typedef struct {
    double top_inset;
    int viewport_height;
} BrowserTouchCoordinates;

static inline double browser_touch_content_y(BrowserTouchCoordinates coordinates, double screen_y)
{
    return screen_y - coordinates.top_inset;
}
