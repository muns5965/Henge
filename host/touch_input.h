#ifndef HENGEVM_TOUCH_INPUT_H
#define HENGEVM_TOUCH_INPUT_H
/* C0: touch to the guest's mouse.

   The guest's pointer device is a RELATIVE mouse (virtio-input, host/vm.h hv_vm_mouse_move), and macOS applies pointer acceleration to it, so "move by N" does not land N pixels
   away. A touch screen is absolute. This module turns one into the other with a closed loop: it keeps a target (where the finger is, in guest pixels) and, on every tick, asks the
   guest where its pointer is (hv_vm_pointer, handed in through hv_touch_pointer) and sends a damped fraction of the remaining error. When the guest cannot say where its pointer
   is (hv_vm_pointer returns -1) it homes the pointer to the top-left corner once and then counts its own moves (dead reckoning, 1:1).

   Gestures (times in milliseconds, distances in view pixels; the constants are below):
     touch down            the pointer goes to the finger and follows it
     tap                   left click at the finger (sent once the pointer has got there, or after a short wait)
     long press            right click (held HV_TOUCH_LONG_MS without moving)
     tap, then touch again within HV_TOUCH_DOUBLE_MS near the first tap and hold: left button down; the finger drags; lifting releases it (drag, text selection)
     two fingers moving    scroll (wheel and horizontal wheel); natural direction: the content follows the fingers
     two-finger tap        right click
   Anything else is ignored. A second finger that arrives while a drag is held is ignored.

   Output goes through an HvInputSink whose members have the signatures of hv_vm_key / hv_vm_mouse_move / hv_vm_mouse_button / hv_vm_mouse_wheel / hv_vm_mouse_hwheel (host/vm.h),
   so the real machine's functions can be passed as they are. Mouse buttons: 0 left, 1 right, 2 middle. Wheel units are notches, positive = away from the user (Linux REL_WHEEL). */
#include <stdint.h>

typedef struct {
    void (*key)(uint16_t linux_code, int down);
    void (*mouse_move)(int dx, int dy);
    void (*mouse_button)(int button, int down);
    void (*mouse_wheel)(int notches);
    void (*mouse_hwheel)(int notches);
} HvInputSink;

enum { HV_TOUCH_DOWN = 0, HV_TOUCH_UP = 1, HV_TOUCH_MOVE = 2, HV_TOUCH_CANCEL = 3 };

#define HV_TOUCH_TAP_MS 250
#define HV_TOUCH_LONG_MS 500
#define HV_TOUCH_DOUBLE_MS 300
#define HV_TOUCH_SLOP_PX 12.0f        /* a finger that stays inside this circle has not moved */
#define HV_TOUCH_DOUBLE_DIST_PX 48.0f
#define HV_TOUCH_SCROLL_PX 30.0f      /* one wheel notch per this much two-finger travel */
#define HV_TOUCH_CLICK_WAIT_MS 300    /* a click waits at most this long for the pointer to arrive */
#define HV_TOUCH_STEP_MAX 200

typedef struct HvTouch {
    HvInputSink sink;
    float vx, vy, vw, vh;          /* the rectangle of the view that shows the display */
    int dw, dh;                    /* guest display size in pixels */
    int known, px, py;             /* the guest's pointer, when it could say */
    int homed;                     /* dead-reckoning state valid */
    double ex, ey;                 /* dead-reckoned pointer */
    double tx, ty;                 /* target, guest pixels */
    int has_target;
    int fb_changed;                /* the pointer feedback differs from the one at the last move we sent */
    int last_fb_x, last_fb_y;
    int64_t last_send_ms;
    int sent_since_fb;
    /* fingers */
    int nptr;
    struct { int id; float x, y; } p[2];
    int64_t down_ms, last_tap_ms;
    float down_x, down_y, last_tap_x, last_tap_y;
    int moved, long_fired, drag, multi, scrolled, ignore_rest;
    float scroll_ax, scroll_ay, scroll_acc_x, scroll_acc_y;
    int64_t multi_down_ms;
    float multi_travel;
    int pending_btn, pending_since_valid;   /* click waiting for the pointer: button + 1, 0 = none */
    int64_t pending_ms;
    int button_held;               /* left button currently down (drag) */
    int last_tap_valid;
    /* what the last event was (read through hv_touch_diag only; nothing here changes how a gesture is handled) */
    int ev_count, ev_last_action;
    float ev_last_x, ev_last_y;
    int64_t ev_last_ms;
    int tgt_seq; float tgt_src_x, tgt_src_y;   /* how many times the target was set, and the view point that set it last (diagnostics only) */
} HvTouch;

void hv_touch_init(HvTouch *t, const HvInputSink *sink);
/* The part of the view that shows the display (x, y, width, height in view pixels) and the guest display's size. Call whenever either changes. */
void hv_touch_set_view(HvTouch *t, float x, float y, float w, float h);
void hv_touch_set_display(HvTouch *t, int w, int h);
/* The guest pointer, as hv_vm_pointer reported it: known = 1 with x, y, or known = 0 when it returned -1. Call before each tick. */
void hv_touch_pointer(HvTouch *t, int known, int x, int y);
/* One touch event (Android MotionEvent: action, pointer id, position in view pixels, time in ms). */
void hv_touch_event(HvTouch *t, int action, int id, float x, float y, int64_t now_ms);
/* Call regularly (every 10-20 ms) while anything is going on: moves the pointer toward the target, fires long presses and waiting clicks. */
void hv_touch_tick(HvTouch *t, int64_t now_ms);
/* Read-only view of the state, for a test screen and for tests. Changes nothing. */
typedef struct {
    int events;                  /* touch events received so far (every call of hv_touch_event) */
    int last_action;             /* HV_TOUCH_* of the last one, -1 before any */
    float last_x, last_y;        /* where it was, in view pixels */
    int64_t last_ms;
    int fingers;                 /* fingers down now (0..2) */
    int has_target;              /* a target exists (it stays after the finger lifts; a cancel or release_all clears it) */
    double target_x, target_y;   /* guest pixels, as converted from the view */
    int pointer_known;           /* the module's own idea of the guest pointer: the feedback, or dead reckoning */
    double pointer_x, pointer_y;
    int drag, button_held;
    int tgt_seq;                 /* number of times the target was set (every call that converts a view point, same point or not; UP, two-finger and ignored events do not). A cancel keeps it. */
    float tgt_src_x, tgt_src_y;  /* the view point the current target was converted from; meaningful only while has_target */
} HvTouchDiag;
void hv_touch_diag(const HvTouch *t, HvTouchDiag *d);

/* Releases anything held (the button, nothing else is held): call when the view goes away or the machine stops. */
void hv_touch_release_all(HvTouch *t);

#endif
