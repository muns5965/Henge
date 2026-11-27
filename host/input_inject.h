#ifndef HENGEVM_INPUT_INJECT_H
#define HENGEVM_INPUT_INJECT_H
/* TEMPORARY guest-input test injector (research/g7-input). Runs inside the VM runner process and produces ONLY ordinary virtual-device events through an HvInputSink
   (hv_vm_key -> virtual USB HID keyboard, hv_vm_mouse_move / hv_vm_mouse_button -> virtual relative mouse). It never touches guest memory and does not look at what the guest
   shows. Every step is reported through the log callback so a run can say where an event got to.

   Commands (one text line each, from the runner's stdin as "inject <line>"):
     key NAME            NAME = enter tab shift+tab space esc up down left right (any "+"-joined list of those and shift/ctrl/alt/cmd; the last name is the key, the others are held)
                         each key: down, 40 ms, up
     click X Y           move the pointer to guest pixel X,Y (closed loop with the guest's pointer when it reports one, dead reckoning otherwise), then left button down, 60 ms, up
     dclick X Y          the same, then a second down/up 150 ms after the first up
     click / dclick      the same at the pointer where it is now
     display W H         the guest picture size X,Y are in (default 1920x1080)
     abort               forget the running and queued jobs (a held button or key is released)
   The preset coordinates are in host/input_inject.h's callers only (the Java screen); nothing here knows a screen. */
#include <stddef.h>
#include <stdint.h>
#include "touch_input.h"

#define HV_INJECT_QUEUE 16
#define HV_INJECT_STEPS 8
#define HV_INJECT_ARRIVE_MS 2500   /* a click goes ahead after this long even if the pointer did not get there (and says so) */
#define HV_INJECT_HOLD_MS 60
#define HV_INJECT_KEY_MS 40
#define HV_INJECT_DOUBLE_GAP_MS 150

typedef struct { int kind; int x, y; int count; uint16_t codes[HV_INJECT_STEPS]; int ncodes; char text[40]; } HvInjectJob;

typedef struct HvInject {
    HvInputSink sink;                  /* the real machine's functions (or a recorder in a test) */
    HvTouch touch;                     /* moves the pointer; its own sink is the same */
    void (*log)(void *ctx, const char *line); void *log_ctx;
    HvInjectJob q[HV_INJECT_QUEUE]; int qhead, qtail;
    HvInjectJob cur; int active, phase, clicks_done; int64_t phase_ms, start_ms; int held_btn; int held_key[HV_INJECT_STEPS]; int dw, dh;
    /* counters (what this module itself called, NOT what the guest did) */
    uint64_t jobs_done, keys_down, keys_up, btn_down, btn_up, refused;
} HvInject;

void hv_inject_init(HvInject *j, const HvInputSink *sink, void (*log)(void *, const char *), void *log_ctx);
/* 0 queued / applied; -1 refused (msg says why). line without the word "inject". */
int hv_inject_command(HvInject *j, const char *line, int64_t now_ms, char *msg, size_t cap);
/* The guest's pointer as hv_vm_pointer says it (known 0/1), then one step of the current job. Call every loop pass (<= 20 ms). */
void hv_inject_tick(HvInject *j, int known, int px, int py, int64_t now_ms);
int hv_inject_busy(const HvInject *j);
/* Android key NAME to Linux code (0 = unknown). Exposed for the test. */
uint16_t hv_inject_key_code(const char *name);
#endif
