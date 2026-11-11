/* host/restore_session.c coupled to host/restore_registry.c: end() closes the session, a stuck task makes it QUIESCENCE_FAILED, release is gated. No device. */
#define _GNU_SOURCE 1
#include "restore_session.h"
#include "restore_registry.h"
#include "net_guard.h"
#include "hv_life.h"
#include <sys/socket.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int lr_state;
static void *life_recv(void *a) { char b[4]; __atomic_store_n(&lr_state, 1, __ATOMIC_SEQ_CST); recv((int)(long)a, b, 4, 0); __atomic_store_n(&lr_state, 2, __ATOMIC_SEQ_CST); return NULL; }
static int nfail;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) nfail++; } while (0)

static uint64_t gen;
static int engine(void *a) { (void)a; return 7; }
static int eng_in, eng_go;
#define LDV(v) __atomic_load_n(&(v), __ATOMIC_SEQ_CST)
#define STV(v, x) __atomic_store_n(&(v), (x), __ATOMIC_SEQ_CST)
static volatile int b_claim = -1;
static int b_close_in, b_fin_in, b_mp_in, b_final_in, b_state_in, b_fin1, b_mp1, b_pub1, b_rel_early, b_state_early, b_fin2, b_mp2, b_pub_a, b_pub_b, b_rel_ok, b_state_idle;
static int b_cancel_a, b_cancel_b, b_fin3, b_cancel_late, b_fin4;
static int busy_fn(void *a) {   /* runs inside the engine, in the owner thread */
    (void)a; b_close_in = hv_restore_session_close(); b_mp_in = 0; b_fin_in = hv_restore_session_finalize(0, 0, &b_mp_in);
    b_final_in = hv_reg_report_final(); b_state_in = hv_reg_state();
    STV(eng_in, 1); while (!LDV(eng_go)) usleep(1000); return 0;
}
static void *busy_thread(void *a) {
    int rc, mp;
    STV(b_claim, hv_restore_session_claim((uint64_t)(uintptr_t)a));
    if (LDV(b_claim) != 0) { STV(eng_in, 1); return NULL; }
    hv_restore_session_run(busy_fn, NULL, &rc);
    b_fin1 = hv_restore_session_finalize(0, 0, &mp); b_mp1 = mp; b_pub1 = hv_reg_pub_state();
    b_rel_early = hv_restore_session_release_idle(); b_state_early = hv_reg_state();
    b_fin2 = hv_restore_session_finalize(0, 0, &mp); b_mp2 = mp;
    b_pub_a = hv_restore_session_publish(NULL, NULL); b_pub_b = hv_restore_session_publish(NULL, NULL);
    b_rel_ok = hv_restore_session_release_idle(); b_state_idle = hv_reg_state();
    hv_restore_session_begin();
    b_cancel_a = hv_restore_session_cancel(); b_cancel_b = hv_restore_session_cancel();
    b_fin3 = hv_restore_session_finalize(1, 0, &mp);
    hv_restore_session_publish(NULL, NULL); hv_restore_session_release_idle();
    hv_restore_session_begin();
    int rc2; hv_restore_session_run(engine, NULL, &rc2);
    b_cancel_late = hv_restore_session_cancel();
    b_fin4 = hv_restore_session_finalize(1, 0, &mp);
    hv_restore_session_publish(NULL, NULL); hv_restore_session_release_idle();
    return NULL;
}
static int hang_go, hang_in, xfer_go;
static volatile int stranger_close, stranger_fin, stranger_rel, stranger_end, stranger_pub;
static uint64_t xfer_arg;
static volatile int xfer_rc, xfer_rcs[2], cancel_from_other;
static volatile int cb_begin, cb_cancel, cb_rel, cb_pub, cb_close, cb_end, cb_abort, cb_claim, cb_fin, cb_fin_mp, cb_state_during, cb_pubstate_during;
static volatile uint64_t cb_appr;
static void *stranger(void *a) { (void)a; int mp; stranger_close = hv_restore_session_close(); stranger_fin = hv_restore_session_finalize(0, 0, &mp); stranger_rel = hv_restore_session_release_idle(); stranger_end = hv_restore_session_end(); stranger_pub = hv_restore_session_publish(NULL, NULL); return NULL; }
static void *claimer(void *a) { (void)a; xfer_rc = hv_restore_session_claim(xfer_arg); return NULL; }
static void end_as_owner(void) { int mp; hv_restore_session_finalize(0, 0, &mp); hv_restore_session_publish(NULL, NULL); hv_restore_session_release_idle(); }
static void *claimer_slot0(void *a) { (void)a; xfer_rcs[0] = hv_restore_session_claim(xfer_arg); if (xfer_rcs[0] == 0) end_as_owner(); return NULL; }
static void *claimer_slot1(void *a) { (void)a; xfer_rcs[1] = hv_restore_session_claim(xfer_arg); if (xfer_rcs[1] == 0) end_as_owner(); return NULL; }
static int xfer_end_ok;
static void *claimer_after_go(void *a) { (void)a; while (!LDV(xfer_go)) ; xfer_rc = hv_restore_session_claim(xfer_arg); if (xfer_rc == 0) { while (!LDV(xfer_end_ok)) usleep(100); end_as_owner(); } return NULL; }
static void *ender(void *a) { (void)a; end_as_owner(); return NULL; }
static void *canceller(void *a) { (void)a; cancel_from_other = hv_restore_session_cancel(); return NULL; }
static void reentry_cb(void *a) {
    (void)a; int mp = -1;
    cb_state_during = hv_reg_state(); cb_pubstate_during = hv_reg_pub_state();
    cb_begin = hv_restore_session_begin(); cb_cancel = hv_restore_session_cancel(); cb_rel = hv_restore_session_release_idle();
    cb_pub = hv_restore_session_publish(NULL, NULL); cb_close = hv_restore_session_close(); cb_end = hv_restore_session_end();
    cb_abort = hv_restore_session_abort_start(); cb_claim = hv_restore_session_claim(1); cb_appr = hv_restore_session_approve_successor();
    cb_fin = hv_restore_session_finalize(0, 0, &mp); cb_fin_mp = mp;
}
static void hang_cb(void *a) { (void)a; STV(hang_in, 1); while (!LDV(hang_go)) usleep(1000); }
static void *owner_hang_thread(void *a) {
    (void)a; int mp;
    if (hv_restore_session_claim(xfer_arg) != 0) return NULL;
    hv_restore_session_finalize(0, 0, &mp);
    hv_restore_session_publish(hang_cb, NULL);
    hv_restore_session_release_idle();
    return NULL;
}
static void *task(void *a) { usleep((int)(long)a * 1000); hv_reg_complete(gen, HV_REG_K_ASYNC, 0); return NULL; }

static volatile int ar_claimed, ar_ran, ar_end, ar_go;
static uint64_t ar_tok;
static void *ar_thread(void *a) {
    (void)a; int rc; STV(ar_claimed, 0); STV(ar_ran, 0); STV(ar_end, -9);
    while (!LDV(ar_go)) ;   /* both sides start together */
    if (hv_restore_session_claim(ar_tok) != 0) return NULL;
    STV(ar_claimed, 1);
    STV(ar_ran, hv_restore_session_run(engine, NULL, &rc) == 0);
    STV(ar_end, hv_restore_session_end());
    return NULL;
}
static void *pr_stranger(void *a) { int *o = a; o[0] = hv_restore_session_publish(NULL, NULL); o[1] = hv_restore_session_release_idle(); int mp; o[2] = hv_restore_session_finalize(0, 0, &mp); o[3] = mp; return NULL; }

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
    } else if (!strcmp(c, "life")) {   /* the vendored-side interface (hv_life.h) is bound to the registry for exactly one session */
        uint64_t tg = 99, cg = 99; int fdp[2];
        CHECK(hv_life_task_begin(HV_LIFE_K_ASYNC, &tg) == 0 && tg == 0 && hv_life_conn_add(-5, &cg) == 0 && hv_life_conn_remove(0, -5) == 0 && hv_life_terminate_requested() == 0, "no session: every helper allows and does nothing");
        CHECK(hv_restore_session_begin() == 0, "begin");
        CHECK(hv_life_task_begin(HV_LIFE_K_ASYNC, &tg) == 0 && tg == hv_reg_generation() && hv_reg_active(HV_REG_K_ASYNC) == 1, "a task registers through the interface");
        hv_life_task_end(tg, HV_LIFE_K_ASYNC, 0);
        CHECK(hv_reg_active(HV_REG_K_ASYNC) == 0, "its completion is counted");
        CHECK(hv_life_task_begin(HV_LIFE_K_PROXY_CONN, &tg) == 0, "register");
        hv_life_task_create_failed(tg, HV_LIFE_K_PROXY_CONN, 5);
        CHECK(hv_reg_active(HV_REG_K_PROXY_CONN) == 0, "a failed thread creation releases the registration");
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fdp) == 0 && hv_life_conn_add(fdp[0], &cg) == 0 && hv_reg_conn_count() == 1, "a connection registers");
        CHECK(hv_life_conn_remove(cg, fdp[0]) == 0 && hv_reg_conn_count() == 0, "unregister: no reference is out, close is allowed");
        CHECK(hv_restore_session_cancel() == 1 && hv_life_terminate_requested() == 1, "cancel is accepted and the terminate flag is visible through the interface");
        close(fdp[0]); close(fdp[1]);
        hv_restore_session_close();
        hv_restore_session_end();
    } else if (!strcmp(c, "life_cancel")) {   /* cancel wakes a blocked recv on a registered connection; unregister waits for the cancel's reference */
        int sp[2]; uint64_t cg;
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "pair");
        hv_restore_session_begin();
        CHECK(hv_life_conn_add(sp[0], &cg) == 0, "register");
        pthread_t t; pthread_create(&t, NULL, life_recv, (void *)(long)sp[0]);
        usleep(200000);
        CHECK(LDV(lr_state) == 1, "the receiver is blocked before the cancel");
        CHECK(hv_restore_session_cancel() == 1, "cancel");
        pthread_join(t, NULL);
        CHECK(LDV(lr_state) == 2, "the blocked recv returned after cancel (shutdown)");
        CHECK(hv_life_conn_remove(cg, sp[0]) == 0, "the owner may close after the references came back");
        close(sp[0]); close(sp[1]);
        hv_restore_session_close(); hv_restore_session_end();
        CHECK(hv_life_task_begin(HV_LIFE_K_ASYNC, &cg) == 0 && cg == 0, "after end the interface is detached again");
    } else if (!strcmp(c, "stuck")) {
        hv_restore_session_begin(); gen = hv_reg_generation();
        hv_reg_try_register(gen, HV_REG_K_PROXY_CONN);   /* never completes */
        CHECK(hv_restore_session_end() == -1 && hv_restore_session_quiescence_failed(), "end: a stuck task -> QUIESCENCE_FAILED after the deadline, end refused");
        CHECK(hv_restore_session_release_permitted() == 0, "release denied");
        CHECK(hv_net_guard_hooks_intact(), "the connection policy stays in force");
        CHECK(hv_restore_session_begin() == -1, "begin refused");
        hv_reg_complete(gen, HV_REG_K_PROXY_CONN, 0);
        CHECK(hv_restore_session_end() == -1 && hv_restore_session_quiescence_failed(), "a late completion does not lift the terminal state");
    } else if (!strcmp(c, "busy")) {   /* C13: the owner decides after the engine returned; an engine still inside run() blocks it. The engine thread is the owner (claim) */
        hv_restore_session_begin();
        uint64_t tok = hv_restore_session_approve_successor();
        pthread_t t; pthread_create(&t, NULL, busy_thread, (void *)(uintptr_t)tok);
        while (!LDV(eng_in)) usleep(1000);
        CHECK(LDV(b_claim) == 0, "the restore thread claimed the right to end the session before it entered the engine");
        CHECK(hv_restore_session_close() == -1 && hv_restore_session_finalize(0, 0, &(int){0}) == (int)HV_REG_R_NONE && hv_reg_state() == HV_REG_ACTIVE, "another thread (the old owner) while the engine runs: close and finalize refused, state unchanged");
        STV(eng_go, 1); pthread_join(t, NULL);
        CHECK(b_close_in == -1 && b_fin_in == (int)HV_REG_R_NONE && b_mp_in == 0 && b_final_in == HV_REG_R_NONE && b_state_in == HV_REG_ACTIVE, "the owner, but the engine is inside run(): close refused, state unchanged, finalize decides and publishes nothing");
        CHECK(b_fin1 == (int)HV_REG_R_SUCCESS && b_mp1 == 1 && b_pub1 == HV_REG_PUB_PUBLISHING, "after it returned: SUCCESS, the right to publish is issued");
        CHECK(b_rel_early == -1 && b_state_early == HV_REG_CLOSED, "release_idle before publish: refused");
        CHECK(b_fin2 == (int)HV_REG_R_SUCCESS && b_mp2 == 0, "a second finalize returns the recorded result and NO new right to publish");
        CHECK(b_pub_a == 0 && b_pub_b == -1, "publish once; a second publish is refused (the right is consumed)");
        CHECK(b_rel_ok == 0 && b_state_idle == HV_REG_IDLE, "release_idle after publish: IDLE");
        CHECK(b_cancel_a == 1 && b_cancel_b == 0, "cancel: accepted once, then 'already'");
        CHECK(b_fin3 == (int)HV_REG_R_ABORTED_BY_USER, "a failing engine after an accepted cancel: ABORTED_BY_USER (history decides)");
        CHECK(b_cancel_late == -1, "cancel after the engine returned: refused (completion first)");
        CHECK(b_fin4 == (int)HV_REG_R_RESTORE_FAILED, "...so a failure is RESTORE_FAILED, not a user cancel");
    } else if (!strcmp(c, "abort_race")) {   /* abort_start() against claim()+run(): one g_own_mu serializes them; after abandon succeeded uninstall can never fail */
        int abort_won = 0, claim_won = 0, bad = 0;
        for (int i = 0; i < 400; i++) {
            if (hv_restore_session_begin() != 0) { bad++; break; }
            ar_tok = hv_restore_session_approve_successor();
            STV(ar_go, 0); pthread_t t; pthread_create(&t, NULL, ar_thread, NULL);
            STV(ar_go, 1);
            if (i % 4) { for (volatile int k = 0; k < (i % 16) * 400; k++) ; }
            int ab = hv_restore_session_abort_start();
            pthread_join(t, NULL);
            /* abort_start() also answers 0 when the generation is already gone (the claimed thread finished first), so the winner is told by whether the claim happened */
            if (!LDV(ar_claimed)) {
                abort_won++;
                if (ab != 0) bad++;
                if (LDV(ar_claimed) || hv_reg_state() != HV_REG_IDLE || hv_net_guard_active() || hv_net_guard_engine_count() != 0) { bad++; if (bad < 4) printf("INFO abort-won round %d: claimed %d state %d active %d eng %d\n", i, LDV(ar_claimed), (int)hv_reg_state(), hv_net_guard_active(), hv_net_guard_engine_count()); }   /* abandoned: nothing left, no policy, no engine */
            } else {
                claim_won++;
                if (!LDV(ar_claimed) || !LDV(ar_ran) || LDV(ar_end) != 0 || hv_reg_state() != HV_REG_IDLE || hv_net_guard_active()) { bad++; if (bad < 4) printf("INFO round %d: claimed %d ran %d end %d state %d active %d\n", i, LDV(ar_claimed), LDV(ar_ran), LDV(ar_end), (int)hv_reg_state(), hv_net_guard_active()); }   /* the claim came first: the thread ran and ended it */
            }
        }
        CHECK(bad == 0, "400 rounds abort_start vs claim+run: exactly one wins, never a leftover policy, engine or non-IDLE state ");
        printf("INFO abort won %d, claim won %d\n", abort_won, claim_won);
        CHECK(hv_restore_session_abort_uninstall_failures() == 0, "the 'cannot happen' uninstall failure in abort_start was never reached");
        CHECK(abort_won > 0 && claim_won > 0, "both orders happened");
    } else if (!strcmp(c, "pubrights")) {   /* the publish right: one per generation, held by the owner; nobody else, nothing early, nothing twice */
        hv_restore_session_begin();
        int o[4] = {0}; pthread_t t; int mp;
        pthread_create(&t, NULL, pr_stranger, o); pthread_join(t, NULL);
        CHECK(o[0] == -1 && o[1] == -1 && o[2] == (int)HV_REG_R_NONE && o[3] == 0, "before finalize: another thread can neither publish, release nor finalize (no right issued)");
        CHECK(hv_restore_session_publish(NULL, NULL) == -1, "the owner cannot publish before finalize either");
        CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_SUCCESS && mp == 1, "finalize issues the right once");
        CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_SUCCESS && mp == 0, "a second finalize issues none");
        pthread_create(&t, NULL, pr_stranger, o); pthread_join(t, NULL);
        CHECK(o[0] == -1 && o[1] == -1 && hv_reg_pub_state() == HV_REG_PUB_PUBLISHING, "the right exists: another thread still cannot publish or release, the state is unchanged");
        CHECK(hv_restore_session_release_idle() == -1, "release before publish: refused");
        CHECK(hv_restore_session_publish(NULL, NULL) == 0 && hv_reg_pub_state() == HV_REG_PUB_PUBLISHED, "the owner publishes");
        CHECK(hv_restore_session_publish(NULL, NULL) == -1, "a second publish is refused");
        CHECK(hv_restore_session_release_idle() == 0 && hv_reg_state() == HV_REG_IDLE, "release_idle: IDLE");
        hv_restore_session_begin();
        CHECK(hv_restore_session_publish(NULL, NULL) == -1, "a new generation starts without any right (the old one did not carry over)");
        hv_restore_session_finalize(0, 0, &mp); hv_restore_session_publish(NULL, NULL); hv_restore_session_release_idle();
    } else if (!strcmp(c, "owner")) {
        hv_restore_session_begin();
        pthread_t t; pthread_create(&t, NULL, stranger, NULL); pthread_join(t, NULL);
        CHECK(stranger_close == -1 && stranger_fin == (int)HV_REG_R_NONE && stranger_rel == -1 && stranger_end == -1 && stranger_pub == -1, "right after begin(): another thread's close, finalize, release_idle, end, publish are all refused");
        CHECK(hv_reg_state() == HV_REG_ACTIVE, "state unchanged");
        int mp; CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_SUCCESS, "the owner (the begin() caller) can");
        pthread_create(&t, NULL, stranger, NULL); pthread_join(t, NULL);
        CHECK(stranger_pub == -1, "another thread cannot use the publish right, though it exists");
        CHECK(hv_restore_session_publish(NULL, NULL) == 0 && hv_restore_session_release_idle() == 0, "the owner publishes and releases");
        CHECK(hv_restore_session_publish(NULL, NULL) == -1, "publish without a right (no finalize in this generation): refused");
    } else if (!strcmp(c, "xfer")) {   /* hand-over to the restore thread by token */
        hv_restore_session_begin();
        CHECK(hv_restore_session_claim(0) == -1 && hv_restore_session_claim(12345) == -1, "claim without / with an unissued token: refused");
        uint64_t tok = hv_restore_session_approve_successor();
        CHECK(tok != 0, "the owner is issued a token");
        pthread_t t; xfer_arg = tok + 1; pthread_create(&t, NULL, claimer, NULL); pthread_join(t, NULL);
        tok = hv_restore_session_approve_successor();
        CHECK(xfer_rc == -1 && tok != 0, "a wrong token: refused, the owner still owns (it can issue a new token; the old one is replaced)");
        xfer_arg = tok; pthread_create(&t, NULL, claimer, NULL); pthread_join(t, NULL);
        CHECK(xfer_rc == 0, "the right token: the hand-over succeeds");
        CHECK(hv_restore_session_close() == -1 && hv_restore_session_finalize(0, 0, &(int){0}) == (int)HV_REG_R_NONE && hv_restore_session_approve_successor() == 0, "the old owner's close, finalize and approve are now refused");
        xfer_arg = tok; pthread_create(&t, NULL, claimer, NULL); pthread_join(t, NULL);
        CHECK(xfer_rc == -1, "a second claim with the consumed token: refused");
        pthread_create(&t, NULL, ender, NULL); pthread_join(t, NULL);
        CHECK(hv_reg_state() == HV_REG_IDLE, "the new owner ended the generation");
    } else if (!strcmp(c, "abort")) {
        hv_restore_session_begin();
        uint64_t tok = hv_restore_session_approve_successor();
        CHECK(hv_restore_session_abort_start() == 0 && hv_reg_state() == HV_REG_IDLE && !hv_net_guard_hooks_intact(), "abort_start by the owner before the hand-over: IDLE, policy removed");
        hv_restore_session_begin();
        pthread_t t; xfer_arg = tok; pthread_create(&t, NULL, claimer, NULL); pthread_join(t, NULL);
        CHECK(xfer_rc == -1, "the token of the aborted generation is dead in the next one");
        hv_reg_try_register(hv_reg_generation(), HV_REG_K_ASYNC);
        CHECK(hv_restore_session_abort_start() == -1 && hv_reg_state() == HV_REG_ACTIVE, "a registered task: abort_start refused, the owner keeps the session");
        hv_reg_complete(hv_reg_generation(), HV_REG_K_ASYNC, 0);
        CHECK(hv_restore_session_abort_start() == 0, "...and succeeds once the task is gone");
        hv_restore_session_begin();
        uint64_t tok2 = hv_restore_session_approve_successor();
        xfer_arg = tok2; pthread_create(&t, NULL, claimer, NULL); pthread_join(t, NULL);
        CHECK(xfer_rc == 0, "claim with the issued token: ok");
        CHECK(hv_restore_session_abort_start() == -1 && hv_restore_session_close() == -1 && hv_restore_session_approve_successor() == 0, "after the hand-over the old owner can neither abort, close nor issue tokens");
    } else if (!strcmp(c, "xfer_race")) {   /* many rounds: two claimers; and claim against the owner's close. The thread that ends up owning the generation ends it. */
        int bad = 0;
        for (int i = 0; i < 300; i++) {
            hv_restore_session_begin();
            uint64_t tok = hv_restore_session_approve_successor();
            pthread_t a2, b2; xfer_arg = tok; xfer_rcs[0] = xfer_rcs[1] = -9;
            pthread_create(&a2, NULL, claimer_slot0, NULL); pthread_create(&b2, NULL, claimer_slot1, NULL);
            pthread_join(a2, NULL); pthread_join(b2, NULL);
            if ((xfer_rcs[0] == 0) + (xfer_rcs[1] == 0) != 1) bad++;
            if (hv_reg_state() != HV_REG_IDLE) bad += 100;   /* the winner ended the generation */
        }
        CHECK(bad == 0, "300 rounds, two threads claim the same token: exactly one wins every time, and the winner owns (and ends) the generation");
        int incons = 0, claimed_n = 0;
        for (int i = 0; i < 300; i++) {
            hv_restore_session_begin();
            uint64_t tok = hv_restore_session_approve_successor();
            pthread_t a2; xfer_arg = tok; xfer_rc = -9; STV(xfer_go, 0); STV(xfer_end_ok, 0);
            pthread_create(&a2, NULL, claimer_after_go, NULL);
            STV(xfer_go, 1);
            if (i % 3) usleep(i % 7);
            int cr = hv_restore_session_close();
            STV(xfer_end_ok, 1);   /* the winner (if the claim won) may end the generation only now: main's close() has returned */
            pthread_join(a2, NULL);
            int claimed = xfer_rc == 0;
            claimed_n += claimed;
            if (claimed == (cr == 0)) incons++;   /* both or neither: impossible */
            if (!claimed) { int mp; hv_restore_session_finalize(0, 0, &mp); hv_restore_session_publish(NULL, NULL); hv_restore_session_release_idle(); }
            if (hv_reg_state() != HV_REG_IDLE) incons += 100;
        }
        CHECK(incons == 0, "300 rounds, owner close() against claim(): exactly one takes effect, never both, never neither");
        printf("INFO claim won %d of 300 rounds\n", claimed_n);
    } else if (!strcmp(c, "finish")) {
        hv_restore_session_begin(); gen = hv_reg_generation();
        int mp;
        CHECK(hv_restore_session_finalize(0, HV_REG_CL_HOST_THREAD | HV_REG_CL_POLICY, &mp) == (int)HV_REG_R_CLEANUP_FAILED && mp == 1 && hv_reg_state() == HV_REG_CLOSED, "engine ok + host thread alive + policy: CLEANUP_FAILED, stays CLOSED (state and result code are separate)");
        CHECK(hv_reg_cleanup_history() == (HV_REG_CL_HOST_THREAD | HV_REG_CL_POLICY), "both reasons accumulated (OR), none overwritten");
        CHECK(hv_restore_session_publish(NULL, NULL) == 0 && hv_restore_session_release_idle() == -1 && hv_reg_state() == HV_REG_CLOSED, "published, but release_idle is refused");
        hv_reg_cleanup_resolve(gen, HV_REG_CL_HOST_THREAD | HV_REG_CL_POLICY);
        CHECK(hv_reg_cleanup_current() == 0 && hv_reg_cleanup_history() == 3, "the resources recovered late: current 0, history kept");
        CHECK(hv_restore_session_release_idle() == -1 && hv_reg_report_final() == HV_REG_R_CLEANUP_FAILED, "...yet IDLE stays refused and the confirmed result is unchanged (C9, C15 (f))");
        CHECK(hv_net_guard_hooks_intact() && hv_restore_session_begin() == -1, "policy kept (a host thread was alive), begin refused until the app restarts");
    } else if (!strcmp(c, "pending")) {   /* an object still pending is decided INSIDE finalize: the published result can never be SUCCESS for a session that cannot go IDLE */
        hv_restore_session_begin(); gen = hv_reg_generation();
        hv_reg_objects_pending(gen);
        int mp;
        CHECK(hv_restore_session_finalize(0, 0, &mp) == (int)HV_REG_R_CLEANUP_FAILED && (hv_reg_cleanup_history() & HV_REG_CL_OBJECT), "a pending (not released) object: CLEANUP_FAILED with the OBJECT reason");
        hv_restore_session_publish(NULL, NULL);
        hv_reg_objects_released(gen);
        CHECK(hv_restore_session_release_idle() == -1 && hv_reg_report_final() == HV_REG_R_CLEANUP_FAILED, "released late: the recorded result is not changed; IDLE stays refused");
    } else if (!strcmp(c, "reentry")) {   /* what a callback can and cannot do while the result is being published */
        hv_restore_session_begin(); gen = hv_reg_generation();
        int mp;
        int rep = hv_restore_session_finalize(0, 0, &mp);
        CHECK(rep == (int)HV_REG_R_SUCCESS && mp == 1, "finalize issues the right");
        CHECK(hv_restore_session_publish(reentry_cb, NULL) == 0, "publish runs the callback and completes");
        CHECK(cb_fin == (int)HV_REG_R_SUCCESS && cb_fin_mp == 0, "inside the callback: finalize returns the recorded result only, no second right");
        CHECK(cb_begin == -1 && cb_cancel == -1 && cb_rel == -1 && cb_pub == -1 && cb_close == -1 && cb_end == -1 && cb_abort == -1 && cb_claim == -1 && cb_appr == 0, "inside the callback: begin, cancel, release_idle, publish, close, end, abort_start, claim, approve all refused");
        CHECK(cb_state_during == HV_REG_CLOSED && cb_pubstate_during == HV_REG_PUB_PUBLISHING, "state was CLOSED/PUBLISHING throughout");
        CHECK(hv_restore_session_release_idle() == 0, "after the callback returned: IDLE");
    } else if (!strcmp(c, "hang")) {   /* a callback that never returns, on the thread that owns the session after the hand-over */
        hv_restore_session_begin();
        uint64_t tok = hv_restore_session_approve_successor();
        pthread_t t; xfer_arg = tok; pthread_create(&t, NULL, owner_hang_thread, NULL);
        while (!LDV(hang_in)) usleep(1000);
        CHECK(hv_reg_state() == HV_REG_CLOSED && hv_reg_pub_state() == HV_REG_PUB_PUBLISHING && hv_restore_session_begin() == -1, "callback not returned: CLOSED, PUBLISHING, next session refused");
        STV(hang_go, 1); pthread_join(t, NULL);
        CHECK(hv_reg_state() == HV_REG_IDLE && hv_restore_session_begin() == 0, "late return: only now the next session can begin");
    } else if (!strcmp(c, "cancel_thread")) {
        hv_restore_session_begin();
        pthread_t t; pthread_create(&t, NULL, canceller, NULL); pthread_join(t, NULL);
        int mp;
        CHECK(cancel_from_other == 1, "cancel from a thread that is not the owner (the UI): accepted");
        CHECK(hv_restore_session_finalize(1, 0, &mp) == (int)HV_REG_R_ABORTED_BY_USER, "and decides the result");
        hv_restore_session_publish(NULL, NULL); hv_restore_session_release_idle();
    } else if (!strcmp(c, "engine")) {
        hv_restore_session_begin();
        CHECK(hv_restore_session_run(engine, NULL, &rc) == 0, "run");
        CHECK(hv_restore_session_end() == 0, "end with no registered task and no engine thread");
    } else { fprintf(stderr, "unknown case\n"); return 2; }
    return nfail;
}
