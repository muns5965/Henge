/* See restore_session.h. */
#include "restore_session.h"

#include "net_guard.h"
#include "restore_registry.h"

#include "idevicerestore.h"

extern int hv_no_ticket_files;   /* third_party/idevicerestore/src/idevicerestore.c */

/* PROVISIONAL, not measured on a device: how long end() waits for registered tasks before it declares the session not quiescent (C19: an absolute
   CLOCK_MONOTONIC deadline). Tests shorten it. */
static int64_t g_close_ms = 15000;
static uint64_t g_gen_v;   /* generation of the session begun here; 0 = none. Set by the starting thread, cleared by the restore thread: atomic accessors only. */
#define g_gen __atomic_load_n(&g_gen_v, __ATOMIC_SEQ_CST)
#define SET_GEN(x) __atomic_store_n(&g_gen_v, (x), __ATOMIC_SEQ_CST)

void hv_restore_session_set_close_timeout_ms(int ms) { g_close_ms = ms > 0 ? ms : 15000; }

int hv_restore_session_begin(void) {
    /* a session that was begun and not ended is NOT replaced (design C15: IDLE only after a clean close); this refuses where it used to reset silently */
    if (g_gen != 0) return -1;
    /* resetting the lists under a running engine thread would change the rules it is subject to; an earlier run that did not end is finished first */
    if (hv_net_guard_engine_count() > 0) return -1;
    hv_net_guard_reset();
    hv_net_guard_install();
    hv_no_ticket_files = 1;
    if (!hv_net_guard_hooks_intact()) return -1;
    uint64_t g = hv_reg_begin();
    if (g == 0) return -1;
    SET_GEN(g);
    return 0;
}

void hv_restore_session_allow_usbmuxd(uint16_t port) { hv_net_guard_allow_local("127.0.0.1", port); }

int hv_restore_session_run(int (*fn)(void *), void *arg, int *rc) {
    if (!fn || g_gen == 0 || hv_reg_state() != HV_REG_ACTIVE || hv_net_guard_engine_enter() != 0) return -1;
    int r = fn(arg);
    hv_net_guard_engine_leave();
    if (rc) *rc = r;
    return 0;
}

static int start_client(void *c) { return idevicerestore_start((struct idevicerestore_client_t *)c); }

int hv_restore_session_run_client(struct idevicerestore_client_t *c, int *rc) {
    if (!c) return -1;
    return hv_restore_session_run(start_client, c, rc);
}

int hv_restore_session_close(void) {
    if (g_gen == 0) return 0;
    uint64_t g = g_gen;
    if (hv_reg_state() == HV_REG_CLOSED) return 0;
    /* C13: the right to decide CLOSED belongs to the engine thread, which calls this AFTER the tool returned. While an engine thread is still registered
       (net_guard's own count -- the single record of "the engine is running"; the registry does not keep a second one) nothing is decided and nothing changes. */
    if (hv_net_guard_engine_count() > 0) return -1;
    if (hv_reg_close_begin(g) != 0) return -1;
    return hv_reg_wait_and_close(g, hv_reg_mono_ns() + g_close_ms * 1000000LL) == 0 ? 0 : -1;   /* QUIESCENCE_FAILED: nothing is released, the policy stays */
}

int hv_restore_session_end(void) {
    if (g_gen == 0) return hv_net_guard_uninstall();   /* no session was begun here (legacy callers) */
    uint64_t g = g_gen;
    if (hv_restore_session_close() != 0) return -1;
    if (hv_net_guard_uninstall() != 0) return -1;   /* an engine thread is still registered: retry later */
    if (hv_reg_finish_session(g) != 0) return -1;   /* cleanup failure or objects not released: stays CLOSED */
    SET_GEN(0);
    return 0;
}

int hv_restore_session_abort_start(void) {
    /* start failed before any task could have used the session: back to IDLE through the registry's own rule (ACTIVE, nothing registered, no failure) */
    if (g_gen == 0) return 0;
    if (hv_net_guard_engine_count() > 0 || hv_reg_abandon(g_gen) != 0) return -1;
    if (hv_net_guard_uninstall() != 0) return -1;   /* cannot happen with no engine thread; if it does the session stays and begin stays refused */
    SET_GEN(0);
    return 0;
}

int hv_restore_session_fail(int why) {   /* a resource the host owns (the private usbmuxd) did not stop: the session is not quiescent; nothing is released */
    if (g_gen == 0) return -1;
    return hv_reg_declare_failed(g_gen, (HvRegQfReason)why) == 1 ? 0 : -1;
}

int hv_restore_session_cancel(void) {
    /* A user cancel, in the host's own terms: accepted once per session (C17), the terminate flag set. No thread is created here, so the reservation is
       started and completed inline; the vendored tasks that react to the flag are not registered yet (I3~I6, not approved). */
    if (g_gen == 0) return -1;
    int r = hv_reg_cancel_request(g_gen);
    if (r == 1) { hv_reg_cancel_started(g_gen); hv_reg_request_terminate(g_gen); hv_reg_cancel_done(g_gen, 1); }
    return r;   /* 1 started now, 0 already started, -1 refused (no session or CLOSED) */
}

int hv_restore_session_finish(int restore_rc, int user_cancel, int extra_cleanup_failed) {
    if (g_gen == 0) return (int)HV_REG_R_NONE;
    uint64_t g = g_gen;
    int qf = 0;
    if (hv_restore_session_close() != 0) {
        if (hv_reg_state() != HV_REG_QUIESCENCE_FAILED) return (int)HV_REG_R_NONE;   /* refused (engine still registered): nothing decided, nothing reported */
        qf = 1;
    }
    if (!qf) {
        if (extra_cleanup_failed) hv_reg_cleanup_failed(g);   /* a host-owned resource is still alive: the policy stays too */
        else if (hv_net_guard_uninstall() != 0) hv_reg_cleanup_failed(g);   /* the policy could not be removed */
    }
    HvRegRestoreOutcome ro = restore_rc == 0 ? HV_REG_RESTORE_OK : (user_cancel ? HV_REG_RESTORE_USER_CANCEL : HV_REG_RESTORE_FAILED);
    HvRegCleanupOutcome co = (!qf && hv_reg_cleanup_failed_flag()) ? HV_REG_CLEANUP_FAILED : HV_REG_CLEANUP_DONE;
    HvRegReport rep = hv_reg_compose(ro, co, qf);   /* decided only now, after the cleanup result is known (C9) */
    hv_reg_report(g, rep);                          /* once per generation */
    if (!qf && hv_reg_finish_session(g) == 0) SET_GEN(0);
    return (int)rep;
}

int hv_restore_session_release_permitted(void) { return g_gen != 0 && hv_reg_release_permitted(g_gen) == 1; }   /* C18: ask before client_free/end */
void hv_restore_session_request_cancel(void) { if (g_gen) hv_reg_request_terminate(g_gen); }
int hv_restore_session_quiescence_failed(void) { return hv_reg_state() == HV_REG_QUIESCENCE_FAILED; }
int hv_restore_session_engine_threads(void) { return hv_net_guard_engine_count(); }
