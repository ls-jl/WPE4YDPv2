#include "browser-touch-gesture.h"

#include <math.h>
#include <string.h>

#define MAX_BUFFERED_TOUCH_FRAMES 512

typedef enum {
    GESTURE_IDLE = 0,
    GESTURE_WAIT_PAIR,
    GESTURE_PAIR_ACTIVE,
    GESTURE_WAIT_NEXT,
    GESTURE_QUIET,
} GestureState;

typedef struct {
    int tracking_id;
    double x;
    double y;
} GesturePoint;

struct BrowserToolbarGesture {
    BrowserToolbarGestureConfig config;
    GestureState state;
    GArray *frames;
    guint tap_count;
    gint64 first_down_us;
    gint64 pair_down_us;
    gint64 last_tap_up_us;
    gint64 deadline_us;
    GesturePoint pair_points[2];
    GesturePoint reference_points[2];
    GesturePoint first_point;
    gboolean have_first_point;
    gboolean have_reference;
    gboolean speculative_stream_active;
    const char *failure_reason;
};

static double point_distance(const GesturePoint *a, const GesturePoint *b)
{
    return hypot(a->x - b->x, a->y - b->y);
}

static guint active_contacts(const BrowserTouchFrame *frame)
{
    guint count = 0;
    for (guint index = 0; index < BROWSER_TOUCH_MAX_CONTACTS; ++index) {
        if (frame->contacts[index].active)
            count++;
    }
    return count;
}

static guint down_contacts(const BrowserTouchFrame *frame)
{
    guint count = 0;
    for (guint index = 0; index < BROWSER_TOUCH_MAX_CONTACTS; ++index) {
        if (frame->contacts[index].just_down)
            count++;
    }
    return count;
}

static gboolean frame_has_effective_input(const BrowserTouchFrame *frame)
{
    for (guint index = 0; index < BROWSER_TOUCH_MAX_CONTACTS; ++index) {
        const BrowserTouchContact *contact = &frame->contacts[index];
        if (contact->active || contact->just_down || contact->just_up)
            return TRUE;
    }
    return FALSE;
}

static gboolean append_frame(BrowserToolbarGesture *gesture,
                             const BrowserTouchFrame *frame)
{
    if (gesture->frames->len >= MAX_BUFFERED_TOUCH_FRAMES)
        return FALSE;
    g_array_append_val(gesture->frames, *frame);
    return TRUE;
}

static void clear_candidate(BrowserToolbarGesture *gesture)
{
    gesture->state = GESTURE_IDLE;
    gesture->tap_count = 0;
    gesture->first_down_us = 0;
    gesture->pair_down_us = 0;
    gesture->last_tap_up_us = 0;
    gesture->deadline_us = 0;
    gesture->have_reference = FALSE;
    gesture->have_first_point = FALSE;
    gesture->speculative_stream_active = FALSE;
    gesture->failure_reason = NULL;
    memset(gesture->pair_points, 0, sizeof(gesture->pair_points));
    memset(gesture->reference_points, 0, sizeof(gesture->reference_points));
}

static gboolean collect_single_active_point(const BrowserTouchFrame *frame,
                                            GesturePoint *point)
{
    for (guint index = 0; index < BROWSER_TOUCH_MAX_CONTACTS; ++index) {
        const BrowserTouchContact *contact = &frame->contacts[index];
        if (!contact->active)
            continue;
        *point = (GesturePoint) {
            .tracking_id = contact->tracking_id,
            .x = contact->x,
            .y = contact->y,
        };
        return TRUE;
    }
    return FALSE;
}

static gboolean first_contact_has_moved(const BrowserToolbarGesture *gesture,
                                        const BrowserTouchFrame *frame)
{
    if (!gesture->have_first_point)
        return FALSE;
    for (guint index = 0; index < BROWSER_TOUCH_MAX_CONTACTS; ++index) {
        const BrowserTouchContact *contact = &frame->contacts[index];
        if ((!contact->active && !contact->just_up)
            || contact->tracking_id != gesture->first_point.tracking_id)
            continue;
        GesturePoint current = {
            .tracking_id = contact->tracking_id,
            .x = contact->x,
            .y = contact->y,
        };
        return point_distance(&current, &gesture->first_point)
            > gesture->config.max_move_px;
    }
    return FALSE;
}

static gboolean collect_active_points(const BrowserTouchFrame *frame,
                                      GesturePoint points[2])
{
    guint count = 0;
    for (guint index = 0; index < BROWSER_TOUCH_MAX_CONTACTS; ++index) {
        const BrowserTouchContact *contact = &frame->contacts[index];
        if (!contact->active)
            continue;
        if (count >= 2)
            return FALSE;
        points[count++] = (GesturePoint) {
            .tracking_id = contact->tracking_id,
            .x = contact->x,
            .y = contact->y,
        };
    }
    return count == 2;
}

static gboolean points_match_reference(const BrowserToolbarGesture *gesture,
                                       const GesturePoint points[2])
{
    if (!gesture->have_reference)
        return TRUE;
    double direct = MAX(point_distance(&points[0], &gesture->reference_points[0]),
                        point_distance(&points[1], &gesture->reference_points[1]));
    double swapped = MAX(point_distance(&points[0], &gesture->reference_points[1]),
                         point_distance(&points[1], &gesture->reference_points[0]));
    return MIN(direct, swapped) <= gesture->config.max_position_drift_px;
}

static gboolean pair_has_moved(const BrowserToolbarGesture *gesture,
                               const BrowserTouchFrame *frame)
{
    for (guint index = 0; index < BROWSER_TOUCH_MAX_CONTACTS; ++index) {
        const BrowserTouchContact *contact = &frame->contacts[index];
        if (!contact->active && !contact->just_up)
            continue;
        for (guint point = 0; point < 2; ++point) {
            if (gesture->pair_points[point].tracking_id != contact->tracking_id)
                continue;
            GesturePoint current = {
                .tracking_id = contact->tracking_id,
                .x = contact->x,
                .y = contact->y,
            };
            if (point_distance(&current, &gesture->pair_points[point])
                > gesture->config.max_move_px)
                return TRUE;
        }
    }
    return FALSE;
}

static gboolean begin_pair(BrowserToolbarGesture *gesture,
                           const BrowserTouchFrame *frame,
                           const char **failure_reason)
{
    GesturePoint points[2];
    if (!collect_active_points(frame, points)) {
        if (failure_reason)
            *failure_reason = "pair_timeout";
        return FALSE;
    }
    if (!points_match_reference(gesture, points)) {
        if (failure_reason)
            *failure_reason = "position_drift";
        return FALSE;
    }
    memcpy(gesture->pair_points, points, sizeof(points));
    gesture->have_first_point = FALSE;
    gesture->pair_down_us = gesture->first_down_us
        ? gesture->first_down_us : frame->monotonic_us;
    gesture->state = GESTURE_PAIR_ACTIVE;
    gesture->deadline_us = gesture->pair_down_us
        + (gint64)gesture->config.tap_duration_ms * 1000;
    return TRUE;
}

static BrowserToolbarGestureAction fail_candidate(
    BrowserToolbarGesture *gesture, const char *reason)
{
    gesture->deadline_us = 0;
    gesture->failure_reason = reason ? reason : "input_mismatch";
    return BROWSER_TOOLBAR_GESTURE_REPLAY;
}

static BrowserToolbarGestureAction finish_speculative_passthrough(
    BrowserToolbarGesture *gesture)
{
    g_array_set_size(gesture->frames, 0);
    clear_candidate(gesture);
    return BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS;
}

BrowserToolbarGesture *browser_toolbar_gesture_new(
    const BrowserToolbarGestureConfig *config)
{
    BrowserToolbarGesture *gesture = g_new0(BrowserToolbarGesture, 1);
    gesture->config = config ? *config : (BrowserToolbarGestureConfig) {
        .max_move_px = 28,
        .max_position_drift_px = 48,
        .pair_down_window_ms = 160,
        .tap_duration_ms = 350,
        .inter_tap_gap_ms = 800,
        .quiet_ms = 500,
    };
    gesture->frames = g_array_new(FALSE, FALSE, sizeof(BrowserTouchFrame));
    return gesture;
}

void browser_toolbar_gesture_free(BrowserToolbarGesture *gesture)
{
    if (!gesture)
        return;
    g_clear_pointer(&gesture->frames, g_array_unref);
    g_free(gesture);
}

void browser_toolbar_gesture_reset(BrowserToolbarGesture *gesture)
{
    if (!gesture)
        return;
    g_array_set_size(gesture->frames, 0);
    clear_candidate(gesture);
}

BrowserToolbarGestureAction browser_toolbar_gesture_handle(
    BrowserToolbarGesture *gesture, const BrowserTouchFrame *frame,
    gboolean enabled)
{
    g_return_val_if_fail(gesture && frame, BROWSER_TOOLBAR_GESTURE_PASS);

    if (gesture->state == GESTURE_IDLE) {
        if (!enabled || !down_contacts(frame))
            return BROWSER_TOOLBAR_GESTURE_PASS;
        guint active = active_contacts(frame);
        if (!active || active > 2 || down_contacts(frame) != active)
            return BROWSER_TOOLBAR_GESTURE_PASS;
        if (!append_frame(gesture, frame))
            return BROWSER_TOOLBAR_GESTURE_PASS;
        gesture->first_down_us = frame->monotonic_us;
        if (active == 2) {
            const char *reason = NULL;
            if (!begin_pair(gesture, frame, &reason))
                return fail_candidate(gesture, reason);
        } else {
            gesture->have_first_point = collect_single_active_point(
                frame, &gesture->first_point);
            gesture->speculative_stream_active = TRUE;
            gesture->state = GESTURE_WAIT_PAIR;
            gesture->deadline_us = frame->monotonic_us
                + (gint64)gesture->config.pair_down_window_ms * 1000;
            return BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS;
        }
        return BROWSER_TOOLBAR_GESTURE_BUFFER;
    }

    if (!append_frame(gesture, frame))
        return fail_candidate(gesture, "buffer_overflow");
    if (!enabled && gesture->speculative_stream_active)
        return finish_speculative_passthrough(gesture);
    if (!enabled)
        return fail_candidate(gesture, "disabled");

    if (gesture->state == GESTURE_QUIET) {
        if (frame_has_effective_input(frame))
            return fail_candidate(gesture, "quiet_interrupted");
        return BROWSER_TOOLBAR_GESTURE_BUFFER;
    }

    guint active = active_contacts(frame);
    if (active > 2)
        return fail_candidate(gesture, "third_contact");

    if (gesture->state == GESTURE_WAIT_NEXT) {
        if (!down_contacts(frame))
            return BROWSER_TOOLBAR_GESTURE_BUFFER;
        gesture->first_down_us = frame->monotonic_us;
        if (active == 2) {
            const char *reason = NULL;
            if (!begin_pair(gesture, frame, &reason))
                return fail_candidate(gesture, reason);
        } else if (active == 1) {
            gesture->have_first_point = collect_single_active_point(
                frame, &gesture->first_point);
            gesture->state = GESTURE_WAIT_PAIR;
            gint64 pair_deadline = frame->monotonic_us
                + (gint64)gesture->config.pair_down_window_ms * 1000;
            gint64 gap_deadline = gesture->last_tap_up_us
                + (gint64)gesture->config.inter_tap_gap_ms * 1000;
            gesture->deadline_us = MIN(pair_deadline, gap_deadline);
        } else
            return fail_candidate(gesture, "pair_timeout");
        return BROWSER_TOOLBAR_GESTURE_BUFFER;
    }

    if (gesture->state == GESTURE_WAIT_PAIR) {
        if (!active)
            return gesture->speculative_stream_active
                ? finish_speculative_passthrough(gesture)
                : fail_candidate(gesture, "pair_timeout");
        if (first_contact_has_moved(gesture, frame))
            return gesture->speculative_stream_active
                ? finish_speculative_passthrough(gesture)
                : fail_candidate(gesture, "move_exceeded");
        if (active == 2) {
            if (frame->monotonic_us - gesture->first_down_us
                    > (gint64)gesture->config.pair_down_window_ms * 1000)
                return gesture->speculative_stream_active
                    ? finish_speculative_passthrough(gesture)
                    : fail_candidate(gesture, "pair_timeout");
            const char *reason = NULL;
            if (!begin_pair(gesture, frame, &reason))
                return fail_candidate(gesture, reason);
            if (gesture->speculative_stream_active) {
                gesture->speculative_stream_active = FALSE;
                return BROWSER_TOOLBAR_GESTURE_CANCEL_BUFFER;
            }
        }
        return gesture->speculative_stream_active
            ? BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS
            : BROWSER_TOOLBAR_GESTURE_BUFFER;
    }

    if (gesture->state != GESTURE_PAIR_ACTIVE)
        return fail_candidate(gesture, "input_mismatch");
    if (pair_has_moved(gesture, frame))
        return fail_candidate(gesture, "move_exceeded");
    if (frame->monotonic_us - gesture->pair_down_us
            > (gint64)gesture->config.tap_duration_ms * 1000)
        return fail_candidate(gesture, "tap_timeout");
    if (active)
        return BROWSER_TOOLBAR_GESTURE_BUFFER;

    gesture->tap_count++;
    gesture->last_tap_up_us = frame->monotonic_us;
    if (!gesture->have_reference) {
        memcpy(gesture->reference_points, gesture->pair_points,
               sizeof(gesture->pair_points));
        gesture->have_reference = TRUE;
    }
    gesture->first_down_us = 0;
    gesture->pair_down_us = 0;
    if (gesture->tap_count >= 3) {
        gesture->state = GESTURE_QUIET;
        gesture->deadline_us = frame->monotonic_us
            + (gint64)gesture->config.quiet_ms * 1000;
    } else {
        gesture->state = GESTURE_WAIT_NEXT;
        gesture->deadline_us = frame->monotonic_us
            + (gint64)gesture->config.inter_tap_gap_ms * 1000;
    }
    return BROWSER_TOOLBAR_GESTURE_BUFFER;
}

BrowserToolbarGestureAction browser_toolbar_gesture_expire(
    BrowserToolbarGesture *gesture, gint64 now_us)
{
    g_return_val_if_fail(gesture, BROWSER_TOOLBAR_GESTURE_PASS);
    if (gesture->state == GESTURE_IDLE || !gesture->deadline_us
        || now_us < gesture->deadline_us)
        return BROWSER_TOOLBAR_GESTURE_PASS;
    if (gesture->state == GESTURE_QUIET && gesture->tap_count == 3)
        return BROWSER_TOOLBAR_GESTURE_TOGGLE;
    if (gesture->state == GESTURE_WAIT_PAIR && gesture->speculative_stream_active) {
        browser_toolbar_gesture_reset(gesture);
        return BROWSER_TOOLBAR_GESTURE_PASS;
    }
    if (gesture->state == GESTURE_WAIT_PAIR)
        return fail_candidate(gesture, "pair_timeout");
    if (gesture->state == GESTURE_PAIR_ACTIVE)
        return fail_candidate(gesture, "tap_timeout");
    if (gesture->state == GESTURE_WAIT_NEXT)
        return fail_candidate(gesture, "gap_timeout");
    return fail_candidate(gesture, "input_mismatch");
}

gint64 browser_toolbar_gesture_deadline_us(
    const BrowserToolbarGesture *gesture)
{
    return gesture ? gesture->deadline_us : 0;
}

guint browser_toolbar_gesture_tap_count(
    const BrowserToolbarGesture *gesture)
{
    return gesture ? gesture->tap_count : 0;
}

guint browser_toolbar_gesture_buffered_count(
    const BrowserToolbarGesture *gesture)
{
    return gesture && gesture->frames ? gesture->frames->len : 0;
}

const char *browser_toolbar_gesture_failure_reason(
    const BrowserToolbarGesture *gesture)
{
    return gesture && gesture->failure_reason
        ? gesture->failure_reason : "unknown";
}

void browser_toolbar_gesture_replay(BrowserToolbarGesture *gesture,
                                    BrowserTouchFrameFunc callback,
                                    gpointer user_data)
{
    if (!gesture)
        return;
    if (callback) {
        for (guint index = 0; index < gesture->frames->len; ++index) {
            const BrowserTouchFrame *frame = &g_array_index(
                gesture->frames, BrowserTouchFrame, index);
            callback(frame, user_data);
        }
    }
    browser_toolbar_gesture_reset(gesture);
}
