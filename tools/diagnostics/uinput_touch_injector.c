#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEVICE_NAME "wpe-test-touch"
#define DEFAULT_FIFO "/tmp/wpe-uinput.cmd"

static volatile sig_atomic_t running = 1;
static int uinput_fd = -1;
static const char *fifo_path = DEFAULT_FIFO;
static const char *relay_device_path;
static int next_tracking_id = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int emit_event(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.code = code;
    event.value = value;
    return write(uinput_fd, &event, sizeof(event)) == (ssize_t)sizeof(event) ? 0 : -1;
}

static int sync_events(void)
{
    return emit_event(EV_SYN, SYN_REPORT, 0);
}

static int touch_down(int x, int y)
{
    int tracking_id = next_tracking_id++;
    if (next_tracking_id < 1)
        next_tracking_id = 1;
    int prime_x = x < 480 ? x + 1 : x - 1;
    int prime_y = y < 960 ? y + 1 : y - 1;

    return emit_event(EV_ABS, ABS_MT_SLOT, 0)
        /*
         * uinput suppresses unchanged ABS values. Prime both axes so the
         * first tap after a WPE restart always contains coordinates, even
         * when it repeats the previous tap position.
         */
        || emit_event(EV_ABS, ABS_MT_POSITION_X, prime_x)
        || emit_event(EV_ABS, ABS_MT_POSITION_Y, prime_y)
        || emit_event(EV_ABS, ABS_MT_POSITION_X, x)
        || emit_event(EV_ABS, ABS_MT_POSITION_Y, y)
        || emit_event(EV_ABS, ABS_MT_TRACKING_ID, tracking_id)
        || emit_event(EV_ABS, ABS_MT_TOUCH_MAJOR, 20)
        || emit_event(EV_KEY, BTN_TOUCH, 1)
        || emit_event(EV_KEY, BTN_TOOL_FINGER, 1)
        || sync_events();
}

static int touch_move(int x, int y)
{
    return emit_event(EV_ABS, ABS_MT_SLOT, 0)
        || emit_event(EV_ABS, ABS_MT_POSITION_X, x)
        || emit_event(EV_ABS, ABS_MT_POSITION_Y, y)
        || sync_events();
}

static int touch_up(void)
{
    return emit_event(EV_ABS, ABS_MT_SLOT, 0)
        || emit_event(EV_ABS, ABS_MT_TRACKING_ID, -1)
        || emit_event(EV_KEY, BTN_TOUCH, 0)
        || emit_event(EV_KEY, BTN_TOOL_FINGER, 0)
        || sync_events();
}

static int tap(int x, int y)
{
    if (touch_down(x, y))
        return -1;
    usleep(60000);
    return touch_up();
}

static int swipe(int x1, int y1, int x2, int y2, int steps, int duration_ms)
{
    if (steps < 2)
        steps = 2;
    if (duration_ms < steps)
        duration_ms = steps;
    if (touch_down(x1, y1))
        return -1;

    for (int i = 1; i <= steps; ++i) {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        usleep((useconds_t)duration_ms * 1000 / (unsigned int)steps);
        if (touch_move(x, y))
            return -1;
    }
    return touch_up();
}

static int enable_capability(int fd, unsigned long request, int value)
{
    if (ioctl(fd, request, value) == 0)
        return 0;
    fprintf(stderr, "ioctl request=0x%lx value=%d failed: %s\n", request, value, strerror(errno));
    return -1;
}

static int create_device(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open /dev/uinput failed: %s\n", strerror(errno));
        return -1;
    }

    if (enable_capability(fd, UI_SET_EVBIT, EV_KEY)
        || enable_capability(fd, UI_SET_KEYBIT, BTN_TOUCH)
        || enable_capability(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER)
        || enable_capability(fd, UI_SET_EVBIT, EV_ABS)
        || enable_capability(fd, UI_SET_ABSBIT, ABS_MT_SLOT)
        || enable_capability(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID)
        || enable_capability(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X)
        || enable_capability(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y)
        || enable_capability(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR)
        || enable_capability(fd, UI_SET_ABSBIT, ABS_MT_WIDTH_MAJOR)
        || enable_capability(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)) {
        close(fd);
        return -1;
    }

    struct uinput_user_dev device;
    memset(&device, 0, sizeof(device));
    snprintf(device.name, sizeof(device.name), "%s", DEVICE_NAME);
    device.id.bustype = BUS_VIRTUAL;
    device.id.vendor = 0x18d1;
    device.id.product = 0x4ee7;
    device.id.version = 1;
    device.absmin[ABS_MT_SLOT] = 0;
    device.absmax[ABS_MT_SLOT] = 9;
    device.absmin[ABS_MT_TRACKING_ID] = 0;
    device.absmax[ABS_MT_TRACKING_ID] = 65535;
    device.absmin[ABS_MT_POSITION_X] = 0;
    device.absmax[ABS_MT_POSITION_X] = 480;
    device.absmin[ABS_MT_POSITION_Y] = 0;
    device.absmax[ABS_MT_POSITION_Y] = 960;
    device.absmin[ABS_MT_TOUCH_MAJOR] = 0;
    device.absmax[ABS_MT_TOUCH_MAJOR] = 255;
    device.absmin[ABS_MT_WIDTH_MAJOR] = 0;
    device.absmax[ABS_MT_WIDTH_MAJOR] = 200;

    if (write(fd, &device, sizeof(device)) != (ssize_t)sizeof(device) || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "create uinput device failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    uinput_fd = fd;
    return 0;
}

static void process_command(const char *line)
{
    int x1;
    int y1;
    int x2;
    int y2;
    int steps;
    int duration_ms;

    if (sscanf(line, "tap %d %d", &x1, &y1) == 2) {
        if (tap(x1, y1))
            fprintf(stderr, "tap failed: %s\n", strerror(errno));
        else
            printf("tap raw=%d,%d\n", x1, y1);
    } else if (sscanf(line, "swipe %d %d %d %d %d %d", &x1, &y1, &x2, &y2, &steps, &duration_ms) == 6) {
        if (swipe(x1, y1, x2, y2, steps, duration_ms))
            fprintf(stderr, "swipe failed: %s\n", strerror(errno));
        else
            printf("swipe raw=%d,%d -> %d,%d steps=%d duration_ms=%d\n",
                   x1, y1, x2, y2, steps, duration_ms);
    } else if (!strncmp(line, "quit", 4)) {
        running = 0;
    } else {
        fprintf(stderr, "commands: tap X Y | swipe X1 Y1 X2 Y2 STEPS DURATION_MS | quit\n");
    }
    fflush(stdout);
    fflush(stderr);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        fifo_path = argv[1];
    if (argc > 2)
        relay_device_path = argv[2];

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (create_device())
        return 1;

    int relay_fd = -1;
    if (relay_device_path) {
        relay_fd = open(relay_device_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (relay_fd < 0) {
            fprintf(stderr, "open relay device %s failed: %s\n", relay_device_path, strerror(errno));
            ioctl(uinput_fd, UI_DEV_DESTROY);
            close(uinput_fd);
            return 1;
        }
    }

    unlink(fifo_path);
    if (mkfifo(fifo_path, 0600) && errno != EEXIST) {
        fprintf(stderr, "mkfifo %s failed: %s\n", fifo_path, strerror(errno));
        if (relay_fd >= 0)
            close(relay_fd);
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        return 1;
    }

    int fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        fprintf(stderr, "open fifo %s failed: %s\n", fifo_path, strerror(errno));
        unlink(fifo_path);
        if (relay_fd >= 0)
            close(relay_fd);
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        return 1;
    }

    printf("ready name=%s fifo=%s relay=%s raw_range=0..480,0..960\n",
        DEVICE_NAME, fifo_path, relay_device_path ? relay_device_path : "none");
    fflush(stdout);

    char command_buffer[512];
    size_t command_length = 0;
    while (running) {
        struct pollfd poll_fds[2];
        nfds_t poll_count = 1;
        poll_fds[0].fd = fifo_fd;
        poll_fds[0].events = POLLIN;
        if (relay_fd >= 0) {
            poll_fds[1].fd = relay_fd;
            poll_fds[1].events = POLLIN;
            poll_count = 2;
        }

        int poll_result = poll(poll_fds, poll_count, 500);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            char input[256];
            ssize_t bytes_read = read(fifo_fd, input, sizeof(input));
            if (bytes_read > 0) {
                size_t available = sizeof(command_buffer) - command_length - 1;
                size_t copied = (size_t)bytes_read < available ? (size_t)bytes_read : available;
                memcpy(command_buffer + command_length, input, copied);
                command_length += copied;
                command_buffer[command_length] = '\0';

                char *line_start = command_buffer;
                char *newline;
                while ((newline = strchr(line_start, '\n'))) {
                    *newline = '\0';
                    process_command(line_start);
                    line_start = newline + 1;
                }
                command_length -= (size_t)(line_start - command_buffer);
                memmove(command_buffer, line_start, command_length);
                command_buffer[command_length] = '\0';

                if (copied < (size_t)bytes_read) {
                    fprintf(stderr, "command buffer overflow, dropping partial command\n");
                    command_length = 0;
                }
            }
        }

        if (relay_fd >= 0 && (poll_fds[1].revents & POLLIN)) {
            struct input_event events[32];
            ssize_t bytes_read = read(relay_fd, events, sizeof(events));
            if (bytes_read < 0 && errno != EAGAIN && errno != EINTR) {
                fprintf(stderr, "read relay device failed: %s\n", strerror(errno));
                break;
            }
            if (bytes_read > 0) {
                size_t event_count = (size_t)bytes_read / sizeof(events[0]);
                for (size_t i = 0; i < event_count; ++i) {
                    if (write(uinput_fd, &events[i], sizeof(events[i])) != (ssize_t)sizeof(events[i])) {
                        fprintf(stderr, "relay event failed: %s\n", strerror(errno));
                        running = 0;
                        break;
                    }
                }
            }
        }
    }

    close(fifo_fd);
    if (relay_fd >= 0)
        close(relay_fd);
    unlink(fifo_path);
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    return 0;
}
