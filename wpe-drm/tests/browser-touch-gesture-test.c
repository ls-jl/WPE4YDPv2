#include "../browser-touch-gesture.h"

typedef struct {
    guint count;
} ReplayCount;

static BrowserTouchFrame frame_at(gint64 ms)
{
    return (BrowserTouchFrame) {
        .event_time_ms = (guint32)ms,
        .monotonic_us = ms * 1000,
    };
}

static void set_contact(BrowserTouchFrame *frame, guint slot, int tracking_id,
                        gboolean active, gboolean down, gboolean up,
                        double x, double y)
{
    frame->contacts[slot] = (BrowserTouchContact) {
        .active = active,
        .just_down = down,
        .just_up = up,
        .tracking_id = tracking_id,
        .x = x,
        .y = y,
    };
}

static BrowserToolbarGesture *new_gesture(void)
{
    BrowserToolbarGestureConfig config = {
        .max_move_px = 28,
        .max_position_drift_px = 48,
        .pair_down_window_ms = 160,
        .tap_duration_ms = 350,
        .inter_tap_gap_ms = 800,
        .quiet_ms = 500,
    };
    return browser_toolbar_gesture_new(&config);
}

static void replay_count(const BrowserTouchFrame *frame, gpointer user_data)
{
    (void)frame;
    ((ReplayCount *)user_data)->count++;
}

static void feed_two_finger_tap(BrowserToolbarGesture *gesture, gint64 start_ms,
                                double x_offset)
{
    BrowserTouchFrame down = frame_at(start_ms);
    set_contact(&down, 0, 10, TRUE, TRUE, FALSE, 100 + x_offset, 100);
    set_contact(&down, 1, 11, TRUE, TRUE, FALSE, 180 + x_offset, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &down, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_BUFFER);

    BrowserTouchFrame up = frame_at(start_ms + 80);
    set_contact(&up, 0, 10, FALSE, FALSE, TRUE, 101 + x_offset, 100);
    set_contact(&up, 1, 11, FALSE, FALSE, TRUE, 181 + x_offset, 101);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &up, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_BUFFER);
}

static void test_success_and_quiet_period(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    feed_two_finger_tap(gesture, 0, 0);
    feed_two_finger_tap(gesture, 180, 3);
    feed_two_finger_tap(gesture, 360, -2);
    g_assert_cmpuint(browser_toolbar_gesture_tap_count(gesture), ==, 3);
    g_assert_cmpint(browser_toolbar_gesture_expire(gesture, 939000), ==,
                    BROWSER_TOOLBAR_GESTURE_PASS);
    g_assert_cmpint(browser_toolbar_gesture_expire(gesture, 940000), ==,
                    BROWSER_TOOLBAR_GESTURE_TOGGLE);
    browser_toolbar_gesture_reset(gesture);
    browser_toolbar_gesture_free(gesture);
}

static void test_balanced_gap_is_allowed(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    feed_two_finger_tap(gesture, 0, 0);
    feed_two_finger_tap(gesture, 680, 4);
    feed_two_finger_tap(gesture, 1430, -3);
    g_assert_cmpuint(browser_toolbar_gesture_tap_count(gesture), ==, 3);
    g_assert_cmpint(browser_toolbar_gesture_expire(gesture, 2009000), ==,
                    BROWSER_TOOLBAR_GESTURE_PASS);
    g_assert_cmpint(browser_toolbar_gesture_expire(gesture, 2010000), ==,
                    BROWSER_TOOLBAR_GESTURE_TOGGLE);
    browser_toolbar_gesture_free(gesture);
}

static void test_gap_timeout_replays(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    feed_two_finger_tap(gesture, 0, 0);
    g_assert_cmpint(browser_toolbar_gesture_expire(gesture, 880000), ==,
                    BROWSER_TOOLBAR_GESTURE_REPLAY);
    g_assert_cmpstr(browser_toolbar_gesture_failure_reason(gesture), ==,
                    "gap_timeout");
    ReplayCount replay = { 0 };
    browser_toolbar_gesture_replay(gesture, replay_count, &replay);
    g_assert_cmpuint(replay.count, ==, 2);
    browser_toolbar_gesture_free(gesture);
}

static void test_position_swap_is_allowed(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    feed_two_finger_tap(gesture, 0, 0);

    BrowserTouchFrame down = frame_at(180);
    set_contact(&down, 0, 20, TRUE, TRUE, FALSE, 181, 101);
    set_contact(&down, 1, 21, TRUE, TRUE, FALSE, 101, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &down, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_BUFFER);
    BrowserTouchFrame up = frame_at(260);
    set_contact(&up, 0, 20, FALSE, FALSE, TRUE, 181, 101);
    set_contact(&up, 1, 21, FALSE, FALSE, TRUE, 101, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &up, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_BUFFER);
    g_assert_cmpuint(browser_toolbar_gesture_tap_count(gesture), ==, 2);
    browser_toolbar_gesture_free(gesture);
}

static void test_drift_replays(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    feed_two_finger_tap(gesture, 0, 0);
    BrowserTouchFrame down = frame_at(180);
    set_contact(&down, 0, 20, TRUE, TRUE, FALSE, 200, 180);
    set_contact(&down, 1, 21, TRUE, TRUE, FALSE, 280, 180);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &down, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_REPLAY);
    g_assert_cmpstr(browser_toolbar_gesture_failure_reason(gesture), ==,
                    "position_drift");
    ReplayCount replay = { 0 };
    browser_toolbar_gesture_replay(gesture, replay_count, &replay);
    g_assert_cmpuint(replay.count, ==, 3);
    browser_toolbar_gesture_free(gesture);
}

static void test_move_and_third_contact_replay(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    BrowserTouchFrame down = frame_at(0);
    set_contact(&down, 0, 10, TRUE, TRUE, FALSE, 100, 100);
    set_contact(&down, 1, 11, TRUE, TRUE, FALSE, 180, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &down, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_BUFFER);
    BrowserTouchFrame move = frame_at(30);
    set_contact(&move, 0, 10, TRUE, FALSE, FALSE, 140, 100);
    set_contact(&move, 1, 11, TRUE, FALSE, FALSE, 180, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &move, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_REPLAY);
    g_assert_cmpstr(browser_toolbar_gesture_failure_reason(gesture), ==,
                    "move_exceeded");
    browser_toolbar_gesture_replay(gesture, NULL, NULL);

    down = frame_at(100);
    set_contact(&down, 0, 20, TRUE, TRUE, FALSE, 100, 100);
    set_contact(&down, 1, 21, TRUE, TRUE, FALSE, 180, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &down, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_BUFFER);
    BrowserTouchFrame third = frame_at(120);
    set_contact(&third, 0, 20, TRUE, FALSE, FALSE, 100, 100);
    set_contact(&third, 1, 21, TRUE, FALSE, FALSE, 180, 100);
    set_contact(&third, 2, 22, TRUE, TRUE, FALSE, 240, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &third, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_REPLAY);
    g_assert_cmpstr(browser_toolbar_gesture_failure_reason(gesture), ==,
                    "third_contact");
    browser_toolbar_gesture_free(gesture);
}

static void test_extra_input_during_quiet_replays(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    feed_two_finger_tap(gesture, 0, 0);
    feed_two_finger_tap(gesture, 180, 0);
    feed_two_finger_tap(gesture, 360, 0);
    BrowserTouchFrame extra = frame_at(700);
    set_contact(&extra, 0, 30, TRUE, TRUE, FALSE, 300, 120);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &extra, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_REPLAY);
    g_assert_cmpstr(browser_toolbar_gesture_failure_reason(gesture), ==,
                    "quiet_interrupted");
    ReplayCount replay = { 0 };
    browser_toolbar_gesture_replay(gesture, replay_count, &replay);
    g_assert_cmpuint(replay.count, ==, 7);
    browser_toolbar_gesture_free(gesture);
}

static void test_single_contact_timeout_replays(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    BrowserTouchFrame down = frame_at(0);
    set_contact(&down, 0, 1, TRUE, TRUE, FALSE, 100, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &down, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS);
    g_assert_cmpint(browser_toolbar_gesture_expire(gesture, 160000), ==,
                    BROWSER_TOOLBAR_GESTURE_PASS);
    g_assert_cmpuint(browser_toolbar_gesture_buffered_count(gesture), ==, 0);
    browser_toolbar_gesture_free(gesture);
}

static void test_single_contact_move_replays_immediately(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    BrowserTouchFrame down = frame_at(0);
    set_contact(&down, 0, 1, TRUE, TRUE, FALSE, 100, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &down, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS);
    BrowserTouchFrame move = frame_at(20);
    set_contact(&move, 0, 1, TRUE, FALSE, FALSE, 140, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &move, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS);
    g_assert_cmpuint(browser_toolbar_gesture_buffered_count(gesture), ==, 0);
    browser_toolbar_gesture_free(gesture);
}

static void test_sequential_pair_cancels_speculative_stream(void)
{
    BrowserToolbarGesture *gesture = new_gesture();
    BrowserTouchFrame first = frame_at(0);
    set_contact(&first, 0, 1, TRUE, TRUE, FALSE, 100, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &first, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS);

    BrowserTouchFrame second = frame_at(40);
    set_contact(&second, 0, 1, TRUE, FALSE, FALSE, 100, 100);
    set_contact(&second, 1, 2, TRUE, TRUE, FALSE, 180, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &second, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_CANCEL_BUFFER);

    BrowserTouchFrame up = frame_at(100);
    set_contact(&up, 0, 1, FALSE, FALSE, TRUE, 100, 100);
    set_contact(&up, 1, 2, FALSE, FALSE, TRUE, 180, 100);
    g_assert_cmpint(browser_toolbar_gesture_handle(gesture, &up, TRUE), ==,
                    BROWSER_TOOLBAR_GESTURE_BUFFER);
    g_assert_cmpuint(browser_toolbar_gesture_tap_count(gesture), ==, 1);
    browser_toolbar_gesture_free(gesture);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/browser/touch-gesture/success", test_success_and_quiet_period);
    g_test_add_func("/browser/touch-gesture/balanced-gap", test_balanced_gap_is_allowed);
    g_test_add_func("/browser/touch-gesture/gap-timeout", test_gap_timeout_replays);
    g_test_add_func("/browser/touch-gesture/swapped", test_position_swap_is_allowed);
    g_test_add_func("/browser/touch-gesture/drift", test_drift_replays);
    g_test_add_func("/browser/touch-gesture/move-third", test_move_and_third_contact_replay);
    g_test_add_func("/browser/touch-gesture/quiet-extra", test_extra_input_during_quiet_replays);
    g_test_add_func("/browser/touch-gesture/single-timeout", test_single_contact_timeout_replays);
    g_test_add_func("/browser/touch-gesture/single-move", test_single_contact_move_replays_immediately);
    g_test_add_func("/browser/touch-gesture/sequential-pair", test_sequential_pair_cancels_speculative_stream);
    return g_test_run();
}
