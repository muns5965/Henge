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
     seq_race       stage 0's transfer start itself bounces the device (leave and return before begin() returns): still seen, because the session was taken before
     race_old       the same bounce observed WITHOUT a baseline taken before it is missed (NO_LEAVE): why the baseline is taken before begin()
     check_timeout  the caller's check runs past its bound: HV_TR_CHECK_TIMEOUT, its answers are not used
     check_cancel   cancel while the check runs: a check that honours the flag returns early, the stage is CANCELLED, quickly
     check_hang     (isolated child) a check that never returns: the parent times only after the child entered check(), shows hv_transition_run() does not return (LIMIT_REPRODUCED), SIGKILLs and reaps (CHILD_REAPED); not a safe-end claim
     verdict        the pure verdict function with the cancelled flag and the check bound
   Exit 0 when the scenario produced what it expects. */
#define main irecv_test_main
#include "test_irecv.c"
#undef main

#include "../host/restore_transition.h"
#include <libirecovery.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>

static int msnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

static int bad;
#define EXPECT(cond, ...) do { int ok_ = !!(cond); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

static volatile int g_cancel;
static int g_hang_fd = -1;
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

static void race_old(void) {
    HvTrSpec sp = {300, 300, 0x1227, 0x1283};
    HvTrObs o;
    char note[240];
    uint32_t base = 0;
    hv_vusb_online(g_u, &base);                  /* the baseline the run takes before the transfer is started */
    hv_vusb_set_online(g_u, 0); hv_vusb_set_online(g_u, 1);   /* the transfer's start bounces the device */
    memset(&o, 0, sizeof o);
    hv_transition_observe_c(g_u, &sp, NULL, &o);   /* looks at the session only now: the bounce is already in it */
    int v1 = hv_transition_verdict(&o, &sp, note, sizeof note);
    EXPECT(v1 == HV_TR_NO_LEAVE, "race_old: an observation that starts after the bounce misses it (%s)", hv_transition_name(v1));
    memset(&o, 0, sizeof o);
    hv_transition_observe_b(g_u, &sp, NULL, base, &o);   /* the same bounce against the earlier baseline */
    EXPECT(o.left && o.arrived && o.fast, "race_old: against a baseline taken before it, the bounce is seen (left %d, arrived %d, fast %d)", o.left, o.arrived, o.fast);
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
typedef struct { int begun[4]; int fail_begin; irecv_client_t cur; Plan plans[4]; pthread_t th[4]; int started[4]; int bounce_in_begin; int check_mode; } Run;   /* check_mode: 0 normal, 1 sleeps 600 ms ignoring its bound, 2 polls cancel until the deadline */
static Run R;
static int step_begin(int stage, void *user) {
    (void)user;
    R.begun[stage]++;
    if (stage == R.fail_begin) return -1;
    if (stage == 0 && R.bounce_in_begin) { hv_vusb_set_online(g_u, 0); hv_vusb_set_online(g_u, 1); return 0; }   /* leave and return inside the start */
    pthread_create(&R.th[stage], NULL, actor, &R.plans[stage]);
    R.started[stage] = 1;
    return 0;
}
static void step_check(int stage, HvTrObs *o, const HvTrCheckCtx *ctx, void *user) {
    (void)stage; (void)user;
    if (R.check_mode == 3) { if (write(g_hang_fd, "x", 1) != 1) _exit(3); for (;;) usleep(10000); }   /* enters the check, then never returns and ignores cancel */
    if (R.check_mode == 1) { usleep(600000); o->opened = 1; o->mode = 0x1227; o->old_io_failed = 1; return; }
    if (R.check_mode == 2) {
        int t = msnow();
        while (msnow() - t < ctx->deadline_ms && !(ctx->cancel && __atomic_load_n(ctx->cancel, __ATOMIC_ACQUIRE))) usleep(2000);
        o->opened = 1; o->mode = 0x1227; o->old_io_failed = 1;
        return;
    }
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
    int want = HV_TR_OK, want_stage = -1, want_done = 3, want_fast0 = 0;
    int begun_expect[3] = {1, 1, 1};
    R.plans[0] = (Plan){50, 250, -1};
    R.plans[1] = (Plan){50, 50, -1};     /* back at once: seen as one look */
    R.plans[2] = (Plan){50, 250, -1};
    if (!strcmp(sc, "seq_stop")) { R.plans[1] = (Plan){50, -1, -1}; sp[1].arrive_ms = 600; want = HV_TR_NO_ARRIVE, want_stage = 1, want_done = 1; begun_expect[2] = 0; }
    if (!strcmp(sc, "seq_cancel")) { R.plans[1] = (Plan){50, -1, 400}; want = HV_TR_CANCELLED, want_stage = 1, want_done = 1; begun_expect[2] = 0; }
    if (!strcmp(sc, "seq_before")) { __atomic_store_n(&g_cancel, 1, __ATOMIC_RELEASE); want = HV_TR_CANCELLED, want_stage = 0, want_done = 0; begun_expect[0] = begun_expect[1] = begun_expect[2] = 0; }
    if (!strcmp(sc, "seq_begin")) { R.fail_begin = 1; want = HV_TR_BEGIN_FAILED, want_stage = 1, want_done = 1; begun_expect[2] = 0; }
    if (!strcmp(sc, "seq_race")) { R.bounce_in_begin = 1; want_fast0 = 1; }
    if (!strcmp(sc, "check_timeout")) { R.check_mode = 1; sp[0].check_ms = 200; want = HV_TR_CHECK_TIMEOUT, want_stage = 0, want_done = 0; begun_expect[1] = begun_expect[2] = 0; }
    if (!strcmp(sc, "check_cancel")) { R.check_mode = 2; sp[0].check_ms = 3000; R.plans[0] = (Plan){50, 100, 350}; want = HV_TR_CANCELLED, want_stage = 0, want_done = 0; begun_expect[1] = begun_expect[2] = 0; }
    if (!strcmp(sc, "check_hang_run")) { R.check_mode = 3; sp[0].check_ms = 200; }
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
    if (!strcmp(sc, "check_cancel")) EXPECT(msnow() - t0 < 900, "scenario check_cancel: the run ended soon after the cancel, not at the check's 3 s deadline (%d ms)", msnow() - t0);
    if (!strcmp(sc, "check_timeout")) EXPECT(strstr(note, "took") != NULL, "scenario check_timeout: the sentence names the overrun");
    (void)want_fast0;
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
    o.cancelled = 0; o.left = 1; o.arrived = 1;
    o.check_took_ms = 4999; EXPECT(hv_transition_verdict(&o, &sp, n, sizeof n) == HV_TR_OK, "a check of 4999 ms is within the default bound (5000)");
    o.check_took_ms = 5001; EXPECT(hv_transition_verdict(&o, &sp, n, sizeof n) == HV_TR_CHECK_TIMEOUT, "a check of 5001 ms is over the default bound");
    sp.check_ms = 100; o.check_took_ms = 101; EXPECT(hv_transition_verdict(&o, &sp, n, sizeof n) == HV_TR_CHECK_TIMEOUT, "a check over the spec's own bound (100 ms)");
    EXPECT(!strcmp(hv_transition_name(HV_TR_CHECK_TIMEOUT), "check_timeout"), "name of check_timeout");
    EXPECT(!strcmp(hv_transition_name(HV_TR_CANCELLED), "cancelled") && !strcmp(hv_transition_name(HV_TR_BEGIN_FAILED), "begin_failed"), "names");
}

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "seq_ok";
    if (!strcmp(sc, "verdict")) { verdict(); return bad ? 1 : 0; }
    if (!strcmp(sc, "check_hang")) {
        /* The LIMIT of the current structure, in an isolated child: a check() that never returns also never lets hv_transition_run() return, and a cancel flag it does not read
           cannot end it. The parent starts timing only after the child SAYS it is inside check(), then kills and reaps it. This shows the limit is reproducible; it does NOT
           show hv_transition_run() can end safely when a check hangs, nor that killing a process stops anything inside a real VM. */
        int fds[2];
        EXPECT(pipe(fds) == 0, "pipe");
        pid_t pid = fork();
        if (pid == 0) {   /* a fresh process image: the parent may already hold library state or threads that a bare fork would leave half-alive */
            close(fds[0]);
            char fdn[16]; snprintf(fdn, sizeof fdn, "%d", fds[1]); setenv("HV_HANG_FD", fdn, 1);
            alarm(60);
            execl("/proc/self/exe", argv[0], "check_hang_run", (char *)NULL);
            _exit(4);
        }
        else {
            close(fds[1]);
            char c = 0; int entered = 0, waited = 0;
            while (waited < 8000 && !entered) {   /* the child must reach check() within 8 s (it runs a real stage 0 first) */
                int st; if (waitpid(pid, &st, WNOHANG) == pid) { printf("FAIL the child ended before it entered check() (status %x)\n", st); return 1; }
                struct timespec ts = {0, 20 * 1000000}; nanosleep(&ts, NULL); waited += 20;
                int fl = 0; (void)fl;
                fd_set rf; FD_ZERO(&rf); FD_SET(fds[0], &rf); struct timeval tv = {0, 0};
                if (select(fds[0] + 1, &rf, NULL, NULL, &tv) > 0 && read(fds[0], &c, 1) == 1) entered = 1;
            }
            EXPECT(entered, "check_hang: the child entered check() (signalled after %d ms)", waited);
            int alive = 1, t = 0, st = 0;
            while (entered && t < 1500) { struct timespec ts = {0, 50 * 1000000}; nanosleep(&ts, NULL); t += 50; if (waitpid(pid, &st, WNOHANG) == pid) { alive = 0; break; } }
            EXPECT(entered && alive, "LIMIT_REPRODUCED check_hang: 1500 ms after entering check() (7x its 200 ms bound) hv_transition_run() has not returned, and nothing in it can end the check");
            kill(pid, SIGKILL);
            pid_t r = waitpid(pid, &st, 0);
            EXPECT(r == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "CHILD_REAPED check_hang: SIGKILL sent, the child was reaped with the signal status");
            printf("note: neither line above says hv_transition_run() ends safely on a hung check(); that is not implemented (it needs the check to be bounded below, in the USB calls).\n");
            printf("%s scenario check_hang\n", bad ? "FAIL" : "PASS");
            return bad ? 1 : 0;
        }
    }
    if (getenv("HV_HANG_FD")) g_hang_fd = atoi(getenv("HV_HANG_FD"));
    g_sc = sc;
    g_dev_ecid = g_ecid;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    pthread_t rt;
    pthread_create(&rt, NULL, rom, NULL);
    hv_vusb_set_online(g_u, 1);
    hv_libusb_shim_attach(g_u);
    if (!strcmp(sc, "race_old")) { irecv_client_t c0 = NULL; EXPECT(irecv_open_with_ecid_and_attempts(&c0, g_ecid, 3) == IRECV_E_SUCCESS, "the first client opened"); race_old(); }
    else if (!strncmp(sc, "seq_", 4) || !strncmp(sc, "check_", 6)) run(sc);
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
