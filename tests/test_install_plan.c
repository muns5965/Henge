/* G2-b: the mock install orchestrator (host/install_plan.c) against a FAKE world: a scripted device, a fake clock, step outcomes written by the test. Nothing real is
   reachable from here or from the module. Shows: ordering, skip conditions, retries with waits, reconnects with their own bound and policy, optional steps, goto and the
   run budget as the end of loops, cancel at every point, malformed plans refused before anything runs. Does NOT show anything about a real device, server or disk. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/install_plan.h"

static int bad;
#define EXPECT(cond, ...) do { int ok_ = !!(cond); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

/* ---- the fake world ---- */
#define NS 16
static struct {
    IpResult script[NS][8];  int nscript[NS]; int pos[NS];   /* outcomes of successive runs of step i; after the script: DONE */
    int ran[NS];                                              /* how many times step i ran */
    int order[64], norder;                                    /* step indices in the order they ran */
    int reconnect_ok_at[NS];                                  /* reconnect succeeds from this try on (0 = never) */
    int clock, waits;
    volatile int cancel;
    int cancel_in_run, cancel_in_wait, cancel_in_reconnect;   /* set cancel when this step runs / on the first wait / on this step's reconnect (-1 none) */
    int flag_need_ramdisk;
    char trace[64][64]; int ntrace;
} W;

static void reset(void) { memset(&W, 0, sizeof W); W.cancel_in_run = W.cancel_in_wait = W.cancel_in_reconnect = -1; }
static IpResult run(IpEnv *e, int i, int attempt) {
    (void)e; (void)attempt;
    W.ran[i]++;
    if (W.norder < 64) W.order[W.norder++] = i;
    if (W.cancel_in_run == i) W.cancel = 1;
    IpResult r = W.pos[i] < W.nscript[i] ? W.script[i][W.pos[i]] : IP_DONE;
    W.pos[i]++;
    return r;
}
static void wait_ms(IpEnv *e, int ms) { (void)e; W.clock += ms; W.waits++; if (W.cancel_in_wait == 0) W.cancel = 1; }
static int reconn(IpEnv *e, int i, int k) {
    (void)e;
    if (W.cancel_in_reconnect == i) W.cancel = 1;
    return W.reconnect_ok_at[i] && k >= W.reconnect_ok_at[i] ? 0 : -1;
}
static void trace(IpEnv *e, const char *l) { (void)e; if (W.ntrace < 64) snprintf(W.trace[W.ntrace++], 64, "%s", l); }
static int need_ramdisk(IpEnv *e, int i) { (void)e; (void)i; return W.flag_need_ramdisk; }
static IpEnv env(void) { IpEnv e = {NULL, &W.cancel, wait_ms, reconn, trace}; return e; }
static void script(int i, int n, IpResult a, IpResult b, IpResult c) { W.nscript[i] = n; W.script[i][0] = a; W.script[i][1] = b; W.script[i][2] = c; }

#define STEP(nm) {nm, run, NULL, 1, 0, 0, 0, IP_RECONNECT_FAIL, 0, -1}

static void t_linear(void) {
    reset();
    IpStep p[8] = {STEP("s0"), STEP("s1"), STEP("s2"), STEP("s3"), STEP("s4"), STEP("s5"), STEP("s6"), STEP("s7")};
    IpEnv e = env(); IpReport r;
    IpVerdict v = ip_execute(p, 8, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.steps_done == 8 && r.failed_step == -1 && r.runs == 8, "linear: ok, 8 steps, 8 runs");
    int in_order = W.norder == 8; for (int i = 0; i < 8 && in_order; i++) in_order = W.order[i] == i;
    EXPECT(in_order, "linear: in order");
    EXPECT(r.rec[3].state == IP_ST_DONE && W.ntrace == 16 && !strcmp(W.trace[0], "s0: run 1") && !strcmp(W.trace[1], "s0: done"), "linear: events recorded in order (%d lines)", W.ntrace);
}
static void t_retry(void) {
    reset();
    IpStep p[3] = {STEP("a"), STEP("b"), STEP("c")};
    p[1].max_attempts = 4; p[1].backoff_ms = 250;
    script(1, 2, IP_AGAIN, IP_AGAIN, IP_DONE);
    IpEnv e = env(); IpReport r;
    IpVerdict v = ip_execute(p, 3, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.rec[1].attempts == 3 && W.ran[1] == 3 && W.clock == 500 && W.waits == 2, "retry: transient twice then done: 3 runs, waited 2 x 250 ms (%d runs, %d ms)", W.ran[1], W.clock);
    reset();
    p[1].max_attempts = 2; script(1, 3, IP_AGAIN, IP_AGAIN, IP_AGAIN);
    e = env();
    v = ip_execute(p, 3, &e, 0, &r);
    EXPECT(v == IP_PLAN_FAILED && r.failed_step == 1 && r.rec[1].attempts == 2 && r.rec[1].state == IP_ST_FAILED && W.ran[2] == 0, "retry: exhausted: failed at step 1 after 2 runs, step 2 never ran");
    EXPECT(strstr(r.note, "after 2 run") != NULL, "retry: the note says why (%s)", r.note);
    reset();
    script(1, 1, IP_FAILED, IP_DONE, IP_DONE);
    p[1].max_attempts = 5;
    e = env();
    v = ip_execute(p, 3, &e, 0, &r);
    EXPECT(v == IP_PLAN_FAILED && W.ran[1] == 1 && W.ran[2] == 0 && W.waits == 0, "retry: a permanent failure is not retried");
}
static void t_skip(void) {
    reset();
    IpStep p[4] = {STEP("a"), STEP("ramdisk"), STEP("b"), STEP("c")};
    p[1].when = need_ramdisk;
    IpEnv e = env(); IpReport r;
    W.flag_need_ramdisk = 0;
    IpVerdict v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.rec[1].state == IP_ST_SKIPPED && W.ran[1] == 0 && r.steps_done == 3 && r.runs == 3, "skip: the conditional step is skipped and recorded, the rest runs");
    reset(); W.flag_need_ramdisk = 1; e = env();
    v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.rec[1].state == IP_ST_DONE && W.ran[1] == 1 && r.steps_done == 4, "skip: with the condition true it runs");
}
static void t_reconnect(void) {
    reset();
    IpStep p[3] = {STEP("a"), STEP("ibss"), STEP("c")};
    p[1].await_return = 1; p[1].max_reconnects = 4; p[1].backoff_ms = 100;
    W.reconnect_ok_at[1] = 3;
    IpEnv e = env(); IpReport r;
    IpVerdict v = ip_execute(p, 3, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.rec[1].reconnects == 3 && W.ran[1] == 1 && W.clock == 200, "reconnect: the device came back on the 3rd try (2 waits of 100 ms), the step ran once");
    reset();   /* never back, policy FAIL */
    p[1].on_reconnect_fail = IP_RECONNECT_FAIL; p[1].max_reconnects = 3;
    e = env();
    v = ip_execute(p, 3, &e, 0, &r);
    EXPECT(v == IP_PLAN_FAILED && r.failed_step == 1 && r.rec[1].reconnects == 3 && W.ran[2] == 0 && strstr(r.note, "did not come back"), "reconnect: never back with policy FAIL: failed after 3 tries (%s)", r.note);
    reset();   /* never back at first, the step is redone, then back */
    p[1].on_reconnect_fail = IP_RECONNECT_RETRY_STEP; p[1].max_attempts = 3; p[1].max_reconnects = 2;
    W.reconnect_ok_at[1] = 0;
    e = env();
    v = ip_execute(p, 3, &e, 0, &r);
    EXPECT(v == IP_PLAN_FAILED && W.ran[1] == 3 && r.rec[1].reconnects == 6, "reconnect: policy RETRY_STEP redoes the step within max_attempts (3 runs, 6 reconnect tries)");
    reset();
    p[1].max_reconnects = 1; W.reconnect_ok_at[1] = 1;
    /* the first reconnect fails only on run 1: emulate by a script that makes run 1 succeed but reconnect depend on the run count */
    e = env();
    v = ip_execute(p, 3, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && W.ran[1] == 1, "reconnect: back at once: nothing redone");
}
static void t_optional_goto(void) {
    reset();
    IpStep p[4] = {STEP("a"), STEP("opt"), STEP("c"), STEP("d")};
    p[1].optional = 1; script(1, 1, IP_FAILED, IP_DONE, IP_DONE);
    IpEnv e = env(); IpReport r;
    IpVerdict v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.rec[1].state == IP_ST_FAILED_OPTIONAL && r.steps_done == 3 && W.ran[3] == 1, "optional: a failure is recorded and the plan goes on");
    reset();   /* d fails once, goes back to b (step 1), then everything works */
    p[1].optional = 0; p[3].on_fail_goto = 1; script(3, 1, IP_FAILED, IP_DONE, IP_DONE);
    e = env();
    v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && W.ran[1] == 2 && W.ran[2] == 2 && W.ran[3] == 2 && W.ran[0] == 1, "goto: a failure sends the plan back to step 1; steps 1-3 ran twice, step 0 once");
    reset();   /* it never works: the budget ends the loop */
    script(3, 8, IP_FAILED, IP_FAILED, IP_FAILED);
    for (int k = 3; k < 8; k++) W.script[3][k] = IP_FAILED;
    e = env();
    v = ip_execute(p, 4, &e, 10, &r);
    EXPECT(v == IP_PLAN_BUDGET && r.runs == 10 && r.failed_step >= 0, "goto: a loop that never works ends at the budget (%d runs), not never", r.runs);
    reset(); script(3, 8, IP_FAILED, IP_FAILED, IP_FAILED);
    for (int k = 3; k < 8; k++) W.script[3][k] = IP_FAILED;
    e = env();
    v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_BUDGET && r.runs == 16, "goto: the default budget is 4 x steps (%d)", r.runs);
}
static void t_cancel(void) {
    IpStep p[4] = {STEP("a"), STEP("b"), STEP("c"), STEP("d")};
    IpEnv e; IpReport r; IpVerdict v;
    reset(); W.cancel = 1; e = env();
    v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_CANCELLED && r.runs == 0 && W.norder == 0, "cancel: set before the start: nothing ran");
    reset(); W.cancel_in_run = 1; e = env();
    v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_CANCELLED && r.failed_step == 1 && r.rec[1].state == IP_ST_CANCELLED && W.ran[2] == 0 && r.rec[0].state == IP_ST_DONE, "cancel: during step 1: step 1 cancelled, step 0 stays done, step 2 never ran");
    reset(); p[1].max_attempts = 5; p[1].backoff_ms = 50; script(1, 4, IP_AGAIN, IP_AGAIN, IP_AGAIN); W.cancel_in_wait = 0; e = env();
    v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_CANCELLED && W.ran[1] == 1 && W.waits == 1, "cancel: during the retry wait: no further run (%d runs)", W.ran[1]);
    p[1].max_attempts = 1;
    reset(); p[2].await_return = 1; p[2].max_reconnects = 5; p[2].backoff_ms = 10; W.cancel_in_reconnect = 2; e = env();
    v = ip_execute(p, 4, &e, 0, &r);
    EXPECT(v == IP_PLAN_CANCELLED && r.failed_step == 2 && r.rec[2].reconnects == 1 && W.ran[3] == 0, "cancel: during the wait for the device: it stops after that try (%d tries)", r.rec[2].reconnects);
    EXPECT(strstr(r.note, "not a finding") != NULL, "cancel: the note says it is not a finding about the plan");
}
static void t_invalid(void) {
    reset();
    IpStep ok[2] = {STEP("a"), STEP("b")};
    IpEnv e = env(); IpReport r;
    EXPECT(ip_execute(ok, 0, &e, 0, &r) == IP_PLAN_INVALID, "invalid: no steps");
    EXPECT(ip_execute(NULL, 2, &e, 0, &r) == IP_PLAN_INVALID, "invalid: NULL plan");
    EXPECT(ip_execute(ok, 17, &e, 0, &r) == IP_PLAN_INVALID, "invalid: more than 16 steps");
    IpStep b1[2] = {STEP("a"), STEP("b")}; b1[1].run = NULL;
    EXPECT(ip_execute(b1, 2, &e, 0, &r) == IP_PLAN_INVALID && W.norder == 0, "invalid: a step without run: refused before step 0 runs");
    IpStep b2[2] = {STEP("a"), STEP("b")}; b2[0].on_fail_goto = 2;
    EXPECT(ip_execute(b2, 2, &e, 0, &r) == IP_PLAN_INVALID, "invalid: goto out of range");
    IpStep b3[2] = {STEP("a"), STEP("b")}; b3[0].await_return = 1; IpEnv e2 = env(); e2.reconnect = NULL;
    EXPECT(ip_execute(b3, 2, &e2, 0, &r) == IP_PLAN_INVALID, "invalid: await_return without a reconnect function");
    IpEnv e3 = env(); e3.wait = NULL;
    EXPECT(ip_execute(ok, 2, &e3, 0, &r) == IP_PLAN_INVALID && W.norder == 0, "invalid: no wait function");
}
/* the shape of the real restore: stages that leave the bus and come back, one conditional, one that may be retried, one optional */
static void t_restore_shape(void) {
    reset();
    IpStep p[8] = {STEP("preflight"), STEP("ibss"), STEP("ibec"), STEP("kernel"), STEP("ramdisk"), STEP("restore-mode"), STEP("verify"), STEP("cleanup")};
    for (int i = 1; i <= 3; i++) { p[i].await_return = 1; p[i].max_reconnects = 3; p[i].backoff_ms = 500; p[i].max_attempts = 2; p[i].on_reconnect_fail = IP_RECONNECT_RETRY_STEP; }
    p[4].when = need_ramdisk; p[5].await_return = 1; p[5].max_reconnects = 3; p[5].max_attempts = 2;
    p[7].optional = 1;
    W.flag_need_ramdisk = 1; W.reconnect_ok_at[1] = 2; W.reconnect_ok_at[2] = 1; W.reconnect_ok_at[3] = 3; W.reconnect_ok_at[5] = 1;
    script(5, 1, IP_AGAIN, IP_DONE, IP_DONE); script(7, 1, IP_FAILED, IP_DONE, IP_DONE);
    IpEnv e = env(); IpReport r;
    IpVerdict v = ip_execute(p, 8, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.rec[1].reconnects == 2 && r.rec[3].reconnects == 3 && r.rec[5].attempts == 2 && r.rec[7].state == IP_ST_FAILED_OPTIONAL && r.steps_done == 7, "restore shape: bounces, a retried stage, a failed optional cleanup: plan ok (%d done)", r.steps_done);
    reset(); W.flag_need_ramdisk = 0; W.reconnect_ok_at[1] = W.reconnect_ok_at[2] = W.reconnect_ok_at[3] = W.reconnect_ok_at[5] = 1;
    e = env();
    v = ip_execute(p, 8, &e, 0, &r);
    EXPECT(v == IP_PLAN_OK && r.rec[4].state == IP_ST_SKIPPED, "restore shape: without the ramdisk condition stage 4 is skipped");
    reset(); W.flag_need_ramdisk = 1; W.reconnect_ok_at[1] = 1; W.reconnect_ok_at[2] = 0;   /* ibec never comes back */
    e = env();
    v = ip_execute(p, 8, &e, 0, &r);
    EXPECT(v == IP_PLAN_FAILED && r.failed_step == 2 && W.ran[3] == 0 && W.ran[4] == 0 && W.ran[2] == 2, "restore shape: ibec never back: stops at stage 2 after 2 runs; kernel and later never start");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *sc = argc > 1 ? argv[1] : "all";
    int all = !strcmp(sc, "all");
    if (all || !strcmp(sc, "linear")) t_linear();
    if (all || !strcmp(sc, "retry")) t_retry();
    if (all || !strcmp(sc, "skip")) t_skip();
    if (all || !strcmp(sc, "reconnect")) t_reconnect();
    if (all || !strcmp(sc, "optional_goto")) t_optional_goto();
    if (all || !strcmp(sc, "cancel")) t_cancel();
    if (all || !strcmp(sc, "invalid")) t_invalid();
    if (all || !strcmp(sc, "restore_shape")) t_restore_shape();
    printf("%s test_install_plan %s: %d failed\n", bad ? "FAIL" : "PASS", sc, bad);
    return bad ? 1 : 0;
}
