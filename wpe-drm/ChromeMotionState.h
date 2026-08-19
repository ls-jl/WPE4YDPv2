#pragma once

#include <stdint.h>

#define WPE_CHROME_MOTION_DATA_KEY "wpe-chrome-motion-state-v2"
#define WPE_CHROME_MOTION_VERSION 2u

typedef enum {
    WPE_CHROME_MOTION_PANEL_NONE = 0,
    WPE_CHROME_MOTION_PANEL_TABS = 1,
    WPE_CHROME_MOTION_PANEL_MENU = 2,
    WPE_CHROME_MOTION_PANEL_LIST = 3,
} WPEChromeMotionPanel;

enum {
    WPE_CHROME_MOTION_FLAG_DRAGGING = 1u << 0,
};

typedef struct {
    uint32_t version;
    uint32_t sequence;
    uint32_t panel;
    uint32_t flags;
    double offset;
    int32_t pressed_row;
    int32_t pressed_segment;
    int32_t pressed_control;
} WPEChromeMotionState;
