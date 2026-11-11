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
static int eng_in, eng_go;
#define LDV(v) __atomic_load_n(&(v), __ATOMIC_SEQ_CST)
#define STV(v, x) __atomic_store_n(&(v), (x), __ATOMIC_SEQ_CST)
static int eng_fn(void *a) { (void)a; STV(eng_in, 1); while (!LDV(eng_go)) usleep(1000); return 0; }
static void *eng_thread(void *a) { (void)a; int rc; hv_restore_session_run(eng_fn, NULL, &rc); return NULL; }
static int hang_go;
static volatile int stranger_close, stranger_fin, stranger_rel, stranger_end;
static void *hang_cb(void *a) { (void)a; while (!LDV(hang_go)) usleep(1000); return NULL; }
static void *stranger(void *a) { (void)a; int mp; stranger_close = hv_restore_session_close(); stranger_fin = hv_restore_session_finalize(0, 0, &mp); stranger_rel = hv_restore_session_release_idle(); stranger_end = hv_restore_session_end(); return NULL; }
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
    } else if (!strcmp(c, "busy")) {   /* C13: the owner decides after the engine returned; an engine still inside run() blocks it */
        hv_restore_session_begin();
        CHECK(hv_restore_session_claim() == 0 && hv_restore_session_claim() == -1, "claim: once per generation");
        pthread_t t; pthread_create(&t, NULL, eng_thread, NULL);
        while (!LDV(eng_in)) usleep(1000);
        int mp = 0;
        CHECK(hv_restore_session_close() == -1 && hv_reg_state() == HV_REG_ACTIVE, "the owner, but the engine is inside run(): close refused, state unchanged");
        CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_NONE && mp == 0 && hv_reg_report_final() == HV_REG_R_NONE, "finalize decides and publishes nothing then");
        STV(eng_go, 1); pthread_join(t, NULL);
        CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_SUCCESS && mp == 1 && hv_reg_pub_state() == HV_REG_PUB_PUBLISHING, "after it returned (the owner's turn, not run()'s): SUCCESS, the right to publish is issued");
        CHECK(hv_restore_session_release_idle() == -1 && hv_reg_state() == HV_REG_CLOSED, "release_idle before publish_done: refused");
        CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_SUCCESS && mp == 0, "a second finalize returns the recorded result and NO new right to publish");
        CHECK(hv_restore_session_publish_done() == 0 && hv_restore_session_release_idle() == 0 && hv_reg_state() == HV_REG_IDLE, "publish_done, then release_idle: IDLE");
        CHECK(hv_restore_session_begin() == 0 && hv_restore_session_claim() == 0, "next generation begins and is claimed");
        CHECK(hv_restore_session_cancel() == 1 && hv_restore_session_cancel() == 0, "cancel: accepted once, then 'already'");
        CHECK(hv_restore_session_finalize(1, 0, &mp) == (int)HV_REG_R_ABORTED_BY_USER, "a failing engine after an accepted cancel: ABORTED_BY_USER (history decides)");
        hv_restore_session_publish_done(); hv_restore_session_release_idle();
        hv_restore_session_begin(); hv_restore_session_claim();
        int rc2; hv_restore_session_run(engine, NULL, &rc2);
        CHECK(hv_restore_session_cancel() == -1, "cancel after the engine returned: refused (completion first)");
        CHECK(hv_restore_session_finalize(1, 0, &mp) == (int)HV_REG_R_RESTORE_FAILED, "...so a failure is RESTORE_FAILED, not a user cancel");
        hv_restore_session_publish_done(); hv_restore_session_release_idle();
    } else if (!strcmp(c, "owner")) {
        hv_restore_session_begin();
        hv_restore_session_claim();
        pthread_t t; pthread_create(&t, NULL, stranger, NULL); pthread_join(t, NULL);
        CHECK(stranger_close == -1 && stranger_fin == (int)HV_REG_R_NONE && stranger_rel == -1 && stranger_end == -1, "another thread: close, finalize, release_idle, end all refused");
        CHECK(hv_reg_state() == HV_REG_ACTIVE, "state unchanged");
        int mp; CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_SUCCESS, "the owner can");
        hv_restore_session_publish_done(); hv_restore_session_release_idle();
        hv_restore_session_begin();   /* a new generation: the old owner record must not carry over */
        CHECK(hv_restore_session_claim() == 0, "the previous owner record was cleared: a fresh claim works");
    } else if (!strcmp(c, "finish")) {
        hv_restore_session_begin(); gen = hv_reg_generation(); hv_restore_session_claim();
        int mp;
        CHECK(hv_restore_session_finalize(0, 1, &mp) == (int)HV_REG_R_CLEANUP_FAILED && mp == 1 && hv_reg_state() == HV_REG_CLOSED, "engine ok + caller cleanup failed: CLEANUP_FAILED, stays CLOSED (state and result code are separate things)");
        CHECK(hv_restore_session_publish_done() == 0 && hv_restore_session_release_idle() == -1 && hv_reg_state() == HV_REG_CLOSED, "published, but a cleanup failure keeps IDLE refused");
        CHECK(hv_net_guard_hooks_intact() && hv_restore_session_begin() == -1, "policy kept, begin refused");
    } else if (!strcmp(c, "pending")) {   /* an object still pending is decided INSIDE finalize: the published result can never be SUCCESS for a session that cannot go IDLE */
        hv_restore_session_begin(); gen = hv_reg_generation(); hv_restore_session_claim();
        hv_reg_objects_pending(gen);
        int mp;
        CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_CLEANUP_FAILED, "a pending (not released) object: CLEANUP_FAILED, not SUCCESS");
        hv_restore_session_publish_done();
        hv_reg_objects_released(gen);
        CHECK(hv_restore_session_release_idle() == -1 && hv_reg_report_final() == HV_REG_R_CLEANUP_FAILED, "the recorded result is not changed afterwards (C15 (f)); IDLE stays refused");
    } else if (!strcmp(c, "reentry")) {   /* what a callback can and cannot do while the result is being published */
        hv_restore_session_begin(); gen = hv_reg_generation(); hv_restore_session_claim();
        int mp;
        int rep = hv_restore_session_finalize(0, 0, &mp);
        /* ---- "inside the callback": PUBLISHING ---- */
        int again_mp = -1, again = hv_restore_session_finalize(0, 0, &again_mp);
        CHECK(rep == (int)HV_REG_R_SUCCESS && mp == 1 && again == rep && again_mp == 0, "finalize inside the callback: the recorded result only, no second right to publish");
        CHECK(hv_restore_session_begin() == -1, "start inside the callback: refused");
        CHECK(hv_restore_session_cancel() == -1, "cancel inside the callback: refused");
        CHECK(hv_restore_session_release_idle() == -1, "release_idle inside the callback: refused (still PUBLISHING)");
        CHECK(hv_restore_session_publish_done() == 0 && hv_restore_session_publish_done() == -1, "publish_done once");
        CHECK(hv_restore_session_release_idle() == 0, "after the callback returned: IDLE");
    } else if (!strcmp(c, "hang")) {   /* a callback that never returns: CLOSED stays, the next session is refused (the test process is not blocked: the 'callback' is a thread) */
        hv_restore_session_begin(); hv_restore_session_claim();
        int mp; hv_restore_session_finalize(0, 0, &mp);
        pthread_t t; pthread_create(&t, NULL, hang_cb, NULL);
        usleep(100000);
        CHECK(hv_reg_state() == HV_REG_CLOSED && hv_restore_session_begin() == -1 && hv_reg_pub_state() == HV_REG_PUB_PUBLISHING, "callback not returned: CLOSED, PUBLISHING, next session refused");
        STV(hang_go, 1); pthread_join(t, NULL);
        hv_restore_session_publish_done();
        CHECK(hv_restore_session_release_idle() == 0 && hv_restore_session_begin() == 0, "late return: only now the next session can begin");
    } else if (!strcmp(c, "engine")) {
        hv_restore_session_begin();
        CHECK(hv_restore_session_run(engine, NULL, &rc) == 0, "run");
        CHECK(hv_restore_session_end() == 0, "end with no registered task and no engine thread");
    } else { fprintf(stderr, "unknown case\n"); return 2; }
    return nfail;
}
