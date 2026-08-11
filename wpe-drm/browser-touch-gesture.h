#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define BROWSER_TOUCH_MAX_CONTACTS 10

typedef struct {
    gboolean active;
    gboolean just_down;
    gboolean just_up;
    int tracking_id;
    int raw_x;
    int raw_y;
    double x;
    double y;
} BrowserTouchContact;

typedef struct {
    guint32 event_time_ms;
    gint64 monotonic_us;
    BrowserTouchContact contacts[BROWSER_TOUCH_MAX_CONTACTS];
} BrowserTouchFrame;

typedef struct {
    double max_move_px;
    double max_position_drift_px;
    guint pair_down_window_ms;
    guint tap_duration_ms;
    guint inter_tap_gap_ms;
    guint quiet_ms;
} BrowserToolbarGestureConfig;

typedef enum {
    BROWSER_TOOLBAR_GESTURE_PASS = 0,
    /* The current one-finger frame is dispatched immediately while the
     * recognizer briefly waits for a second contact. */
    BROWSER_TOOLBAR_GESTURE_SPECULATIVE_PASS,
    BROWSER_TOOLBAR_GESTURE_BUFFER,
    /* A second contact completed the pair. Cancel the speculative web
     * stream, then keep the buffered frames for recognition/replay. */
    BROWSER_TOOLBAR_GESTURE_CANCEL_BUFFER,
    BROWSER_TOOLBAR_GESTURE_REPLAY,
    BROWSER_TOOLBAR_GESTURE_TOGGLE,
} BrowserToolbarGestureAction;

typedef struct BrowserToolbarGesture BrowserToolbarGesture;
typedef void (*BrowserTouchFrameFunc)(const BrowserTouchFrame *frame,
                                      gpointer user_data);

BrowserToolbarGesture *browser_toolbar_gesture_new(
    const BrowserToolbarGestureConfig *config);
void browser_toolbar_gesture_free(BrowserToolbarGesture *gesture);
void browser_toolbar_gesture_reset(BrowserToolbarGesture *gesture);
BrowserToolbarGestureAction browser_toolbar_gesture_handle(
    BrowserToolbarGesture *gesture, const BrowserTouchFrame *frame,
    gboolean enabled);
BrowserToolbarGestureAction browser_toolbar_gesture_expire(
    BrowserToolbarGesture *gesture, gint64 now_us);
gint64 browser_toolbar_gesture_deadline_us(
    const BrowserToolbarGesture *gesture);
guint browser_toolbar_gesture_tap_count(
    const BrowserToolbarGesture *gesture);
guint browser_toolbar_gesture_buffered_count(
    const BrowserToolbarGesture *gesture);
const char *browser_toolbar_gesture_failure_reason(
    const BrowserToolbarGesture *gesture);
void browser_toolbar_gesture_replay(BrowserToolbarGesture *gesture,
                                    BrowserTouchFrameFunc callback,
                                    gpointer user_data);

G_END_DECLS
