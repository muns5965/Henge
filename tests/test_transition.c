/* G2-a: the cancel and timeout contract of host/restore_transition.c, and a run of consecutive transitions, against the real libirecovery + host/libusb_shim.c + the
   stand-in ROM of tests/test_irecv.c. No image is transferred and nothing leaves the process: the guest side leaves and re-joins the virtual bus
   (hv_vusb_set_online), which is what devices/bdif.c does after an iBSS / iBEC. host/install.c and the vendored restore tool are NOT involved. Linux. argv[1]:
     cancel_leave   cancel while waiting for the device to leave        -> HV_TR_CANCELLED, quickly (not NO_LEAVE)
     cancel_arrive  cancel while waiting for it to come back            -> HV_TR_CANCELLED, quickly (not NO_ARRIVE)
     bound_leave    the device never leaves: NO_LEAVE after the bound, not much later
     bound_arrive   it leaves and stays away: NO_ARRIVE after the bound, not much later
     seq_ok         three stages (normal, fast, normal): all pass, in order, each with a new client and a failing old client
     seq_stop       stage 1 never returns: the run stops there; stage 2 is not started
     seq_cancel     cancel during stage 1: CANCELLED at stage 1; stage 2 is not started
     seq_before     cancel set before the run: stage 0 is not started
     seq_begin      stage 1's transfer cannot be started: BEGIN_FAILED at stage 1, stage 2 not started
     verdict        the pure verdict function with the cancelled flag
   Exit 0 when the scenario produced what it expects. */
#define main irecv_test_main
#include "test_irecv.c"
#undef main

#include "../host/restore_transition.h"
#include <libirecovery.h>
#include <time.h>

static int msnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

static int bad;
#define EXPECT(cond, ...) do { int ok_ = !!(cond); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

static volatile int g_cancel;
static uint64_t g_ecid = 0x2d2a8d94f32ba4d2ull;

/* ---- single transitions ---- */
typedef struct { int leave_after, back_after, cancel_after; } Plan;   /* ms from the start; -1: never */
static void *actor(void *arg) {
    Plan p = *(Plan *)arg;
    int t0 = msnow(), left = 0, back = 0, canc = 0;
    while (!(left || p.leave_after < 0) || !(back || p.back_after < 0) || !(canc || p.cancel_after < 0)) {
        int t = msnow() - t0;
        if (p.leave_after >= 0 && !left && t >= p.leave_after) { hv_vusb_set_online(g_u, 0); left = 1; }
        if (p.back_after >= 0 && left && !back && t >= p.back_after) { hv_vusb_set_online(g_u, 1); back = 1; }
        if (p.cancel_after >= 0 && !canc && t >= p.cancel_after) { __atomic_store_n(&g_cancel, 1, __ATOMIC_RELEASE); canc = 1; }
        usleep(1000);
        if (t > 4000) break;
    }
    return NULL;
}

static void single(const char *sc) {
    HvTrSpec sp = {3000, 3000, 0x1227, 0x1283};
    Plan p = {-1, -1, -1};
    int lo = 0, hi = 0;   /* bounds on the observed wait, ms */
    int want = HV_TR_OK;
    if (!strcmp(sc, "cancel_leave")) { p.cancel_after = 200; lo = 200, hi = 600; want = HV_TR_CANCELLED; }
    else if (!strcmp(sc, "cancel_arrive")) { p.leave_after = 100, p.cancel_after = 400; lo = 400, hi = 900; want = HV_TR_CANCELLED; }
    else if (!strcmp(sc, "bound_leave")) { sp.leave_ms = 500; lo = 500, hi = 900; want = HV_TR_NO_LEAVE; }
    else if (!strcmp(sc, "bound_arrive")) { sp.arrive_ms = 500; p.leave_after = 100; lo = 600, hi = 1000; want = HV_TR_NO_ARRIVE; }
    pthread_t at;
    pthread_create(&at, NULL, actor, &p);
    HvTrObs o; memset(&o, 0, sizeof o);
    int t0 = msnow();
    hv_transition_observe_c(g_u, &sp, &g_cancel, &o);
    int took = msnow() - t0;
    pthread_join(at, NULL);
    char note[240];
    int v = hv_transition_verdict(&o, &sp, note, sizeof note);
    printf("scenario %s: %s after %d ms -- %s\n", sc, hv_transition_name(v), took, note);
    EXPECT(v == want, "scenario %s verdict is %s", sc, hv_transition_name(want));
    EXPECT(took >= lo && took <= hi, "scenario %s: the wait ended between %d and %d ms (took %d)", sc, lo, hi, took);
    if (want == HV_TR_CANCELLED) EXPECT(o.cancelled && !(v == HV_TR_NO_LEAVE || v == HV_TR_NO_ARRIVE), "a cancel is not reported as a finding about the device");
}

/* ---- runs ---- */
typedef struct { int begun[4]; int fail_begin; irecv_client_t cur; Plan plans[4]; pthread_t th[4]; int started[4]; int cancel_in_stage; } Run;
static Run R;
static int step_begin(int stage, void *user) {
    (void)user;
    R.begun[stage]++;
    if (stage == R.fail_begin) return -1;
    pthread_create(&R.th[stage], NULL, actor, &R.plans[stage]);
    R.started[stage] = 1;
    return 0;
}
static void step_check(int stage, HvTrObs *o, void *user) {
    (void)stage; (void)user;
    irecv_client_t nw = NULL;
    o->opened = irecv_open_with_ecid_and_attempts(&nw, g_ecid, 2) == IRECV_E_SUCCESS;
    if (o->opened) { int m = 0; irecv_get_mode(nw, &m); o->mode = m; }
    unsigned char st[1] = {0};
    int t1 = msnow();
    int r = irecv_usb_control_transfer(R.cur, 0xA1, 5, 0, 0, st, 1, 800);
    o->old_io_hung = msnow() - t1 > 3000;
    o->old_io_failed = r < 0;
    irecv_close(R.cur);
    R.cur = nw;
}

static void run(const char *sc) {
    memset(&R, 0, sizeof R);
    R.fail_begin = -1;
    for (int i = 0; i < 4; i++) R.plans[i] = (Plan){-1, -1, -1};
    HvTrSpec sp[3] = {{2000, 2000, 0x1227, 0x1283}, {2000, 2000, 0x1227, 0x1283}, {2000, 2000, 0x1227, 0x1283}};
    int want = HV_TR_OK, want_stage = -1, want_done = 3;
    int begun_expect[3] = {1, 1, 1};
    R.plans[0] = (Plan){50, 250, -1};
    R.plans[1] = (Plan){50, 50, -1};     /* back at once: seen as one look */
    R.plans[2] = (Plan){50, 250, -1};
    if (!strcmp(sc, "seq_stop")) { R.plans[1] = (Plan){50, -1, -1}; sp[1].arrive_ms = 600; want = HV_TR_NO_ARRIVE, want_stage = 1, want_done = 1; begun_expect[2] = 0; }
    if (!strcmp(sc, "seq_cancel")) { R.plans[1] = (Plan){50, -1, 400}; want = HV_TR_CANCELLED, want_stage = 1, want_done = 1; begun_expect[2] = 0; }
    if (!strcmp(sc, "seq_before")) { __atomic_store_n(&g_cancel, 1, __ATOMIC_RELEASE); want = HV_TR_CANCELLED, want_stage = 0, want_done = 0; begun_expect[0] = begun_expect[1] = begun_expect[2] = 0; }
    if (!strcmp(sc, "seq_begin")) { R.fail_begin = 1; want = HV_TR_BEGIN_FAILED, want_stage = 1, want_done = 1; begun_expect[2] = 0; }
    int opened = irecv_open_with_ecid_and_attempts(&R.cur, g_ecid, 3) == IRECV_E_SUCCESS;
    EXPECT(opened, "the first client opened");
    if (!opened) return;
    HvTrSteps steps = {step_begin, step_check};
    HvTrRun out;
    char note[300];
    int t0 = msnow();
    int v = hv_transition_run(g_u, sp, 3, &g_cancel, &steps, NULL, &out, note, sizeof note);
    printf("scenario %s: run %s after %d ms, %d stage(s) done, failed stage %d -- %s\n", sc, hv_transition_name(v), msnow() - t0, out.stages_done, out.failed_stage, note);
    for (int i = 0; i < 4; i++) if (R.started[i]) pthread_join(R.th[i], NULL);
    EXPECT(v == want && out.verdict == want, "scenario %s verdict is %s", sc, hv_transition_name(want));
    EXPECT(out.failed_stage == want_stage && out.stages_done == want_done, "scenario %s: failed stage %d, stages done %d", sc, want_stage, want_done);
    for (int i = 0; i < 3; i++) EXPECT(R.begun[i] == begun_expect[i], "scenario %s: stage %d transfer started %d time(s) (expected %d)", sc, i, R.begun[i], begun_expect[i]);
    if (R.cur) irecv_close(R.cur);
}

static void verdict(void) {
    HvTrSpec sp = {1000, 1000, 0x1227, 0x1283};
    HvTrObs o = {.left = 1, .left_after_ms = 10, .arrived = 1, .arrived_after_ms = 20, .opened = 1, .mode = 0x1227, .old_io_failed = 1};
    char n[200];
    EXPECT(hv_transition_verdict(&o, &sp, n, sizeof n) == HV_TR_OK, "baseline observation is OK");
    o.cancelled = 1;
    EXPECT(hv_transition_verdict(&o, &sp, n, sizeof n) == HV_TR_CANCELLED, "cancelled wins over an otherwise good observation");
    o.left = 0; o.arrived = 0;
    EXPECT(hv_transition_verdict(&o, &sp, n, sizeof n) == HV_TR_CANCELLED, "cancelled is not NO_LEAVE");
    EXPECT(!strcmp(hv_transition_name(HV_TR_CANCELLED), "cancelled") && !strcmp(hv_transition_name(HV_TR_BEGIN_FAILED), "begin_failed"), "names");
}

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "seq_ok";
    if (!strcmp(sc, "verdict")) { verdict(); return bad ? 1 : 0; }
    g_sc = sc;
    g_dev_ecid = g_ecid;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    pthread_t rt;
    pthread_create(&rt, NULL, rom, NULL);
    hv_vusb_set_online(g_u, 1);
    hv_libusb_shim_attach(g_u);
    if (!strncmp(sc, "seq_", 4)) run(sc);
    else {
        irecv_client_t c = NULL;
        EXPECT(irecv_open_with_ecid_and_attempts(&c, g_ecid, 3) == IRECV_E_SUCCESS, "the first client opened");
        single(sc);
    }
    g_stop = 1;
    pthread_join(rt, NULL);
    printf("%s scenario %s\n", bad ? "FAIL" : "PASS", sc);
    return bad ? 1 : 0;
}
