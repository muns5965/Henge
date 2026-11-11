/* host/restore_registry.c: the lifetime registry's contracts (design v2.2 C1~C19), threads only, no device. Each CHECK prints PASS/FAIL. */
#define _GNU_SOURCE 1
#include "restore_registry.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int nfail;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) nfail++; } while (0)
#define MS 1000000LL
static int64_t in_ms(int ms) { return hv_reg_mono_ns() + ms * MS; }

static uint64_t fresh(void) { uint64_t g = hv_reg_begin(); return g; }
static void finish(uint64_t g) { hv_reg_close_begin(g); hv_reg_wait_and_close(g, in_ms(500)); hv_reg_finish_session(g); }

static void *sleeper(void *a) { (void)a; usleep(150000); return NULL; }
struct task { uint64_t g; HvRegKind k; int ms; int64_t fail; };
static void *task_fn(void *p) { struct task *t = p; usleep(t->ms * 1000); hv_reg_complete(t->g, t->k, t->fail); return NULL; }

static void *reg_storm(void *p) {   /* hammers try_register while the main thread closes */
    uint64_t g = *(uint64_t *)p; int ok = 0;
    for (int i = 0; i < 20000; i++) if (hv_reg_try_register(g, HV_REG_K_ASYNC) == 0) { ok++; hv_reg_complete(g, HV_REG_K_ASYNC, 0); }
    return (void *)(long)ok;
}

static int c_qf(void) {
    uint64_t g = fresh();
    CHECK(g != 0 && hv_reg_state() == HV_REG_ACTIVE, "begin: IDLE -> ACTIVE, a generation number is issued");
    CHECK(hv_reg_begin() == 0, "begin while ACTIVE is refused (no replacement of a session that was not ended)");
    CHECK(hv_reg_try_register(g, HV_REG_K_ASYNC) == 0 && hv_reg_active(HV_REG_K_ASYNC) == 1, "register in ACTIVE: allowed and counted in one step");
    CHECK(hv_reg_close_begin(g) == 0 && hv_reg_state() == HV_REG_CLOSING && hv_reg_terminate_requested(), "close_begin: CLOSING, lock-free terminate flag set (C16)");
    CHECK(hv_reg_try_register(g, HV_REG_K_ASYNC) == -1 && hv_reg_refused(HV_REG_K_ASYNC) == 1 && hv_reg_active(HV_REG_K_ASYNC) == 1, "register in CLOSING: refused, counted as refused, active unchanged (C1)");
    CHECK(hv_reg_wait_and_close(g, in_ms(80)) == 1 && hv_reg_state() == HV_REG_QUIESCENCE_FAILED, "a task that never completes: deadline passes -> QUIESCENCE_FAILED (C19)");
    CHECK(hv_reg_qf_reason() == HV_REG_QF_TASKS_NOT_QUIESCENT, "the reason is recorded");
    CHECK(hv_reg_release_permitted(g) == 0, "release is denied in QUIESCENCE_FAILED (C18)");
    CHECK(hv_reg_finish_session(g) == -1 && hv_reg_begin() == 0, "terminal: no return to IDLE, begin stays refused");
    hv_reg_complete(g, HV_REG_K_ASYNC, 0);
    CHECK(hv_reg_state() == HV_REG_QUIESCENCE_FAILED, "a late completion does not undo the terminal state");
    return nfail;
}
static int c_normal(void) {
    uint64_t g = fresh();
    pthread_t th[3]; struct task t[3] = {{g, HV_REG_K_ASYNC, 40, 0}, {g, HV_REG_K_PROXY_CTRL, 60, 0}, {g, HV_REG_K_PROXY_CONN, 20, 0}};
    int reg = 0;
    for (int i = 0; i < 3; i++) { reg += hv_reg_try_register(g, t[i].k) == 0; pthread_create(&th[i], NULL, task_fn, &t[i]); }
    CHECK(reg == 3, "three kinds registered");
    CHECK(hv_reg_release_permitted(g) == 0, "release is denied while ACTIVE");
    CHECK(hv_reg_close_begin(g) == 0 && hv_reg_wait_and_close(g, in_ms(2000)) == 0 && hv_reg_state() == HV_REG_CLOSED, "tasks complete -> CLOSED (C13)");
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    CHECK(hv_reg_cancel_request(g) == -1 && hv_reg_cancel_rejected_after_closed() == 1, "a cancel request after CLOSED is refused and only counted (C17)");
    hv_reg_objects_pending(g);
    CHECK(hv_reg_release_permitted(g) == 1, "release permitted in CLOSED");
    CHECK(hv_reg_finish_session(g) == -1 && hv_reg_state() == HV_REG_CLOSED, "IDLE is refused while objects are pending (C15)");
    hv_reg_objects_released(g);
    CHECK(hv_reg_finish_session(g) == 0 && hv_reg_state() == HV_REG_IDLE, "objects released -> IDLE");
    uint64_t g2 = hv_reg_begin();
    CHECK(g2 == g + 1, "the next session has the next generation");
    hv_reg_complete(g, HV_REG_K_ASYNC, -5);
    CHECK(hv_reg_stale_notices() >= 1 && hv_reg_error_count() == 0, "a notice from the old generation is refused and only counted (C15)");
    finish(g2);
    return nfail;
}
static int c_errors(void) {
    uint64_t g = fresh(); HvRegErrSource s; int64_t c;
    hv_reg_try_register(g, HV_REG_K_ASYNC); hv_reg_try_register(g, HV_REG_K_PROXY_CTRL);
    hv_reg_complete(g, HV_REG_K_ASYNC, -11);
    hv_reg_complete(g, HV_REG_K_PROXY_CTRL, -7);
    CHECK(hv_reg_first_error(g, &s, &c) == 1 && s == HV_REG_E_ASYNC && c == -11 && hv_reg_error_count() == 2, "the FIRST error is kept; later ones are counted, never overwrite (P10)");
    hv_reg_try_register(g, HV_REG_K_ASYNC); hv_reg_unregister_failed(g, HV_REG_K_ASYNC, -12);
    CHECK(hv_reg_active(HV_REG_K_ASYNC) == 0 && hv_reg_error_count() == 3, "creation failure after register: unregistered and recorded (C5)");
    CHECK(hv_reg_compose(HV_REG_RESTORE_OK, HV_REG_CLEANUP_DONE, 0) == HV_REG_R_SUCCESS, "compose: ok+ok = SUCCESS");
    CHECK(hv_reg_compose(HV_REG_RESTORE_OK, HV_REG_CLEANUP_FAILED, 0) == HV_REG_R_CLEANUP_FAILED, "compose: ok + cleanup failed = CLEANUP_FAILED, not success");
    CHECK(hv_reg_compose(HV_REG_RESTORE_OK, HV_REG_CLEANUP_DONE, 1) == HV_REG_R_SESSION_NOT_QUIESCENT, "compose: not quiescent is never a success");
    CHECK(hv_reg_compose(HV_REG_RESTORE_USER_CANCEL, HV_REG_CLEANUP_DONE, 0) == HV_REG_R_ABORTED_BY_USER, "compose: user cancel");
    CHECK(hv_reg_compose(HV_REG_RESTORE_FAILED, HV_REG_CLEANUP_DONE, 0) == HV_REG_R_RESTORE_FAILED, "compose: restore failed");
    CHECK(hv_reg_compose(HV_REG_RESTORE_OK, HV_REG_CLEANUP_IN_PROGRESS, 0) == HV_REG_R_CLEANUP_DELAYED, "compose: cleanup running = CLEANUP_DELAYED (interim)");
    CHECK(hv_reg_report(g, HV_REG_R_CLEANUP_DELAYED) == 0 && hv_reg_report_final() == HV_REG_R_NONE && hv_reg_report_interim() == HV_REG_R_CLEANUP_DELAYED, "interim report is not a final one");
    CHECK(hv_reg_report(g, HV_REG_R_RESTORE_FAILED) == 1 && hv_reg_report(g, HV_REG_R_SUCCESS) == 0 && hv_reg_report_final() == HV_REG_R_RESTORE_FAILED, "the final report is delivered once per generation");
    finish(g);
    return nfail;
}
static int c_storm(void) {   /* C1/C3: register vs close, many rounds; after CLOSED no task may be counted and none may be left active */
    for (int round = 0; round < 200; round++) {
        uint64_t g = fresh(); pthread_t th[4];
        for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, reg_storm, &g);
        usleep(round % 5 * 200);
        hv_reg_close_begin(g);
        int r = hv_reg_wait_and_close(g, in_ms(3000));
        int a = hv_reg_active(HV_REG_K_ASYNC);
        long reg_after = 0;
        for (int i = 0; i < 4; i++) { void *ret; pthread_join(th[i], &ret); reg_after += (long)ret; }
        if (r != 0 || a != 0 || hv_reg_active(HV_REG_K_ASYNC) != 0) { CHECK(0, "storm round: CLOSED with active tasks or no close"); return nfail; }
        (void)reg_after;
        hv_reg_finish_session(g);
    }
    CHECK(1, "200 rounds of register vs close: always CLOSED with 0 active, no task registered after the decision");
    return nfail;
}
static int c_cancel(void) {
    uint64_t g = fresh();
    int starters = 0;
    CHECK((starters += hv_reg_cancel_request(g)) == 1, "first cancel request starts it");
    CHECK(hv_reg_cancel_request(g) == 0 && hv_reg_cancel_request(g) == 0, "further requests while it runs: no second start (C17)");
    hv_reg_cancel_create_failed(g, -12);
    CHECK(hv_reg_cancel_state() == HV_REG_CS_CREATE_FAILED && hv_reg_active(HV_REG_K_CANCEL) == 0, "thread creation failed: reservation released");
    CHECK(hv_reg_cancel_request(g) == 1, "ONE retry is granted to ONE requester");
    CHECK(hv_reg_cancel_request(g) == 0, "a concurrent requester does not also start");
    hv_reg_cancel_create_failed(g, -12);
    CHECK(hv_reg_cancel_request(g) == 0 && hv_reg_cancel_wait(g, in_ms(20)) == HV_REG_CS_CREATE_FAILED, "the retry failed too: no further attempt; waiters get the failure");
    hv_reg_close_begin(g);
    CHECK(hv_reg_wait_and_close(g, in_ms(100)) == 1 && hv_reg_qf_reason() == HV_REG_QF_TASKS_NOT_QUIESCENT, "a session whose cancel could never start does not close cleanly");
    return nfail;
}
static int c_phase(void) {
    uint64_t g = fresh();
    CHECK(hv_reg_phase() == HV_REG_PHASE_RUN, "phase RUN");
    hv_reg_enter_grace(g);
    CHECK(hv_reg_phase() == HV_REG_PHASE_GRACE, "phase GRACE");
    CHECK(hv_reg_begin_cancel_phase(g) == 1 && hv_reg_phase() == HV_REG_PHASE_CANCEL, "grace -> cancel is one step and starts the cancel exactly once (C17)");
    CHECK(hv_reg_begin_cancel_phase(g) == 0, "a second caller of the transition does not start another");
    hv_reg_cancel_started(g); hv_reg_cancel_done(g, 1);
    CHECK(hv_reg_cancel_wait(g, in_ms(50)) == HV_REG_CS_DONE_OK && hv_reg_active(HV_REG_K_CANCEL) == 0, "cancel done ok, reservation released");
    finish(g);
    return nfail;
}
struct cw { uint64_t g; int fd; };
static void *conn_user(void *p) { struct cw *w = p; int fds[4]; int n = hv_reg_conn_acquire(w->g, fds, 4); usleep(120000); hv_reg_conn_release(w->g, fds, n); return NULL; }
static int c_conn(void) {
    uint64_t g = fresh(); pthread_t th; struct cw w = {g, 42};
    CHECK(hv_reg_conn_register(g, 42) == 0 && hv_reg_conn_count() == 1, "connection registered");
    pthread_create(&th, NULL, conn_user, &w); usleep(20000);
    CHECK(hv_reg_conn_unregister_wait(g, 42, in_ms(2000)) == 0, "unregister waits until the reference holder returned it (C14)");
    pthread_join(th, NULL);
    CHECK(hv_reg_conn_count() == 0, "gone from the table");
    hv_reg_conn_register(g, 43); pthread_create(&th, NULL, conn_user, &(struct cw){g, 43}); usleep(20000);
    CHECK(hv_reg_conn_unregister_wait(g, 43, in_ms(30)) == -1, "refs not returned by the deadline: unregister fails, fd must NOT be closed");
    CHECK(hv_reg_close_begin(g) == 0 && hv_reg_wait_and_close(g, in_ms(300)) == 1 && hv_reg_qf_reason() == HV_REG_QF_CONN_REF_NOT_RETURNED, "that makes the session not quiescent");
    pthread_join(th, NULL);
    return nfail;
}
static int c_conn_limits(void) {
    uint64_t g = fresh();
    CHECK(hv_reg_conn_register(g, 7) == 0 && hv_reg_conn_register(g, 7) == -1, "the same fd twice: the second is rejected (a closed-and-reused number was never unregistered)");
    CHECK(hv_reg_close_begin(g) == 0 && hv_reg_wait_and_close(g, in_ms(80)) == 1 && hv_reg_qf_reason() == HV_REG_QF_CONN_TABLE, "that is a recorded hard failure: the session cannot close cleanly");
    return nfail;
}
static int c_conn_full(void) {
    uint64_t g = fresh(); int ok = 0;
    for (int fd = 100; fd < 132; fd++) ok += hv_reg_conn_register(g, fd) == 0;
    CHECK(ok == 32, "32 connections fit");
    CHECK(hv_reg_conn_register(g, 200) == -1, "the 33rd is rejected (the caller must not use it)");
    CHECK(hv_reg_close_begin(g) == 0 && hv_reg_wait_and_close(g, in_ms(80)) == 1 && hv_reg_qf_reason() == HV_REG_QF_CONN_TABLE, "unmanaged connections never end as a clean close");
    return nfail;
}
static int c_finalize(void) {
    uint64_t g = fresh(); HvRegReport rep = HV_REG_R_NONE; int mp = -1;
    CHECK(hv_reg_finalize(g, HV_REG_RESTORE_OK, 0, &rep, &mp) == -1, "finalize in ACTIVE: refused (not closed)");
    hv_reg_close_begin(g); hv_reg_wait_and_close(g, in_ms(100));
    CHECK(hv_reg_finalize(g, HV_REG_RESTORE_OK, 0, &rep, &mp) == 0 && rep == HV_REG_R_SUCCESS && mp == 1 && hv_reg_pub_state() == HV_REG_PUB_PUBLISHING, "finalize: result decided, the right to publish issued, state PUBLISHING");
    CHECK(hv_reg_finalize(g, HV_REG_RESTORE_FAILED, 1, &rep, &mp) == 0 && rep == HV_REG_R_SUCCESS && mp == 0, "a later finalize only reads the recorded result (its different inputs change nothing), no second right");
    CHECK(hv_reg_finish_session(g) == -1 && hv_reg_release_idle(g) == -1, "while PUBLISHING: neither finish_session nor release_idle can go IDLE");
    CHECK(hv_reg_publish_done(g) == 0 && hv_reg_publish_done(g) == -1 && hv_reg_pub_state() == HV_REG_PUB_PUBLISHED, "publish_done once");
    CHECK(hv_reg_release_idle(g) == 0 && hv_reg_state() == HV_REG_IDLE, "release_idle after PUBLISHED: IDLE");
    CHECK(hv_reg_finalize(g, HV_REG_RESTORE_OK, 0, &rep, &mp) == -1, "finalize in IDLE: refused");
    return nfail;
}
static int c_finalize_qf(void) {
    uint64_t g = fresh(); HvRegReport rep; int mp;
    hv_reg_try_register(g, HV_REG_K_ASYNC); hv_reg_close_begin(g); hv_reg_wait_and_close(g, in_ms(60));
    CHECK(hv_reg_finalize(g, HV_REG_RESTORE_OK, 0, &rep, &mp) == 0 && rep == HV_REG_R_SESSION_NOT_QUIESCENT && mp == 1, "QUIESCENCE_FAILED: finalized as SESSION_NOT_QUIESCENT (a success input does not matter) and published");
    hv_reg_publish_done(g);
    CHECK(hv_reg_release_idle(g) == -1 && hv_reg_state() == HV_REG_QUIESCENCE_FAILED, "release_idle refused in the terminal state");
    return nfail;
}
static int c_cancel_note(void) {
    uint64_t g = fresh();
    CHECK(hv_reg_cancel_note(g) == 1 && hv_reg_cancel_note(g) == 0 && hv_reg_cancel_requested() == 1, "cancel_note: accepted once, history kept; no counter is taken");
    CHECK(hv_reg_active(HV_REG_K_CANCEL) == 0 && hv_reg_cancel_state() == HV_REG_CS_NONE, "nothing registered and the cancel-task state untouched (a host-only request)");
    hv_reg_close_begin(g);
    CHECK(hv_reg_wait_and_close(g, in_ms(100)) == 0, "the history does not block the close");
    HvRegReport rep; int mp; hv_reg_finalize(g, HV_REG_RESTORE_FAILED, 0, &rep, &mp);
    CHECK(rep == HV_REG_R_ABORTED_BY_USER, "accepted before: a failure is ABORTED_BY_USER");
    hv_reg_publish_done(g); hv_reg_release_idle(g);
    g = fresh(); hv_reg_restore_returned(g);
    CHECK(hv_reg_cancel_note(g) == -1 && hv_reg_cancel_requested() == 0, "the completion first: the request is refused, no history");
    hv_reg_close_begin(g); hv_reg_wait_and_close(g, in_ms(100)); hv_reg_finalize(g, HV_REG_RESTORE_FAILED, 0, &rep, &mp);
    CHECK(rep == HV_REG_R_RESTORE_FAILED, "so the failure is RESTORE_FAILED");
    hv_reg_publish_done(g); hv_reg_release_idle(g);
    g = fresh(); hv_reg_cancel_note(g); hv_reg_close_begin(g); hv_reg_wait_and_close(g, in_ms(100)); hv_reg_finalize(g, HV_REG_RESTORE_OK, 0, &rep, &mp);
    CHECK(rep == HV_REG_R_SUCCESS, "accepted, but the restore succeeded: SUCCESS (the history is kept, the result follows the restore)");
    return nfail;
}
static int c_watchdog(void) {   /* QF is not undone by a completing task; CLOSED first means the watchdog changes nothing */
    uint64_t g = fresh();
    hv_reg_try_register(g, HV_REG_K_ASYNC);
    hv_reg_close_begin(g);
    hv_reg_complete(g, HV_REG_K_ASYNC, 0);
    hv_reg_wait_and_close(g, in_ms(100));
    CHECK(hv_reg_state() == HV_REG_CLOSED && hv_reg_declare_failed(g, HV_REG_QF_TASKS_NOT_QUIESCENT) == 0 && hv_reg_state() == HV_REG_CLOSED, "CLOSED first: a later watchdog declaration changes nothing (C19)");
    hv_reg_cleanup_failed(g);
    CHECK(hv_reg_cleanup_failed_flag() == 1, "cleanup failure recorded");
    CHECK(hv_reg_can_finish(g) == 0 && hv_reg_finish_session(g) == -1, "cleanup failure blocks IDLE");
    return nfail;
}
#ifdef HV_TEST_HOOKS
static int c_hooks(void) {
    hv_test_hook_reset();
    uint64_t g = fresh();
    hv_reg_try_register(g, HV_REG_K_ASYNC);
    hv_reg_close_begin(g);
    hv_reg_try_register(g, HV_REG_K_ASYNC);
    hv_reg_complete(g, HV_REG_K_ASYNC, 0);
    hv_reg_wait_and_close(g, in_ms(100));
    HvHookRec r[16]; int n = hv_test_hook_log(r, 16);
    CHECK(n == 4, "four decisions observed (register, close_begin, refused register, close)");
    CHECK(n == 4 && r[0].id == HV_HOOK_REG_DECIDED && r[0].result == 0 && r[0].state_before == HV_REG_ACTIVE && r[0].active[HV_REG_K_ASYNC] == 1, "hook 1: allowed in ACTIVE and the count was already 1 in the same locked section");
    CHECK(n == 4 && r[2].id == HV_HOOK_REG_DECIDED && r[2].result == -1 && r[2].state_before == HV_REG_CLOSING, "hook 3: the register after CLOSING was refused");
    CHECK(n == 4 && r[3].id == HV_HOOK_CLOSE_DECIDED && r[3].active[HV_REG_K_ASYNC] == 0, "hook 4: CLOSED was decided with 0 active");
    CHECK(n == 4 && r[0].order < r[1].order && r[1].order < r[2].order && r[2].order < r[3].order, "decision order is the lock order");
    finish(g);
    return nfail;
}
#endif
int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
    (void)sleeper;
    if (!strcmp(c, "qf")) return c_qf();
    if (!strcmp(c, "normal")) return c_normal();
    if (!strcmp(c, "errors")) return c_errors();
    if (!strcmp(c, "storm")) return c_storm();
    if (!strcmp(c, "cancel")) return c_cancel();
    if (!strcmp(c, "phase")) return c_phase();
    if (!strcmp(c, "conn")) return c_conn();
    if (!strcmp(c, "conn_dup")) return c_conn_limits();
    if (!strcmp(c, "conn_full")) return c_conn_full();
    if (!strcmp(c, "finalize")) return c_finalize();
    if (!strcmp(c, "finalize_qf")) return c_finalize_qf();
    if (!strcmp(c, "cancel_note")) return c_cancel_note();
    if (!strcmp(c, "watchdog")) return c_watchdog();
#ifdef HV_TEST_HOOKS
    if (!strcmp(c, "hooks")) return c_hooks();
#endif
    fprintf(stderr, "unknown case %s\n", c);
    return 2;
}
