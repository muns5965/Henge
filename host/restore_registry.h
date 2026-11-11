#ifndef HENGEVM_RESTORE_REGISTRY_H
#define HENGEVM_RESTORE_REGISTRY_H
/* Session lifetime registry (docs/restore-lifetime-integrated-design.md v2.2, I1; constraints C1~C19).
   One restore session at a time, process-static (it outlives every session object, C5). All state changes are made under ONE mutex; nothing here calls out
   (no shutdown, close, thread creation) while that mutex is held (C12). Deadlines are absolute CLOCK_MONOTONIC times in nanoseconds (C19); the condition variable
   uses the same clock.
   States:  IDLE -begin()-> ACTIVE -close_begin()-> CLOSING -(C13)-> CLOSED -finish_session()-> IDLE ; ACTIVE|CLOSING -declare_failed()/deadline-> QUIESCENCE_FAILED (terminal).
   Every call that belongs to a session carries the generation number begin() returned; a call with another generation is refused and only counted (C15).
   THIS MODULE ONLY KEEPS THE RULES. Whoever calls it (host/restore_session.c, host/install.c, and later the vendored hooks I3~I6) is separate. */
#include <stdint.h>

typedef enum { HV_REG_IDLE = 0, HV_REG_ACTIVE, HV_REG_CLOSING, HV_REG_CLOSED, HV_REG_QUIESCENCE_FAILED } HvRegState;
typedef enum { HV_REG_K_ASYNC = 0, HV_REG_K_PROXY_CTRL, HV_REG_K_PROXY_CONN, HV_REG_K_CANCEL, HV_REG_K_N } HvRegKind;
typedef enum { HV_REG_PHASE_RUN = 0, HV_REG_PHASE_GRACE, HV_REG_PHASE_CANCEL } HvRegPhase;
typedef enum { HV_REG_CS_NONE = 0, HV_REG_CS_RESERVED, HV_REG_CS_RUNNING, HV_REG_CS_DONE_OK, HV_REG_CS_DONE_FAILED, HV_REG_CS_CREATE_FAILED } HvRegCancelState;
/* where the first error came from */
typedef enum { HV_REG_E_NONE = 0, HV_REG_E_LOOP, HV_REG_E_ASYNC, HV_REG_E_PROXY, HV_REG_E_CREATE, HV_REG_E_CANCEL, HV_REG_E_WATCH } HvRegErrSource;
/* why QUIESCENCE_FAILED was declared (4.2) */
typedef enum { HV_REG_QF_NONE = 0, HV_REG_QF_TASKS_NOT_QUIESCENT, HV_REG_QF_ENGINE_NOT_EXITED, HV_REG_QF_CONN_REF_NOT_RETURNED, HV_REG_QF_CONN_TABLE } HvRegQfReason;
/* application report kinds (4.7) */
typedef enum { HV_REG_R_NONE = 0, HV_REG_R_SUCCESS, HV_REG_R_RESTORE_FAILED, HV_REG_R_ABORTED_BY_USER, HV_REG_R_CLEANUP_DELAYED, HV_REG_R_CLEANUP_FAILED, HV_REG_R_SESSION_NOT_QUIESCENT } HvRegReport;
typedef enum { HV_REG_RESTORE_OK = 0, HV_REG_RESTORE_FAILED, HV_REG_RESTORE_USER_CANCEL, HV_REG_RESTORE_UNFINISHED } HvRegRestoreOutcome;
typedef enum { HV_REG_CLEANUP_DONE = 0, HV_REG_CLEANUP_IN_PROGRESS, HV_REG_CLEANUP_FAILED } HvRegCleanupOutcome;

int64_t hv_reg_mono_ns(void);   /* CLOCK_MONOTONIC, the clock every deadline here is on */

/* ---- state -------------------------------------------------------------------------------------------------------------------------------------- */
uint64_t hv_reg_begin(void);                  /* IDLE -> ACTIVE. Returns the new generation (> 0), or 0 when not IDLE. Clears every per-session record. */
int hv_reg_abandon(uint64_t gen);             /* ACTIVE with no task, no pending object and no engine work -> IDLE (a begin() that could not be completed). 0 / -1 */
HvRegState hv_reg_state(void);
uint64_t hv_reg_generation(void);
int hv_reg_close_begin(uint64_t gen);         /* ACTIVE -> CLOSING (also sets the terminate flag). CLOSING again: 0. Otherwise -1 */
/* Engine only (C13): waits until all four counters are 0, no hard failure is recorded, and then moves CLOSING -> CLOSED in the same critical section. 0 = CLOSED,
   1 = the deadline passed (or it was already declared) and the state is QUIESCENCE_FAILED, -1 = wrong generation / state (call close_begin first). */
int hv_reg_wait_and_close(uint64_t gen, int64_t deadline_ns);
/* Watchdog or engine: ACTIVE|CLOSING -> QUIESCENCE_FAILED. 1 = declared now, 0 = nothing changed (already CLOSED / failed), -1 = wrong generation. */
int hv_reg_declare_failed(uint64_t gen, HvRegQfReason why);
HvRegQfReason hv_reg_qf_reason(void);

/* ---- tasks (async request, proxy control, proxy connection): C1~C4 --------------------------------------------------------------------------------- */
int hv_reg_try_register(uint64_t gen, HvRegKind kind);   /* ACTIVE only. 0 = registered (counter++ in the same critical section), -1 = refused (counted) */
void hv_reg_complete(uint64_t gen, HvRegKind kind, int64_t failure);   /* the task's last act: failure != 0 is recorded, counter--, same critical section */
void hv_reg_unregister_failed(uint64_t gen, HvRegKind kind, int64_t failure);   /* thread creation failed after try_register: failure + release in one critical section (C2) */
void hv_reg_record_error(uint64_t gen, HvRegErrSource src, int64_t code);   /* only the FIRST error is kept; later ones are counted (C8) */
int hv_reg_first_error(uint64_t gen, HvRegErrSource *src, int64_t *code);   /* 1 if there is one */
int hv_reg_error_count(void);
int hv_reg_active(HvRegKind kind);
int hv_reg_refused(HvRegKind kind);
void hv_reg_record_hard_failure(uint64_t gen, HvRegQfReason why);   /* a termination failure that blocks CLOSED (C13 (b)) */

/* ---- terminate flag (C16): lock-free, read by the restore loop at the top of each iteration ---------------------------------------------------------- */
void hv_reg_request_terminate(uint64_t gen);
int hv_reg_terminate_requested(void);

/* ---- cancel (C17): single start, creation failure, result for the waiters ---------------------------------------------------------------------------- */
/* 1 = the caller must now run the cancel (outside the lock) and report hv_reg_cancel_started / _create_failed / _done; 0 = a cancel is already reserved, running or
   finished (wait with hv_reg_cancel_wait); -1 = refused (CLOSED / failed / wrong generation; after CLOSED only a refusal is counted). */
int hv_reg_cancel_request(uint64_t gen);
/* The grace -> cancel transition: sets phase CANCEL and reserves the cancel in ONE critical section. Return values as cancel_request. */
int hv_reg_begin_cancel_phase(uint64_t gen);
void hv_reg_enter_grace(uint64_t gen);
HvRegPhase hv_reg_phase(void);
void hv_reg_cancel_started(uint64_t gen);
void hv_reg_cancel_create_failed(uint64_t gen, int64_t code);   /* releases the reservation and records the failure in one critical section; a retry is allowed once to ONE requester */
void hv_reg_cancel_done(uint64_t gen, int ok);                  /* counter--; a failed cancel is a hard failure */
HvRegCancelState hv_reg_cancel_wait(uint64_t gen, int64_t deadline_ns);   /* returns the state it stopped at (a terminal one, or the state when the deadline came) */
HvRegCancelState hv_reg_cancel_state(void);
int hv_reg_cancel_rejected_after_closed(void);

/* ---- connection table (C14): reference-counted so a cancel can shutdown() without racing the owner's close() ------------------------------------------ */
int hv_reg_conn_register(uint64_t gen, int fd);                      /* ACTIVE|CLOSING. 0 / -1; -1 (duplicate fd, full table) is also a recorded hard failure: do not use the connection */
int hv_reg_conn_acquire(uint64_t gen, int *fds, int max);            /* takes a reference on every registered fd and lists them; call hv_reg_conn_release with the same list */
void hv_reg_conn_release(uint64_t gen, const int *fds, int n);
/* Owner side: unregister, then wait (until the deadline) for the references to come back. 0 = the caller may close(fd). -1 = a reference did not come back: the caller
   must NOT close the fd; a hard failure (conn_ref_not_returned) is recorded. */
int hv_reg_conn_unregister_wait(uint64_t gen, int fd, int64_t deadline_ns);
int hv_reg_conn_count(void);

/* ---- release and cleanup (C15, C18, C19) --------------------------------------------------------------------------------------------------------- */
/* C18: 1 = CLOSED and no cleanup failure: the session objects may be released; 0 = QUIESCENCE_FAILED or cleanup failed: do not release; -1 = not decidable (other
   state, wrong generation): do not release. A 1 is never taken back. */
int hv_reg_release_permitted(uint64_t gen);
void hv_reg_objects_pending(uint64_t gen);    /* the caller created session objects that must be released before IDLE */
void hv_reg_objects_released(uint64_t gen);
/* cleanup failure reasons: OR-accumulated bits. HISTORY (every reason seen this generation) is diagnosis only; CURRENT is what is still unresolved. */
#define HV_REG_CL_HOST_THREAD 1   /* a host-owned thread (the private usbmuxd) did not stop */
#define HV_REG_CL_POLICY 2        /* the connection policy could not be removed */
#define HV_REG_CL_OBJECT 4        /* a session object is not released */
#define HV_REG_CL_OTHER 8
void hv_reg_cleanup_mark(uint64_t gen, int mask);      /* CLOSED only. history |= mask, current |= mask */
void hv_reg_cleanup_resolve(uint64_t gen, int mask);   /* current &= ~mask (history keeps it); a CONFIRMED CLEANUP_FAILED result still blocks IDLE */
int hv_reg_cleanup_history(void);
int hv_reg_cleanup_current(void);
void hv_reg_cleanup_failed(uint64_t gen);     /* watchdog: the cleanup deadline passed (only meaningful in CLOSED). Sticky; no forced release (C19) */
int hv_reg_cleanup_failed_flag(void);
int hv_reg_can_finish(uint64_t gen);          /* 1 if finish_session() would succeed now */
int hv_reg_finish_session(uint64_t gen);      /* CLOSED -> IDLE when C15 (a)(b)(c) hold; 0 / -1. Refused while a result is being published (PUBLISHING). Legacy path: no finalize was ever called. */

/* ---- result: finalize, publish, release (C9, C15 (e)(f)) ----------------------------------------------------------------------------------------
   finalize decides the session's final result in ONE critical section: the closing condition, the cleanup result (a recorded cleanup failure, or objects still
   pending, count as CLEANUP_FAILED), the composition (4.7) and the once-per-generation record. The first finalize of a generation also receives the RIGHT TO PUBLISH
   (publish state NOT_PUBLISHED -> PUBLISHING); a later call only reads the recorded result and never gets the right again. The caller publishes OUTSIDE every lock
   (callbacks, result variables), then calls publish_done; release_idle moves CLOSED -> IDLE only for PUBLISHED and only when no cleanup failure is recorded.
   QUIESCENCE_FAILED also finalizes (result SESSION_NOT_QUIESCENT) and publishes, but release_idle is refused there. */
typedef enum { HV_REG_PUB_NONE = 0, HV_REG_PUB_PUBLISHING, HV_REG_PUB_PUBLISHED } HvRegPubState;
int hv_reg_finalize(uint64_t gen, HvRegRestoreOutcome restore, int cleanup_mask, HvRegReport *out, int *may_publish);   /* 0, or -1 (stale / not CLOSED or QUIESCENCE_FAILED) */
int hv_reg_publish_done(uint64_t gen);        /* PUBLISHING -> PUBLISHED; 0 / -1 */
int hv_reg_release_idle(uint64_t gen);        /* CLOSED + PUBLISHED + no cleanup failure/pending object -> IDLE; 0 / -1 */
HvRegPubState hv_reg_pub_state(void);
/* cancel history (host-only path: no cancel task exists, so nothing is counted and nothing blocks the close). requested = the request was ACCEPTED before the restore
   result was decided; it is kept for the whole generation and is separate from the result. note: 1 first accept, 0 already accepted, -1 refused (stale, not ACTIVE/CLOSING,
   or the restore already returned: the completion came first). */
int hv_reg_cancel_note(uint64_t gen);
int hv_reg_cancel_requested(void);
void hv_reg_restore_returned(uint64_t gen);   /* the engine's run returned: later cancel requests are refused */
int64_t hv_reg_set_deadline(uint64_t gen, int64_t ns);   /* the watchdog's current phase deadline, readable by the engine; returns the previous one */
int64_t hv_reg_deadline(void);
/* Blocks until the sequence number differs from `seen` (any state, phase, counter or flag change) or the deadline; returns the current sequence number. */
uint64_t hv_reg_wait_change(uint64_t gen, uint64_t seen, int64_t deadline_ns);
uint64_t hv_reg_seq(void);

/* ---- final result (C9, 4.7): composed from the restore result and the cleanup result, fixed once per generation ------------------------------------ */
HvRegReport hv_reg_compose(HvRegRestoreOutcome restore, HvRegCleanupOutcome cleanup, int quiescence_failed);
int hv_reg_report(uint64_t gen, HvRegReport kind);   /* final kinds are set once per generation (returns 1 for the call that set it, 0 for a later one); CLEANUP_DELAYED is interim */
HvRegReport hv_reg_report_final(void);
HvRegReport hv_reg_report_interim(void);

/* ---- diagnostics ---------------------------------------------------------------------------------------------------------------------------- */
int hv_reg_stale_notices(void);   /* calls that carried another generation */

#ifdef HV_TEST_HOOKS
/* TEST BUILDS ONLY (tools/check-no-test-flags.sh keeps them out of the shipped objects). OBSERVATION ONLY: inside the lock they append one fixed-size record; they never
   wait, change a result or release the lock (docs/fake-device-s4-test-hook-design-2026-11-11.md §3). */
enum { HV_HOOK_REG_DECIDED = 1, HV_HOOK_CLOSE_DECIDED = 2 };
typedef struct { uint64_t order; int id; int state_before, state_after; int active[HV_REG_K_N]; int result; } HvHookRec;
void hv_test_hook_reset(void);
int hv_test_hook_log(HvHookRec *out, int max);
#endif

#endif
