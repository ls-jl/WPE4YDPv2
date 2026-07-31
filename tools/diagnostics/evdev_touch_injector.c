#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int emit_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event = {
        .type = type,
        .code = code,
        .value = value,
    };

    if (write(fd, &event, sizeof(event)) == (ssize_t)sizeof(event))
        return 0;
    fprintf(stderr, "write type=%u code=%u value=%d failed: %s\n",
        type, code, value, strerror(errno));
    return -1;
}

static int sync_events(int fd)
{
    return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int touch_down(int fd, int x, int y)
{
    static int tracking_id = 20000;

    return emit_event(fd, EV_ABS, ABS_MT_SLOT, 0)
        || emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x)
        || emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y)
        || emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, tracking_id++)
        || emit_event(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 20)
        || emit_event(fd, EV_KEY, BTN_TOUCH, 1)
        || emit_event(fd, EV_KEY, BTN_TOOL_FINGER, 1)
        || sync_events(fd);
}

static int touch_move(int fd, int x, int y)
{
    return emit_event(fd, EV_ABS, ABS_MT_SLOT, 0)
        || emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x)
        || emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y)
        || sync_events(fd);
}

static int touch_up(int fd)
{
    return emit_event(fd, EV_ABS, ABS_MT_SLOT, 0)
        || emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
        || emit_event(fd, EV_KEY, BTN_TOUCH, 0)
        || emit_event(fd, EV_KEY, BTN_TOOL_FINGER, 0)
        || sync_events(fd);
}

static void sleep_milliseconds(int milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) && errno == EINTR) {
    }
}

static int tap(int fd, int x, int y)
{
    if (touch_down(fd, x, y))
        return -1;
    sleep_milliseconds(60);
    return touch_up(fd);
}

static int swipe(int fd, int x1, int y1, int x2, int y2, int steps, int duration_ms)
{
    if (steps < 2)
        steps = 2;
    if (duration_ms < steps)
        duration_ms = steps;
    if (touch_down(fd, x1, y1))
        return -1;

    for (int step = 1; step <= steps; ++step) {
        sleep_milliseconds(duration_ms / steps);
        int x = x1 + (x2 - x1) * step / steps;
        int y = y1 + (y2 - y1) * step / steps;
        if (touch_move(fd, x, y))
            return -1;
    }
    return touch_up(fd);
}

int main(int argc, char** argv)
{
    if (argc != 5 && argc != 9) {
        fprintf(stderr,
            "usage: %s DEVICE tap X Y\n"
            "       %s DEVICE swipe X1 Y1 X2 Y2 STEPS DURATION_MS\n",
            argv[0], argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", argv[1], strerror(errno));
        return 1;
    }

    int result;
    if (!strcmp(argv[2], "tap") && argc == 5)
        result = tap(fd, atoi(argv[3]), atoi(argv[4]));
    else if (!strcmp(argv[2], "swipe") && argc == 9)
        result = swipe(fd, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
            atoi(argv[6]), atoi(argv[7]), atoi(argv[8]));
    else {
        fprintf(stderr, "invalid command\n");
        result = -1;
    }

    close(fd);
    return result ? 1 : 0;
}
