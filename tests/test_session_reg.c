/* host/restore_session.c coupled to host/restore_registry.c: end() closes the session, a stuck task makes it QUIESCENCE_FAILED, release is gated. No device. */
#define _GNU_SOURCE 1
#include "restore_session.h"
#include "restore_registry.h"
#include "net_guard.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int nfail;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) nfail++; } while (0)

static uint64_t gen;
static int engine(void *a) { (void)a; return 7; }
static volatile int eng_in, eng_go;
static int eng_fn(void *a) { (void)a; eng_in = 1; while (!eng_go) usleep(1000); return 0; }
static void *eng_thread(void *a) { (void)a; int rc; hv_restore_session_run(eng_fn, NULL, &rc); return NULL; }
static void *task(void *a) { usleep((int)(long)a * 1000); hv_reg_complete(gen, HV_REG_K_ASYNC, 0); return NULL; }

int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
    hv_restore_session_set_close_timeout_ms(300);
    int rc = 0;
    if (!strcmp(c, "normal")) {
        CHECK(hv_restore_session_begin() == 0 && hv_reg_state() == HV_REG_ACTIVE, "begin: policy in force and registry ACTIVE");
        gen = hv_reg_generation();
        CHECK(hv_restore_session_begin() == -1, "a second begin without end is refused (this used to reset silently)");
        CHECK(hv_restore_session_run(engine, NULL, &rc) == 0 && rc == 7, "run works in ACTIVE");
        pthread_t t; hv_reg_try_register(gen, HV_REG_K_ASYNC); pthread_create(&t, NULL, task, (void *)50L);
        CHECK(hv_restore_session_release_permitted() == 0, "release is not permitted while the session is ACTIVE");
        CHECK(hv_restore_session_close() == 0 && hv_reg_state() == HV_REG_CLOSED, "close waits for the task and reaches CLOSED");
        pthread_join(t, NULL);
        CHECK(hv_restore_session_release_permitted() == 1, "release permitted after CLOSED");
        CHECK(hv_restore_session_run(engine, NULL, &rc) == -1, "run after close is refused");
        CHECK(hv_restore_session_end() == 0 && hv_reg_state() == HV_REG_IDLE, "end: policy removed, registry IDLE");
        CHECK(hv_restore_session_begin() == 0, "the next session can begin");
        CHECK(hv_restore_session_end() == 0, "and end");
    } else if (!strcmp(c, "stuck")) {
        hv_restore_session_begin(); gen = hv_reg_generation();
        hv_reg_try_register(gen, HV_REG_K_PROXY_CONN);   /* never completes */
        CHECK(hv_restore_session_end() == -1 && hv_restore_session_quiescence_failed(), "end: a stuck task -> QUIESCENCE_FAILED after the deadline, end refused");
        CHECK(hv_restore_session_release_permitted() == 0, "release denied");
        CHECK(hv_net_guard_hooks_intact(), "the connection policy stays in force");
        CHECK(hv_restore_session_begin() == -1, "begin refused");
        hv_reg_complete(gen, HV_REG_K_PROXY_CONN, 0);
        CHECK(hv_restore_session_end() == -1 && hv_restore_session_quiescence_failed(), "a late completion does not lift the terminal state");
    } else if (!strcmp(c, "busy")) {   /* C13: the decision belongs to the engine thread after it returned; an engine still inside run() blocks it */
        hv_restore_session_begin();
        pthread_t t; pthread_create(&t, NULL, eng_thread, NULL);
        while (!eng_in) usleep(1000);
        CHECK(hv_restore_session_close() == -1 && hv_reg_state() == HV_REG_ACTIVE, "close while the engine is inside run(): refused, state unchanged");
        CHECK(hv_restore_session_finish(0, 0, 0) == (int)HV_REG_R_NONE && hv_reg_report_final() == HV_REG_R_NONE, "finish decides and reports nothing then");
        eng_go = 1; pthread_join(t, NULL);
        CHECK(hv_restore_session_finish(0, 0, 0) == (int)HV_REG_R_SUCCESS && hv_reg_state() == HV_REG_IDLE, "after it returned: SUCCESS, IDLE");
        CHECK(hv_restore_session_begin() == 0, "next begin ok");
        CHECK(hv_restore_session_finish(1, 1, 0) == (int)HV_REG_R_ABORTED_BY_USER, "a failing engine after a user cancel: ABORTED_BY_USER");
    } else if (!strcmp(c, "finish")) {
        hv_restore_session_begin(); gen = hv_reg_generation();
        CHECK(hv_restore_session_finish(0, 0, 1) == (int)HV_REG_R_CLEANUP_FAILED && hv_reg_state() == HV_REG_CLOSED, "engine ok + caller cleanup failed: CLEANUP_FAILED, stays CLOSED");
        CHECK(hv_net_guard_hooks_intact() && hv_restore_session_begin() == -1, "policy kept, begin refused");
    } else if (!strcmp(c, "engine")) {
        hv_restore_session_begin();
        CHECK(hv_restore_session_run(engine, NULL, &rc) == 0, "run");
        CHECK(hv_restore_session_end() == 0, "end with no registered task and no engine thread");
    } else { fprintf(stderr, "unknown case\n"); return 2; }
    return nfail;
}
