#define _GNU_SOURCE
#include "restore_transition.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

static int stop_now(volatile int *cancel) { return cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE); }

void hv_transition_observe(HvVirtioUsb *usb, const HvTrSpec *spec, HvTrObs *o) { hv_transition_observe_c(usb, spec, NULL, o); }

void hv_transition_observe_c(HvVirtioUsb *usb, const HvTrSpec *spec, volatile int *cancel, HvTrObs *o) {
    uint32_t s0 = 0;
    hv_vusb_online(usb, &s0);
    hv_transition_observe_b(usb, spec, cancel, s0, o);
}

void hv_transition_observe_b(HvVirtioUsb *usb, const HvTrSpec *spec, volatile int *cancel, uint32_t s0, HvTrObs *o) {
    uint32_t s = 0;
    int t0 = now_ms(), on;
    o->left = o->arrived = o->fast = o->cancelled = 0;
    o->left_after_ms = o->arrived_after_ms = 0;
    for (;;) {
        on = hv_vusb_online(usb, &s);
        if (s != s0 || !on) { o->left = 1; o->left_after_ms = now_ms() - t0; if (on) { o->arrived = 1; o->fast = 1; } break; }
        if (stop_now(cancel)) { o->cancelled = 1; return; }
        if (now_ms() - t0 >= spec->leave_ms) return;
        usleep(2000);
    }
    if (o->arrived) return;
    int t1 = now_ms();
    for (;;) {
        on = hv_vusb_online(usb, &s);
        if (on) { o->arrived = 1; o->arrived_after_ms = now_ms() - t1; return; }
        if (stop_now(cancel)) { o->cancelled = 1; return; }
        if (now_ms() - t1 >= spec->arrive_ms) return;
        usleep(2000);
    }
}

int hv_transition_verdict(const HvTrObs *o, const HvTrSpec *spec, char *note, int cap) {
#define FAIL(code, ...) do { snprintf(note, (size_t)cap, __VA_ARGS__); return code; } while (0)
    if (o->cancelled) FAIL(HV_TR_CANCELLED, "cancelled by the caller while waiting for the device (not a finding about the device)");
    if (!o->left) FAIL(HV_TR_NO_LEAVE, "the device did not leave the bus within %d ms after the transfer (the transfer function returning is not the device switching stages)", spec->leave_ms);
    if (!o->arrived) FAIL(HV_TR_NO_ARRIVE, "the device left (after %d ms) and did not come back within %d ms", o->left_after_ms, spec->arrive_ms);
    {   int bound = spec->check_ms > 0 ? spec->check_ms : HV_TR_CHECK_DEFAULT_MS;
        if (o->check_took_ms > bound) FAIL(HV_TR_CHECK_TIMEOUT, "the check on the returned device took %d ms, over its bound of %d ms (its result is not used)", o->check_took_ms, bound); }
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
    case HV_TR_CANCELLED: return "cancelled";
    case HV_TR_BEGIN_FAILED: return "begin_failed";
    case HV_TR_CHECK_TIMEOUT: return "check_timeout";
    }
    return "?";
}

int hv_transition_run(HvVirtioUsb *usb, const HvTrSpec *specs, int n, volatile int *cancel, const HvTrSteps *steps, void *user, HvTrRun *out, char *note, int cap) {
    out->stages_done = 0, out->failed_stage = -1, out->verdict = HV_TR_OK;
    if (cap > 0) note[0] = 0;
    for (int i = 0; i < n; i++) {
        char one[200];
        int v;
        if (stop_now(cancel)) { out->verdict = HV_TR_CANCELLED, out->failed_stage = i; snprintf(note, (size_t)cap, "stage %d: cancelled before it started", i); return HV_TR_CANCELLED; }
        uint32_t base = 0;
        hv_vusb_online(usb, &base);   /* before the transfer is started: a leave and return inside begin() still show as a changed session */
        if (steps->begin(i, user) != 0) { out->verdict = HV_TR_BEGIN_FAILED, out->failed_stage = i; snprintf(note, (size_t)cap, "stage %d: the transfer could not be started", i); return HV_TR_BEGIN_FAILED; }
        HvTrObs o; memset(&o, 0, sizeof o);
        hv_transition_observe_b(usb, &specs[i], cancel, base, &o);
        if (o.arrived && !o.cancelled) {
            HvTrCheckCtx ctx = {cancel, specs[i].check_ms > 0 ? specs[i].check_ms : HV_TR_CHECK_DEFAULT_MS};
            int t = now_ms();
            steps->check(i, &o, &ctx, user);
            o.check_took_ms = now_ms() - t;
            if (stop_now(cancel)) o.cancelled = 1;
        }
        v = hv_transition_verdict(&o, &specs[i], one, sizeof one);
        if (v != HV_TR_OK) { out->verdict = v, out->failed_stage = i; snprintf(note, (size_t)cap, "stage %d: %s", i, one); return v; }
        out->stages_done++;
    }
    snprintf(note, (size_t)cap, "%d stages passed", n);
    return HV_TR_OK;
}
