/* See restore_registry.h. */
#define _GNU_SOURCE 1
#include "restore_registry.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define MAX_CONN 32
#define CANCEL_RETRIES 1   /* one retry of a cancel whose thread could not be created, granted to ONE requester (C17) */

typedef struct { int fd, refs, registered, used; } ConnItem;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    HvRegState state;
    HvRegPhase phase;
    uint64_t gen, seq;
    int active[HV_REG_K_N], refused[HV_REG_K_N], hard_failures;
    HvRegQfReason qf, hard_reason;
    int err_have, err_count;
    HvRegErrSource err_src;
    int64_t err_code;
    HvRegCancelState cs;
    int cancel_retry_left, cancel_rejected_closed;
    ConnItem conns[MAX_CONN];
    int objects_pending, objects_released, cleanup_failed, release_granted;
    HvRegReport report_final, report_interim;
    int stale;
    int64_t deadline_ns;
    volatile int terminate;
} R = {.mu = PTHREAD_MUTEX_INITIALIZER};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init_once(void) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);   /* C19: the same clock as every deadline */
    pthread_cond_init(&R.cv, &a);
    pthread_condattr_destroy(&a);
}
static void lock(void) { pthread_once(&g_once, init_once); pthread_mutex_lock(&R.mu); }
static void unlock(void) { pthread_mutex_unlock(&R.mu); }
static void changed(void) { R.seq++; pthread_cond_broadcast(&R.cv); }

int64_t hv_reg_mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int wait_until(int64_t deadline_ns) {   /* mutex held; 0 = woken (or spurious), 1 = the deadline passed */
    struct timespec ts = {.tv_sec = (time_t)(deadline_ns / 1000000000LL), .tv_nsec = (long)(deadline_ns % 1000000000LL)};
    return pthread_cond_timedwait(&R.cv, &R.mu, &ts) == ETIMEDOUT;
}

#ifdef HV_TEST_HOOKS
#define HOOK_MAX 256
static HvHookRec g_hooks[HOOK_MAX];
static int g_nhooks;
static uint64_t g_hook_order;
static void hook_record(int id, int before, int result) {   /* mutex held; observation only: one fixed-size record, nothing else */
    if (g_nhooks >= HOOK_MAX) return;
    HvHookRec *h = &g_hooks[g_nhooks++];
    h->order = ++g_hook_order; h->id = id; h->state_before = before; h->state_after = (int)R.state; h->result = result;
    for (int i = 0; i < HV_REG_K_N; i++) h->active[i] = R.active[i];
}
void hv_test_hook_reset(void) { lock(); g_nhooks = 0; g_hook_order = 0; unlock(); }
int hv_test_hook_log(HvHookRec *out, int max) { lock(); int n = g_nhooks < max ? g_nhooks : max; memcpy(out, g_hooks, (size_t)n * sizeof *out); unlock(); return n; }
#define HV_TEST_HOOK(id, before, result) hook_record((id), (before), (result))
#else
#define HV_TEST_HOOK(id, before, result) ((void)(before), (void)(result))
#endif

static int stale_locked(uint64_t gen) {   /* a call from another generation is refused and only counted (C15) */
    if (gen != R.gen || gen == 0) { R.stale++; return 1; }
    return 0;
}
static int total_active(void) { int n = 0; for (int i = 0; i < HV_REG_K_N; i++) n += R.active[i]; return n; }
static void record_error_locked(HvRegErrSource src, int64_t code) {
    R.err_count++;
    if (!R.err_have) { R.err_have = 1; R.err_src = src; R.err_code = code; }
}

uint64_t hv_reg_begin(void) {
    lock();
    if (R.state != HV_REG_IDLE) { unlock(); return 0; }
    R.gen++;
    memset(R.active, 0, sizeof R.active); memset(R.refused, 0, sizeof R.refused);
    memset(R.conns, 0, sizeof R.conns);
    R.hard_failures = 0; R.hard_reason = HV_REG_QF_NONE; R.qf = HV_REG_QF_NONE; R.err_have = 0; R.err_count = 0; R.err_src = HV_REG_E_NONE; R.err_code = 0;
    R.cs = HV_REG_CS_NONE; R.cancel_retry_left = CANCEL_RETRIES; R.cancel_rejected_closed = 0;
    R.objects_pending = R.objects_released = R.cleanup_failed = R.release_granted = 0;
    R.report_final = R.report_interim = HV_REG_R_NONE;
    R.phase = HV_REG_PHASE_RUN; R.deadline_ns = 0; R.terminate = 0;
    R.state = HV_REG_ACTIVE;
    uint64_t g = R.gen;
    changed();
    unlock();
    return g;
}
int hv_reg_abandon(uint64_t gen) {
    lock();
    int r = -1;
    if (!stale_locked(gen) && R.state == HV_REG_ACTIVE && total_active() == 0 && !R.objects_pending && R.hard_failures == 0) { R.state = HV_REG_IDLE; changed(); r = 0; }
    unlock();
    return r;
}
HvRegState hv_reg_state(void) { lock(); HvRegState s = R.state; unlock(); return s; }
uint64_t hv_reg_generation(void) { lock(); uint64_t g = R.gen; unlock(); return g; }
HvRegQfReason hv_reg_qf_reason(void) { lock(); HvRegQfReason q = R.qf; unlock(); return q; }

int hv_reg_close_begin(uint64_t gen) {
    lock();
    int r = -1;
    if (!stale_locked(gen)) {
        int before = (int)R.state;
        if (R.state == HV_REG_ACTIVE) { R.state = HV_REG_CLOSING; R.terminate = 1; changed(); r = 0; }   /* C3: CLOSING entry and the registration check share this lock */
        else if (R.state == HV_REG_CLOSING) r = 0;
        HV_TEST_HOOK(HV_HOOK_CLOSE_DECIDED, before, r);
    }
    unlock();
    return r;
}
static void qf_locked(HvRegQfReason why) {
    R.state = HV_REG_QUIESCENCE_FAILED; R.qf = why; R.terminate = 1;   /* a terminal state: nothing is released, nothing returns to IDLE (C11) */
    changed();
}
int hv_reg_declare_failed(uint64_t gen, HvRegQfReason why) {
    lock();
    int r = -1;
    if (!stale_locked(gen)) {
        if (R.state == HV_REG_ACTIVE || R.state == HV_REG_CLOSING) { int before = (int)R.state; qf_locked(why); HV_TEST_HOOK(HV_HOOK_CLOSE_DECIDED, before, 1); r = 1; }
        else r = 0;   /* CLOSED first: the watchdog changes nothing */
    }
    unlock();
    return r;
}
int hv_reg_wait_and_close(uint64_t gen, int64_t deadline_ns) {
    lock();
    int r;
    for (;;) {
        if (stale_locked(gen)) { r = -1; break; }
        if (R.state == HV_REG_CLOSED) { r = 0; break; }
        if (R.state == HV_REG_QUIESCENCE_FAILED) { r = 1; break; }
        if (R.state != HV_REG_CLOSING) { r = -1; break; }
        if (total_active() == 0 && R.hard_failures == 0) {   /* C13 (a)(b), (c) the cancel registration closes with this decision: CLOSED refuses new cancels */
            int before = (int)R.state;
            R.state = HV_REG_CLOSED; changed();
            HV_TEST_HOOK(HV_HOOK_CLOSE_DECIDED, before, 0);
            r = 0; break;
        }
        if (wait_until(deadline_ns) && R.state == HV_REG_CLOSING && !(total_active() == 0 && R.hard_failures == 0)) {
            int before = (int)R.state;
            qf_locked(total_active() == 0 && R.hard_failures ? R.hard_reason : HV_REG_QF_TASKS_NOT_QUIESCENT);
            HV_TEST_HOOK(HV_HOOK_CLOSE_DECIDED, before, 1);
            r = 1; break;
        }
    }
    unlock();
    return r;
}

int hv_reg_try_register(uint64_t gen, HvRegKind kind) {
    if ((int)kind < 0 || kind >= HV_REG_K_CANCEL) return -1;
    lock();
    int r = -1;
    if (!stale_locked(gen)) {
        int before = (int)R.state;
        if (R.state == HV_REG_ACTIVE) { R.active[kind]++; r = 0; }   /* C1: the decision and the counter increment are one step under this lock */
        else R.refused[kind]++;
        HV_TEST_HOOK(HV_HOOK_REG_DECIDED, before, r);
    }
    unlock();
    return r;
}
static void release_locked(HvRegKind kind) { if (R.active[kind] > 0) R.active[kind]--; }
void hv_reg_complete(uint64_t gen, HvRegKind kind, int64_t failure) {
    lock();
    if (!stale_locked(gen)) {
        if (failure) record_error_locked(kind == HV_REG_K_ASYNC ? HV_REG_E_ASYNC : HV_REG_E_PROXY, failure);   /* C4: the failure and the decrement are one critical section */
        release_locked(kind);
        changed();
    }
    unlock();
}
void hv_reg_unregister_failed(uint64_t gen, HvRegKind kind, int64_t failure) {
    lock();
    if (!stale_locked(gen)) { record_error_locked(HV_REG_E_CREATE, failure ? failure : -1); release_locked(kind); changed(); }
    unlock();
}
void hv_reg_record_error(uint64_t gen, HvRegErrSource src, int64_t code) {
    lock();
    if (!stale_locked(gen)) { record_error_locked(src, code); changed(); }
    unlock();
}
int hv_reg_first_error(uint64_t gen, HvRegErrSource *src, int64_t *code) {
    lock();
    int r = 0;
    if (!stale_locked(gen) && R.err_have) { if (src) *src = R.err_src; if (code) *code = R.err_code; r = 1; }
    unlock();
    return r;
}
int hv_reg_error_count(void) { lock(); int n = R.err_count; unlock(); return n; }
int hv_reg_active(HvRegKind kind) { lock(); int n = R.active[kind]; unlock(); return n; }
int hv_reg_refused(HvRegKind kind) { lock(); int n = R.refused[kind]; unlock(); return n; }
void hv_reg_record_hard_failure(uint64_t gen, HvRegQfReason why) {
    lock();
    if (!stale_locked(gen)) { R.hard_failures++; if (R.hard_reason == HV_REG_QF_NONE) R.hard_reason = why; changed(); }
    unlock();
}

void hv_reg_request_terminate(uint64_t gen) { lock(); if (!stale_locked(gen)) { R.terminate = 1; changed(); } unlock(); }
int hv_reg_terminate_requested(void) { return __atomic_load_n(&R.terminate, __ATOMIC_ACQUIRE); }

/* ---- cancel ---- */
static int cancel_reserve_locked(void) {   /* 1 = the caller starts it */
    switch (R.cs) {
    case HV_REG_CS_NONE:
        R.cs = HV_REG_CS_RESERVED; R.active[HV_REG_K_CANCEL]++; changed(); return 1;
    case HV_REG_CS_CREATE_FAILED:
        if (R.cancel_retry_left > 0) { R.cancel_retry_left--; R.cs = HV_REG_CS_RESERVED; R.active[HV_REG_K_CANCEL]++; changed(); return 1; }   /* the first requester after the failure gets the retry */
        return 0;
    default:
        return 0;   /* reserved, running or finished: the requester waits for that one's result */
    }
}
int hv_reg_cancel_request(uint64_t gen) {
    lock();
    int r = -1;
    if (!stale_locked(gen)) {
        if (R.state == HV_REG_ACTIVE || R.state == HV_REG_CLOSING) r = cancel_reserve_locked();
        else if (R.state == HV_REG_CLOSED) R.cancel_rejected_closed++;   /* a late request for a finished session: refused, only recorded (C17) */
    }
    unlock();
    return r;
}
int hv_reg_begin_cancel_phase(uint64_t gen) {
    lock();
    int r = -1;
    if (!stale_locked(gen) && (R.state == HV_REG_ACTIVE || R.state == HV_REG_CLOSING)) {
        if (R.phase != HV_REG_PHASE_CANCEL) { R.phase = HV_REG_PHASE_CANCEL; changed(); }   /* the phase change and the reservation: one critical section */
        r = cancel_reserve_locked();
    }
    unlock();
    return r;
}
void hv_reg_enter_grace(uint64_t gen) { lock(); if (!stale_locked(gen) && R.phase == HV_REG_PHASE_RUN) { R.phase = HV_REG_PHASE_GRACE; changed(); } unlock(); }
HvRegPhase hv_reg_phase(void) { lock(); HvRegPhase p = R.phase; unlock(); return p; }
void hv_reg_cancel_started(uint64_t gen) { lock(); if (!stale_locked(gen) && R.cs == HV_REG_CS_RESERVED) { R.cs = HV_REG_CS_RUNNING; changed(); } unlock(); }
void hv_reg_cancel_create_failed(uint64_t gen, int64_t code) {
    lock();
    if (!stale_locked(gen) && R.cs == HV_REG_CS_RESERVED) {
        record_error_locked(HV_REG_E_CREATE, code ? code : -1);
        R.cs = HV_REG_CS_CREATE_FAILED; release_locked(HV_REG_K_CANCEL);   /* failure record and release: one critical section */
        if (R.cancel_retry_left <= 0) { R.hard_failures++; if (R.hard_reason == HV_REG_QF_NONE) R.hard_reason = HV_REG_QF_TASKS_NOT_QUIESCENT; }   /* no further attempt is possible: the session cannot be cancelled */
        changed();
    }
    unlock();
}
void hv_reg_cancel_done(uint64_t gen, int ok) {
    lock();
    if (!stale_locked(gen) && (R.cs == HV_REG_CS_RUNNING || R.cs == HV_REG_CS_RESERVED)) {
        R.cs = ok ? HV_REG_CS_DONE_OK : HV_REG_CS_DONE_FAILED;
        if (!ok) { R.hard_failures++; if (R.hard_reason == HV_REG_QF_NONE) R.hard_reason = HV_REG_QF_TASKS_NOT_QUIESCENT; }
        release_locked(HV_REG_K_CANCEL);
        changed();
    }
    unlock();
}
HvRegCancelState hv_reg_cancel_wait(uint64_t gen, int64_t deadline_ns) {
    lock();
    HvRegCancelState s;
    for (;;) {
        s = R.cs;
        if (stale_locked(gen)) break;
        if (s == HV_REG_CS_DONE_OK || s == HV_REG_CS_DONE_FAILED) break;
        if (s == HV_REG_CS_CREATE_FAILED && R.cancel_retry_left <= 0) break;
        if (R.state != HV_REG_ACTIVE && R.state != HV_REG_CLOSING) break;
        if (wait_until(deadline_ns)) { s = R.cs; break; }
    }
    unlock();
    return s;
}
HvRegCancelState hv_reg_cancel_state(void) { lock(); HvRegCancelState s = R.cs; unlock(); return s; }
int hv_reg_cancel_rejected_after_closed(void) { lock(); int n = R.cancel_rejected_closed; unlock(); return n; }

/* ---- connection table ---- */
int hv_reg_conn_register(uint64_t gen, int fd) {
    /* -1: the caller must NOT use this connection for the proxy (nobody could shut it down at cancel time). A duplicate fd means an earlier owner closed
       without unregistering (the number was reused) and a full table means connections would go unmanaged: both are recorded as hard failures, so the
       session cannot end as cleanly closed (C13 (b)). */
    lock();
    int r = -1;
    if (!stale_locked(gen) && (R.state == HV_REG_ACTIVE || R.state == HV_REG_CLOSING)) {
        int dup = 0, slot = -1;
        for (int i = 0; i < MAX_CONN; i++) {
            if (R.conns[i].used && R.conns[i].fd == fd) dup = 1;
            if (!R.conns[i].used && slot < 0) slot = i;
        }
        if (dup || slot < 0) { R.hard_failures++; if (R.hard_reason == HV_REG_QF_NONE) R.hard_reason = HV_REG_QF_CONN_TABLE; changed(); }
        else { R.conns[slot] = (ConnItem){.fd = fd, .refs = 0, .registered = 1, .used = 1}; r = 0; }
    }
    unlock();
    return r;
}
int hv_reg_conn_acquire(uint64_t gen, int *fds, int max) {
    lock();
    int n = 0;
    if (!stale_locked(gen)) for (int i = 0; i < MAX_CONN && n < max; i++) if (R.conns[i].used && R.conns[i].registered) { R.conns[i].refs++; fds[n++] = R.conns[i].fd; }
    unlock();   /* the caller calls shutdown() now, outside the lock (C12, C14) */
    return n;
}
void hv_reg_conn_release(uint64_t gen, const int *fds, int n) {
    lock();
    if (!stale_locked(gen)) {
        for (int k = 0; k < n; k++) for (int i = 0; i < MAX_CONN; i++) if (R.conns[i].used && R.conns[i].fd == fds[k] && R.conns[i].refs > 0) { R.conns[i].refs--; break; }
        changed();
    }
    unlock();
}
int hv_reg_conn_unregister_wait(uint64_t gen, int fd, int64_t deadline_ns) {
    lock();
    int idx = -1, r = -1;
    if (!stale_locked(gen)) {
        for (int i = 0; i < MAX_CONN; i++) if (R.conns[i].used && R.conns[i].fd == fd) { idx = i; break; }
        if (idx < 0) r = 0;   /* not registered: nothing to wait for */
        else {
            R.conns[idx].registered = 0;   /* no new reference can be taken from now on */
            while (R.conns[idx].refs > 0) if (wait_until(deadline_ns)) break;
            if (R.conns[idx].refs == 0) { R.conns[idx].used = 0; r = 0; }
            else { R.hard_failures++; if (R.hard_reason == HV_REG_QF_NONE) R.hard_reason = HV_REG_QF_CONN_REF_NOT_RETURNED; changed(); }   /* the fd stays open and the slot stays used: closing it could hand its number to someone else while shutdown() may still run */
        }
    }
    unlock();
    return r;
}
int hv_reg_conn_count(void) { lock(); int n = 0; for (int i = 0; i < MAX_CONN; i++) n += R.conns[i].used && R.conns[i].registered; unlock(); return n; }

/* ---- release and cleanup ---- */
int hv_reg_release_permitted(uint64_t gen) {
    lock();
    int r = -1;
    if (!stale_locked(gen)) {
        if (R.state == HV_REG_CLOSED) { if (R.cleanup_failed && !R.release_granted) r = 0; else { R.release_granted = 1; r = 1; } }   /* a granted release is never taken back */
        else r = 0;   /* ACTIVE, CLOSING, QUIESCENCE_FAILED: not permitted */
    }
    unlock();
    return r;
}
void hv_reg_objects_pending(uint64_t gen) { lock(); if (!stale_locked(gen)) { R.objects_pending = 1; R.objects_released = 0; changed(); } unlock(); }
void hv_reg_objects_released(uint64_t gen) { lock(); if (!stale_locked(gen)) { R.objects_released = 1; changed(); } unlock(); }
void hv_reg_cleanup_failed(uint64_t gen) { lock(); if (!stale_locked(gen) && R.state == HV_REG_CLOSED) { R.cleanup_failed = 1; changed(); } unlock(); }
int hv_reg_cleanup_failed_flag(void) { lock(); int f = R.cleanup_failed; unlock(); return f; }
static int can_finish_locked(void) { return R.state == HV_REG_CLOSED && !R.cleanup_failed && (!R.objects_pending || R.objects_released); }
int hv_reg_can_finish(uint64_t gen) { lock(); int r = !stale_locked(gen) && can_finish_locked(); unlock(); return r; }
int hv_reg_finish_session(uint64_t gen) {
    lock();
    int r = -1;
    if (!stale_locked(gen) && can_finish_locked()) { R.state = HV_REG_IDLE; changed(); r = 0; }   /* C15 (a) generation, (b) no cleanup failure, (c) objects released; (d) end() returned is the caller's precondition */
    unlock();
    return r;
}
int64_t hv_reg_set_deadline(uint64_t gen, int64_t ns) { lock(); int64_t p = R.deadline_ns; if (!stale_locked(gen)) { R.deadline_ns = ns; changed(); } unlock(); return p; }
int64_t hv_reg_deadline(void) { lock(); int64_t d = R.deadline_ns; unlock(); return d; }
uint64_t hv_reg_wait_change(uint64_t gen, uint64_t seen, int64_t deadline_ns) {
    lock();
    while (R.seq == seen && !stale_locked(gen)) if (wait_until(deadline_ns)) break;
    uint64_t s = R.seq;
    unlock();
    return s;
}
uint64_t hv_reg_seq(void) { lock(); uint64_t s = R.seq; unlock(); return s; }

/* ---- final result ---- */
HvRegReport hv_reg_compose(HvRegRestoreOutcome restore, HvRegCleanupOutcome cleanup, int quiescence_failed) {
    if (quiescence_failed) return HV_REG_R_SESSION_NOT_QUIESCENT;   /* the run never counts as a success when the session did not settle (C9) */
    if (cleanup == HV_REG_CLEANUP_FAILED) return HV_REG_R_CLEANUP_FAILED;
    if (cleanup == HV_REG_CLEANUP_IN_PROGRESS) return HV_REG_R_CLEANUP_DELAYED;
    switch (restore) {
    case HV_REG_RESTORE_OK: return HV_REG_R_SUCCESS;
    case HV_REG_RESTORE_USER_CANCEL: return HV_REG_R_ABORTED_BY_USER;
    default: return HV_REG_R_RESTORE_FAILED;
    }
}
int hv_reg_report(uint64_t gen, HvRegReport kind) {
    lock();
    int r = 0;
    if (!stale_locked(gen)) {
        if (kind == HV_REG_R_CLEANUP_DELAYED) { if (R.report_final == HV_REG_R_NONE) { R.report_interim = kind; changed(); } }
        else if (kind != HV_REG_R_NONE && R.report_final == HV_REG_R_NONE) { R.report_final = kind; changed(); r = 1; }   /* compare-and-set under the lock: once per generation */
    }
    unlock();
    return r;
}
HvRegReport hv_reg_report_final(void) { lock(); HvRegReport k = R.report_final; unlock(); return k; }
HvRegReport hv_reg_report_interim(void) { lock(); HvRegReport k = R.report_interim; unlock(); return k; }
int hv_reg_stale_notices(void) { lock(); int n = R.stale; unlock(); return n; }
