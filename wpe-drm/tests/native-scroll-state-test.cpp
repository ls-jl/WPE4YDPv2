#include "NativeScrollGestureState.h"
#include "NativeTouchGestureState.h"
#include "BrowserTouchCoordinates.h"
#include "MemoryReliefCadence.h"
#include "ChromeAutoVisibility.h"
#include <cassert>
#include <cstdio>

int main()
{
    WPE::NativeTouchGestureState touch;
    assert(!touch.acceptsCompletion(95));
    touch.begin(95, false);
    assert(touch.acceptsCompletion(95)); // Delayed UP within the same group is valid.
    touch.begin(96, false);
    assert(!touch.acceptsCompletion(95)); // Old DOWN cannot resurrect a finished gesture.
    assert(touch.acceptsCompletion(96));
    for (uint32_t sequence = 97; sequence < 400; ++sequence) {
        touch.begin(sequence, false);
        assert(!touch.acceptsCompletion(95)); // Not limited to two retired ranges.
        assert(!touch.acceptsCompletion(sequence - 1));
        assert(touch.acceptsCompletion(sequence));
    }
    touch.begin(400, false);
    touch.begin(401, true);
    assert(!touch.acceptsCompletion(400));
    assert(!touch.acceptsCompletion(401)); // Multitouch belongs to DOM, not native panning.
    touch.begin(402, false);
    assert(touch.acceptsCompletion(402));
    touch.cancel();
    assert(!touch.acceptsCompletion(402));
    touch.begin(403, false);
    assert(touch.acceptsCompletion(403));
    ChromeAutoVisibility automatic { true, false, 0 };
    for (int64_t t = 0; t < 1000000; t += 16000) {
        automatic.last_input_us = t;
        assert(!chrome_auto_visibility_ready(&automatic, t, true));
        assert(!chrome_auto_visibility_ready(&automatic, t + 80000, false));
    }
    assert(chrome_auto_visibility_ready(&automatic, automatic.last_input_us + 120000, false));
    automatic.visible = true; // Opposite drag coalesces, preserving the original visible state.
    assert(automatic.visible);
    automatic.pending = false; // Manual/menu action cancels automatic work.
    assert(!chrome_auto_visibility_ready(&automatic, automatic.last_input_us + 500000, false));
    WTF::MemoryReliefCadence cadence;
    const uint64_t mb = 1024 * 1024;
    assert(cadence.shouldReclaim(2, 300 * mb, 0, false));
    cadence.didReclaim(2, 300 * mb, 0);
    assert(!cadence.shouldReclaim(1, 260 * mb, 2000000, false));
    assert(!cadence.shouldReclaim(2, 330 * mb, 4000000, false));
    assert(cadence.shouldReclaim(2, 335 * mb, 4000000, false));
    assert(cadence.shouldReclaim(2, 300 * mb, 10000000, false));
    assert(cadence.shouldReclaim(1, 260 * mb, 2000000, true));
    assert(!cadence.shouldReclaim(0, 300 * mb, 10000000, true));
    cadence.didReclaim(1, 260 * mb, 1000000);
    assert(cadence.shouldReclaim(2, 270 * mb, 2000000, false));
    WPE::NativeScrollGestureState state;
    assert(!state.active(1));
    assert(!state.consumesCompletion(1, 100));
    state.begin(1, 100, 120);
    assert(state.active(1));
    assert(!state.active(2));
    assert(state.consumesCompletion(1, 110));
    assert(!state.consumesCompletion(1, 99));
    assert(!state.consumesCompletion(2, 110));
    state.advance(1, 140);
    assert(state.consumesCompletion(1, 140));
    state.finish(1, 160);
    assert(!state.active(1));
    assert(state.consumesCompletion(1, 160)); // Late DOM cancel cannot reset a new gesture.
    state.begin(2, 200, 220);
    assert(state.consumesCompletion(1, 130));
    assert(!state.consumesCompletion(2, 130));
    assert(!state.consumesCompletion(1, 210));
    state.finish(2, 230);
    state.begin(3, UINT32_MAX - 10, 10);
    assert(state.consumesCompletion(3, UINT32_MAX - 5));
    assert(state.consumesCompletion(3, 5));
    assert(!state.consumesCompletion(3, 11));

    BrowserTouchCoordinates portrait { 80, 880 };
    double before = browser_touch_content_y(portrait, 800);
    double after = browser_touch_content_y(portrait, 780);
    assert(after - before == -20); // Hiding an 80px toolbar is not a +60px move.
    assert(browser_touch_content_y(portrait, 100) == 20);
    BrowserTouchCoordinates hidden { 0, 960 };
    assert(browser_touch_content_y(hidden, 780) == 780);
    assert(browser_touch_content_y(hidden, 760) - browser_touch_content_y(hidden, 780) == -20);
    assert(browser_touch_content_y(portrait, 0) == -80);
    assert(browser_touch_content_y(portrait, 960) == 880);
    BrowserTouchCoordinates landscape { 44, 222 };
    assert(browser_touch_content_y(landscape, 200) - browser_touch_content_y(landscape, 180) == 20);
    puts("native scroll completion and frozen-coordinate tests passed");
}
