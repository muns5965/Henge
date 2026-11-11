/* See restore_session.h. */
#include "restore_session.h"

#include "net_guard.h"
#include "restore_registry.h"

#include <pthread.h>

#include "idevicerestore.h"

extern int hv_no_ticket_files;   /* third_party/idevicerestore/src/idevicerestore.c */

/* PROVISIONAL, not measured on a device: how long end() waits for registered tasks before it declares the session not quiescent (C19: an absolute
   CLOCK_MONOTONIC deadline). Tests shorten it. */
static int64_t g_close_ms = 15000;
static uint64_t g_gen_v;   /* generation of the session begun here; 0 = none. Set by the starting thread, cleared by the restore thread: atomic accessors only. */
#define g_gen __atomic_load_n(&g_gen_v, __ATOMIC_SEQ_CST)
#define SET_GEN(x) __atomic_store_n(&g_gen_v, (x), __ATOMIC_SEQ_CST)

/* LOCK ORDER (one rule, no exceptions): g_own_mu  ->  the registry's mutex. The registry never takes g_own_mu. Only claim(), close()'s close_begin step and
   abort_start() hold g_own_mu while calling the registry, and only for short state operations (no callback, no network call, no waiting: C12). Everything else
   releases g_own_mu before calling the registry.

   OWNER: the thread that decides how this generation ends (C13). begin() records its caller as the owner, so a session never has "no owner". Ownership moves
   ONCE, to the restore thread, by claim(token): approve_successor() (owner only) issues the token before the thread is created; claim() verifies token, generation
   and the registry state ACTIVE, swaps the owner and consumes the token in ONE g_own_mu critical section. close()'s close_begin step runs under the same g_own_mu
   together with its owner check, so "transfer authorized" and "close started" cannot both happen: whichever takes g_own_mu first wins, and the other sees the result
   (a close by the old owner after the swap is refused as not-owner; a claim after close_begin sees CLOSING and is refused). The token proves a right to ask, not the
   identity of the caller: the threat model is a wrong code path, not malicious code. "Is the engine inside run()" is net_guard's count, not part of ownership. */
static pthread_mutex_t g_own_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_owner;
static int g_owner_set;
static uint64_t g_owner_gen, g_token, g_token_seq;
static int g_transferred, g_pub_right;
static unsigned g_abort_uninstall_failed;
static unsigned g_run_deny_owner_state, g_run_deny_engine;   /* observation only: why run() refused */
static __thread int t_in_publish;   /* this thread is inside the publication callback: every state-changing public API refuses (checked FIRST in each) */

static int owner_locked(void) { return g_owner_set && g_owner_gen == g_gen && pthread_equal(g_owner, pthread_self()); }
static int owner_ok(void) { pthread_mutex_lock(&g_own_mu); int ok = owner_locked(); pthread_mutex_unlock(&g_own_mu); return ok; }
static void owner_reset_locked(void) { g_owner_set = 0; g_token = 0; g_transferred = 0; g_pub_right = 0; }

uint64_t hv_restore_session_approve_successor(void) {
    if (t_in_publish) return 0;
    uint64_t t = 0;
    pthread_mutex_lock(&g_own_mu);
    if (g_gen != 0 && owner_locked() && !g_transferred) { t = (g_gen << 20) ^ (++g_token_seq * 0x9E3779B97F4A7C15ULL); if (t == 0) t = 1; g_token = t; }
    pthread_mutex_unlock(&g_own_mu);
    return t;
}

int hv_restore_session_claim(uint64_t token) {
    if (t_in_publish || g_gen == 0 || token == 0) return -1;
    int r = -1;
    pthread_mutex_lock(&g_own_mu);
    if (g_token == token && !g_transferred && g_owner_gen == g_gen && hv_reg_state() == HV_REG_ACTIVE) {   /* own -> registry, state read only */
        g_owner = pthread_self(); g_transferred = 1; g_token = 0;   /* swap and consume: one critical section, never two owners, never none */
        r = 0;
    }
    pthread_mutex_unlock(&g_own_mu);
    return r;
}

void hv_restore_session_set_close_timeout_ms(int ms) { g_close_ms = ms > 0 ? ms : 15000; }

int hv_restore_session_begin(void) {
    if (t_in_publish) return -1;
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
    pthread_mutex_lock(&g_own_mu);
    SET_GEN(g);
    owner_reset_locked();
    g_owner = pthread_self(); g_owner_gen = g; g_owner_set = 1;   /* the starting thread owns the generation until it hands over (or ends it) */
    pthread_mutex_unlock(&g_own_mu);
    return 0;
}

void hv_restore_session_allow_usbmuxd(uint16_t port) { hv_net_guard_allow_local("127.0.0.1", port); }

int hv_restore_session_run(int (*fn)(void *), void *arg, int *rc) {
    if (!fn || g_gen == 0) return -1;
    /* Entry is ONE g_own_mu section: owner AND ACTIVE AND the engine registration. abort_start() takes the same lock, so either the engine is already counted (abort
       refuses) or the abort came first (this refuses). The engine itself runs outside the lock. g_own_mu -> net_guard mutex is a new edge; net_guard never takes g_own_mu. */
    pthread_mutex_lock(&g_own_mu);
    int allowed = owner_locked() && hv_reg_state() == HV_REG_ACTIVE;
    if (!allowed) { __atomic_add_fetch(&g_run_deny_owner_state, 1, __ATOMIC_RELAXED); }
    else if (hv_net_guard_engine_enter() != 0) { allowed = 0; __atomic_add_fetch(&g_run_deny_engine, 1, __ATOMIC_RELAXED); }
    pthread_mutex_unlock(&g_own_mu);
    if (!allowed) return -1;
    int r = fn(arg);
    hv_reg_restore_returned(g_gen);   /* from here a cancel request is refused: the completion came first */
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
    if (t_in_publish || g_gen == 0) return t_in_publish ? -1 : 0;
    /* C13: the right to decide CLOSED belongs to the owner, which calls this AFTER the tool returned. While an engine thread is still registered (net_guard's own
       count -- the single record of "the engine is running") nothing is decided and nothing changes. The owner check and close_begin are one g_own_mu section. */
    pthread_mutex_lock(&g_own_mu);
    uint64_t g = g_gen;
    int ok = owner_locked() && hv_net_guard_engine_count() == 0;
    int already = ok && hv_reg_state() == HV_REG_CLOSED;
    int began = ok && !already && hv_reg_close_begin(g) == 0;
    pthread_mutex_unlock(&g_own_mu);
    if (already) return 0;
    if (!began) return -1;
    return hv_reg_wait_and_close(g, hv_reg_mono_ns() + g_close_ms * 1000000LL) == 0 ? 0 : -1;   /* waits OUTSIDE g_own_mu; QUIESCENCE_FAILED: nothing is released, the policy stays */
}

int hv_restore_session_end(void) {
    if (t_in_publish) return -1;
    if (g_gen == 0) return hv_net_guard_uninstall();   /* no session was begun here (legacy callers) */
    if (!owner_ok()) return -1;
    uint64_t g = g_gen;
    if (hv_restore_session_close() != 0) return -1;
    if (hv_net_guard_uninstall() != 0) return -1;   /* an engine thread is still registered: retry later */
    if (hv_reg_finish_session(g) != 0) return -1;   /* cleanup failure or objects not released: stays CLOSED */
    pthread_mutex_lock(&g_own_mu); owner_reset_locked(); SET_GEN(0); pthread_mutex_unlock(&g_own_mu);
    return 0;
}

int hv_restore_session_abort_start(void) {
    /* start failed before the restore thread took over: the starting thread (still the owner, no transfer) returns the session to IDLE. The registry's abandon (ACTIVE,
       nothing registered, no failure) decides; only after it succeeded are token and ownership dropped. If it refuses, the owner keeps the session. */
    if (t_in_publish || g_gen == 0) return t_in_publish ? -1 : 0;
    int r = -1;
    pthread_mutex_lock(&g_own_mu);
    if (owner_locked() && !g_transferred && hv_net_guard_engine_count() == 0 && hv_reg_abandon(g_gen) == 0) {
        /* uninstall fails only while an engine thread is registered; run() enters under this same lock, so after the count check above it cannot. The check is a
           defensive assertion: abandon already made the registry IDLE and that is not undone, so reaching the failure branch is a contract violation, not a path. */
        if (hv_net_guard_uninstall() != 0) { g_abort_uninstall_failed++; }
        owner_reset_locked(); SET_GEN(0);
        r = 0;
    }
    pthread_mutex_unlock(&g_own_mu);
    return r;
}

int hv_restore_session_fail(int why) {   /* a resource the host owns (the private usbmuxd) did not stop: the session is not quiescent; nothing is released */
    if (g_gen == 0) return -1;
    return hv_reg_declare_failed(g_gen, (HvRegQfReason)why) == 1 ? 0 : -1;
}

int hv_restore_session_cancel(void) {
    /* Host-only path: no cancel task exists, so only the HISTORY is recorded (accepted before the restore returned, once per session) and the terminate flag set.
       Nothing is counted, so a request can never block the close; whether the work actually stopped is the registered tasks' business (I3~I6, not approved). */
    if (t_in_publish || g_gen == 0) return -1;   /* from inside the publication callback: refused; from any OTHER thread (the UI) it is the normal path */
    int r = hv_reg_cancel_note(g_gen);
    if (r == 1) hv_reg_request_terminate(g_gen);
    return r;   /* 1 accepted now, 0 already accepted, -1 refused (no session, closed, or the completion came first) */
}

int hv_restore_session_finalize(int restore_rc, int cleanup_mask, int *may_publish) {
    if (may_publish) *may_publish = 0;
    if (t_in_publish) return g_gen != 0 && owner_ok() ? (int)hv_reg_report_final() : (int)HV_REG_R_NONE;   /* from inside the callback: the recorded result, read only; never a second right */
    if (g_gen == 0 || !owner_ok()) return (int)HV_REG_R_NONE;
    uint64_t g = g_gen;
    int qf = 0;
    if (hv_restore_session_close() != 0) {
        if (hv_reg_state() != HV_REG_QUIESCENCE_FAILED) return (int)HV_REG_R_NONE;   /* refused (engine still inside run): nothing decided */
        qf = 1;
    }
    if (!qf) {
        if (cleanup_mask & HV_REG_CL_HOST_THREAD) { /* a host-owned thread is alive: the policy stays too, and nothing it depends on is released */ }
        else if (hv_net_guard_uninstall() != 0) cleanup_mask |= HV_REG_CL_POLICY;   /* the policy could not be removed */
    }
    int mp = 0;
    HvRegReport rep = HV_REG_R_NONE;
    if (hv_reg_finalize(g, restore_rc == 0 ? HV_REG_RESTORE_OK : HV_REG_RESTORE_FAILED, cleanup_mask, &rep, &mp) != 0) return (int)HV_REG_R_NONE;
    if (mp) { pthread_mutex_lock(&g_own_mu); g_pub_right = 1; pthread_mutex_unlock(&g_own_mu); }   /* the one right to publish, kept by the session */
    if (may_publish) *may_publish = mp;
    return (int)rep;
}

int hv_restore_session_publish(void (*fn)(void *), void *arg) {
    /* The ONLY path that runs the callback and marks the generation PUBLISHED. It needs the one-time right that finalize issued (consumed here), the owner, and
       a thread that is not already inside a publication. in_publish is set around the callback and cleared right after it returned; no public function can clear
       it, so a callback cannot end the publication early. A callback that never returns leaves the generation PUBLISHING: the next start is refused. */
    if (t_in_publish || g_gen == 0) return -1;
    pthread_mutex_lock(&g_own_mu);
    int ok = owner_locked() && g_pub_right;
    if (ok) g_pub_right = 0;
    pthread_mutex_unlock(&g_own_mu);
    if (!ok) return -1;
    t_in_publish = 1;
    if (fn) fn(arg);
    t_in_publish = 0;
    return hv_reg_publish_done(g_gen) == 0 ? 0 : -1;
}

int hv_restore_session_release_idle(void) {
    if (t_in_publish || g_gen == 0) return -1;
    int r = -1;
    pthread_mutex_lock(&g_own_mu);
    int ok = owner_locked();
    pthread_mutex_unlock(&g_own_mu);
    if (ok && hv_reg_release_idle(g_gen) == 0) {   /* PUBLISHED, nothing unresolved now, no confirmed CLEANUP_FAILED */
        pthread_mutex_lock(&g_own_mu); owner_reset_locked(); SET_GEN(0); pthread_mutex_unlock(&g_own_mu);
        r = 0;
    }
    return r;
}

int hv_restore_session_release_permitted(void) { return g_gen != 0 && hv_reg_release_permitted(g_gen) == 1; }   /* C18: ask before client_free/end */
void hv_restore_session_request_cancel(void) { if (g_gen) hv_reg_request_terminate(g_gen); }
int hv_restore_session_quiescence_failed(void) { return hv_reg_state() == HV_REG_QUIESCENCE_FAILED; }
int hv_restore_session_engine_threads(void) { return hv_net_guard_engine_count(); }

/* observation only (tests): counters of refused run() entries and of the "cannot happen" uninstall failure in abort_start() */
unsigned hv_restore_session_run_denied_owner_state(void) { return __atomic_load_n(&g_run_deny_owner_state, __ATOMIC_RELAXED); }
unsigned hv_restore_session_run_denied_engine(void) { return __atomic_load_n(&g_run_deny_engine, __ATOMIC_RELAXED); }
unsigned hv_restore_session_abort_uninstall_failures(void) { return g_abort_uninstall_failed; }
