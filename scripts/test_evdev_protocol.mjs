import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'

const production = fs.readFileSync('wpe-drm/wpe-drm-minimal.c', 'utf8')
function method(signature) {
  const start = production.indexOf(signature)
  assert.notEqual(start, -1)
  let end = production.indexOf('{', start) + 1
  let depth = 1
  for (; depth; end++) {
    assert.ok(end < production.length)
    if (production[end] === '{') depth++
    if (production[end] === '}') depth--
  }
  return production.slice(start, end)
}
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-evdev-'))
try {
  const source = `
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
typedef int gboolean, gint;
typedef unsigned guint, guint32, GIOCondition;
typedef void* gpointer;
#define TRUE 1
#define FALSE 0
#define G_IO_ERR 1
#define G_IO_HUP 2
#define G_IO_NVAL 4
#define G_SOURCE_REMOVE 0
#define G_SOURCE_CONTINUE 1
#define MAX_TOUCH_SLOTS 10
typedef struct { int tracking_id, active, just_down, just_up, raw_x, raw_y; } RawTouchSlot;
typedef struct { RawTouchSlot contacts[MAX_TOUCH_SLOTS]; } BrowserTouchFrame;
typedef struct {
    RawTouchSlot raw_slots[MAX_TOUCH_SLOTS];
    int current_slot, abs_x_code, abs_y_code, touch_mt_tracking, touch_discard_until_syn;
    guint toolbar_gesture_source_id, touch_source_id;
    void *toolbar_gesture;
    int live[MAX_TOUCH_SLOTS], downs, ups, cancels, resets;
} AppState;
static void g_warning(const char *format, ...) { (void)format; }
static void g_source_remove(guint id) { (void)id; }
static void browser_toolbar_gesture_reset(void *gesture) { (void)gesture; }
static int64_t g_get_monotonic_time(void) { return 1000000; }
static guint32 input_event_time_ms(AppState *state, struct input_event *event) {
    (void)state; return event->time.tv_sec * 1000 + event->time.tv_usec / 1000;
}
static void cancel_speculative_touch_stream(AppState *state, guint32 time) {
    (void)time;
    state->resets++;
    for (int i=0; i<MAX_TOUCH_SLOTS; i++) {
        if (state->live[i]) state->cancels++;
        state->live[i]=0;
    }
}
static void build_touch_frame(AppState *state, guint32 time, BrowserTouchFrame *frame) {
    (void)time;
    memcpy(frame->contacts, state->raw_slots, sizeof(frame->contacts));
    for (int i=0; i<MAX_TOUCH_SLOTS; i++) {
        state->raw_slots[i].just_down=state->raw_slots[i].just_up=0;
        if (!state->raw_slots[i].active) state->raw_slots[i].tracking_id=-1;
    }
}
static void handle_touch_frame(AppState *state, BrowserTouchFrame *frame) {
    for (int i=0; i<MAX_TOUCH_SLOTS; i++) {
        if (frame->contacts[i].just_down) {
            assert(!state->live[i]); // Never lose UP/CANCEL by overwriting a live sequence.
            state->downs++; state->live[i]=1;
        }
        if (frame->contacts[i].just_up) {
            assert(state->live[i]); state->ups++; state->live[i]=0;
        }
    }
}
${method('static void reset_raw_touch_stream(')}
${method('static void refresh_raw_touch_positions(')}
${method('static gboolean touch_io_cb(')}
static void emit(int fd, int type, int code, int value) {
    struct input_event event={.type=type,.code=code,.value=value};
    assert(write(fd,&event,sizeof(event))==sizeof(event));
}
static void init(AppState *state, int mt) {
    memset(state,0,sizeof(*state)); state->touch_mt_tracking=mt;
    state->abs_x_code=ABS_MT_POSITION_X; state->abs_y_code=ABS_MT_POSITION_Y;
    for (int i=0;i<MAX_TOUCH_SLOTS;i++) state->raw_slots[i].tracking_id=-1;
}
int main(void) {
    int fd[2]; assert(pipe2(fd,O_NONBLOCK)==0);
    AppState state; init(&state,1);
    state.raw_slots[0].raw_x=100; state.raw_slots[0].raw_y=200;
    emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,1);
    emit(fd[1],EV_KEY,BTN_TOUCH,1); emit(fd[1],EV_SYN,SYN_REPORT,0);
    assert(touch_io_cb(fd[0],0,&state));
    assert(state.downs==1 && state.raw_slots[0].tracking_id==1);
    emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,2); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state);
    assert(state.downs==2 && state.cancels==1 && state.live[0]);
    assert(state.raw_slots[0].raw_x==100 && state.raw_slots[0].raw_y==200);
    emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,2); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state); assert(state.downs==2);
    emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,-1);
    emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,3); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state); assert(state.downs==3 && state.cancels==2);
    emit(fd[1],EV_SYN,SYN_DROPPED,0); emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,4);
    emit(fd[1],EV_SYN,SYN_REPORT,0); touch_io_cb(fd[0],0,&state);
    assert(!state.live[0] && state.downs==3 && state.cancels==3);
    emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,5); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state); assert(state.live[0] && state.downs==4);
    emit(fd[1],EV_ABS,ABS_MT_SLOT,1); emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,6);
    emit(fd[1],EV_KEY,BTN_TOUCH,1); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state);
    assert(state.live[0] && state.live[1] && state.downs==5);
    emit(fd[1],EV_ABS,ABS_MT_TRACKING_ID,7);
    emit(fd[1],EV_ABS,ABS_MT_POSITION_X,90); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state);
    assert(state.current_slot==1 && !state.live[0] && state.live[1]);
    assert(state.raw_slots[1].raw_x==90 && state.cancels==5);
    assert(touch_io_cb(fd[0],G_IO_HUP,&state)==G_SOURCE_REMOVE);
    assert(!state.live[0] && !state.live[1] && state.cancels==6);
    init(&state,0);
    emit(fd[1],EV_KEY,BTN_TOUCH,1); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state); assert(state.downs==1);
    emit(fd[1],EV_KEY,BTN_TOUCH,1); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state); assert(state.downs==1);
    emit(fd[1],EV_KEY,BTN_TOUCH,0); emit(fd[1],EV_SYN,SYN_REPORT,0);
    touch_io_cb(fd[0],0,&state); assert(state.ups==1 && !state.live[0]);
    close(fd[0]); close(fd[1]);
}
`
  const input = path.join(directory, 'protocol.c')
  const binary = path.join(directory, 'protocol')
  fs.writeFileSync(input, source)
  const flags = ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror']
  if (process.env.WPE_TEST_SANITIZERS === '1')
    flags.push('-g', '-fno-pie', '-no-pie', '-fno-omit-frame-pointer', '-fsanitize=address,undefined')
  execFileSync('cc', [...flags, input, '-o', binary], { stdio: 'inherit' })
  execFileSync(binary, [], { stdio: 'inherit', timeout: 10000 })
  console.log('Production evdev fixtures passed: MT/legacy, replacement, same-packet UP/DOWN, dropped sync and disconnect')
} finally {
  fs.rmSync(directory, { recursive: true, force: true })
}
