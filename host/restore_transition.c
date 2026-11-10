#define _GNU_SOURCE
#include "restore_transition.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

void hv_transition_observe(HvVirtioUsb *usb, const HvTrSpec *spec, HvTrObs *o) {
    uint32_t s0 = 0, s = 0;
    hv_vusb_online(usb, &s0);
    int t0 = now_ms(), on;
    o->left = o->arrived = o->fast = 0;
    o->left_after_ms = o->arrived_after_ms = 0;
    for (;;) {
        on = hv_vusb_online(usb, &s);
        if (s != s0 || !on) { o->left = 1; o->left_after_ms = now_ms() - t0; if (on) { o->arrived = 1; o->fast = 1; } break; }
        if (now_ms() - t0 >= spec->leave_ms) return;
        usleep(2000);
    }
    if (o->arrived) return;
    int t1 = now_ms();
    for (;;) {
        on = hv_vusb_online(usb, &s);
        if (on) { o->arrived = 1; o->arrived_after_ms = now_ms() - t1; return; }
        if (now_ms() - t1 >= spec->arrive_ms) return;
        usleep(2000);
    }
}

int hv_transition_verdict(const HvTrObs *o, const HvTrSpec *spec, char *note, int cap) {
#define FAIL(code, ...) do { snprintf(note, (size_t)cap, __VA_ARGS__); return code; } while (0)
    if (!o->left) FAIL(HV_TR_NO_LEAVE, "the device did not leave the bus within %d ms after the transfer (the transfer function returning is not the device switching stages)", spec->leave_ms);
    if (!o->arrived) FAIL(HV_TR_NO_ARRIVE, "the device left (after %d ms) and did not come back within %d ms", o->left_after_ms, spec->arrive_ms);
    if (!o->opened) FAIL(HV_TR_OPEN_FAILED, "the device came back but a new client could not be opened on it");
    if (o->mode < spec->mode_lo || o->mode > spec->mode_hi) FAIL(HV_TR_BAD_MODE, "the new device reports product id 0x%04x, outside the accepted 0x%04x-0x%04x", o->mode, spec->mode_lo, spec->mode_hi);
    if (o->old_io_hung) FAIL(HV_TR_OLD_HANDLE_HUNG, "an operation on the old client did not return: the old handle must fail, not wait");
    if (!o->old_io_failed) FAIL(HV_TR_OLD_HANDLE_USABLE, "an operation on the old client still worked: it is not tied to the old connection");
    snprintf(note, (size_t)cap, "left after %d ms, back %s, new client opened (product id 0x%04x), the old client fails", o->left_after_ms, o->fast ? "in the same look (fast)" : "after a further wait", o->mode);
    return HV_TR_OK;
#undef FAIL
}

const char *hv_transition_name(int r) {
    switch (r) {
    case HV_TR_OK: return "ok";
    case HV_TR_NO_LEAVE: return "no_leave";
    case HV_TR_NO_ARRIVE: return "no_arrive";
    case HV_TR_OPEN_FAILED: return "open_failed";
    case HV_TR_BAD_MODE: return "bad_mode";
    case HV_TR_OLD_HANDLE_USABLE: return "old_handle_usable";
    case HV_TR_OLD_HANDLE_HUNG: return "old_handle_hung";
    }
    return "?";
}
