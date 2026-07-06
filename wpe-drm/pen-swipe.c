#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static void emit_event(int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event event;
    memset(&event, 0, sizeof(event));
    gettimeofday(&event.time, NULL);
    event.type = type;
    event.code = code;
    event.value = value;
    if (write(fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
        fprintf(stderr, "write event type=%u code=%u value=%d failed: %s\n",
            type, code, value, strerror(errno));
        exit(1);
    }
}

static void syn(int fd)
{
    emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /dev/input/eventX [x y0 y1 steps delay_us]\n", argv[0]);
        return 2;
    }
    const char* device = argv[1];
    int x = argc > 2 ? atoi(argv[2]) : 240;
    int y0 = argc > 3 ? atoi(argv[3]) : 780;
    int y1 = argc > 4 ? atoi(argv[4]) : 180;
    int steps = argc > 5 ? atoi(argv[5]) : 30;
    int delay_us = argc > 6 ? atoi(argv[6]) : 12000;
    if (steps < 1)
        steps = 1;

    int fd = open(device, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
        return 1;
    }

    emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 1001);
    emit_event(fd, EV_KEY, BTN_TOUCH, 1);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y0);
    syn(fd);

    for (int i = 1; i <= steps; ++i) {
        int y = y0 + (y1 - y0) * i / steps;
        emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
        emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
        emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
        syn(fd);
        usleep(delay_us);
    }

    emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit_event(fd, EV_KEY, BTN_TOUCH, 0);
    syn(fd);
    close(fd);
    return 0;
}
