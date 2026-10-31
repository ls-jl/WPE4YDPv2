/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool pending;
    bool visible;
    int64_t last_input_us;
} ChromeAutoVisibility;

static inline bool chrome_auto_visibility_ready(const ChromeAutoVisibility *state,
                                                int64_t now_us, bool touches_active)
{
    return state->pending && !touches_active && now_us >= state->last_input_us
        && now_us - state->last_input_us >= 120000;
}
