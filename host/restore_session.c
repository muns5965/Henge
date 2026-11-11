/* See restore_session.h. */
#include "restore_session.h"

#include "net_guard.h"
#include "restore_registry.h"

#include "idevicerestore.h"

extern int hv_no_ticket_files;   /* third_party/idevicerestore/src/idevicerestore.c */

/* PROVISIONAL, not measured on a device: how long end() waits for registered tasks before it declares the session not quiescent (C19: an absolute
   CLOCK_MONOTONIC deadline). Tests shorten it. */
static int64_t g_close_ms = 15000;
static uint64_t g_gen;   /* generation of the session begun here; 0 = none */

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
    g_gen = g;
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
    if (hv_reg_close_begin(g) != 0) return -1;
    return hv_reg_wait_and_close(g, hv_reg_mono_ns() + g_close_ms * 1000000LL) == 0 ? 0 : -1;   /* QUIESCENCE_FAILED: nothing is released, the policy stays */
}

int hv_restore_session_end(void) {
    if (g_gen == 0) return hv_net_guard_uninstall();   /* no session was begun here (legacy callers) */
    uint64_t g = g_gen;
    if (hv_restore_session_close() != 0) return -1;
    if (hv_net_guard_uninstall() != 0) return -1;   /* an engine thread is still registered: retry later */
    if (hv_reg_finish_session(g) != 0) return -1;   /* cleanup failure or objects not released: stays CLOSED */
    g_gen = 0;
    return 0;
}

int hv_restore_session_release_permitted(void) { return g_gen != 0 && hv_reg_release_permitted(g_gen) == 1; }   /* C18: ask before client_free/end */
void hv_restore_session_request_cancel(void) { if (g_gen) hv_reg_request_terminate(g_gen); }
int hv_restore_session_quiescence_failed(void) { return hv_reg_state() == HV_REG_QUIESCENCE_FAILED; }
int hv_restore_session_engine_threads(void) { return hv_net_guard_engine_count(); }
