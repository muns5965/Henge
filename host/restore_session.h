#ifndef HENGEVM_RESTORE_SESSION_H
#define HENGEVM_RESTORE_SESSION_H

#include <stdint.h>

struct idevicerestore_client_t;

/* The one entry through which the restore tool (idevicerestore_start) may be run. It exists so that the tool cannot start without the connection policy
   (host/net_guard.h) in force, and so that the policy cannot be taken away while the tool is still running:
     begin()    resets the policy lists, installs the three hooks, turns off ticket files, and verifies the hooks are really in place. -1 = do not run anything.
     allow_usbmuxd(port)  registers the private usbmuxd's loopback listener (the only allow-list entry a session starts with).
     run*()     registers the calling thread as an engine thread, runs, unregisters on every path. It REFUSES (-1, the tool is not called) unless the hooks
                are intact at that moment.
     end()      removes the hooks only when no engine thread is registered; otherwise it returns -1 and the policy stays in force (a thread that did not return
                keeps being refused instead of being let go). Call it again later to retry.
   tools/check-no-test-flags.sh requires that idevicerestore_start is called from host/restore_session.c only. */
int hv_restore_session_begin(void);
void hv_restore_session_allow_usbmuxd(uint16_t port);
unsigned hv_restore_session_run_denied_owner_state(void);   /* observation only */
unsigned hv_restore_session_run_denied_engine(void);
unsigned hv_restore_session_abort_uninstall_failures(void);
int hv_restore_session_run(int (*fn)(void *), void *arg, int *rc);   /* generic; the tests use it */
int hv_restore_session_run_client(struct idevicerestore_client_t *c, int *rc);
int hv_restore_session_end(void);
int hv_restore_session_engine_threads(void);
/* Lifetime registry coupling (host/restore_registry.h, design v2.2): begin() is refused while an earlier session has not been ended cleanly; end() closes
   the session (CLOSING, waits for registered tasks up to the close timeout, CLOSED) and returns -1 -- policy kept, nothing released -- when it cannot. */
void hv_restore_session_set_close_timeout_ms(int ms);   /* provisional default 15000, unmeasured */
int hv_restore_session_close(void);                       /* CLOSING -> CLOSED (0), or -1 = QUIESCENCE_FAILED / refused */
int hv_restore_session_release_permitted(void);          /* 1 only when idevicerestore_client_free() may be called now */
void hv_restore_session_request_cancel(void);            /* lock-free terminate flag for the registered tasks */
int hv_restore_session_quiescence_failed(void);
int hv_restore_session_abort_start(void);   /* start failed before anything ran: ACTIVE -> IDLE and the policy removed (0), or -1 = stays */
int hv_restore_session_fail(int why);       /* declare QUIESCENCE_FAILED (why = HvRegQfReason) for a host-owned resource that did not stop */
int hv_restore_session_cancel(void);        /* 1 = cancel started now, 0 = already started, -1 = refused */
/* Ownership (C13) and ending a generation. begin() makes its caller the OWNER. A restore thread takes over ONCE: the owner calls approve_successor() before creating
   the thread and hands the token to it; the thread calls claim(token). Only the owner may close, finalize, publish, release_idle or end; cancel() may come from any
   thread. Every state-changing call refuses when made from inside the publication callback.
   abort_start   start failed before the hand-over: the owner (no transfer yet) returns the session to IDLE (registry abandon decides; on refusal the owner keeps it).
   finalize      closes (CLOSED, or QUIESCENCE_FAILED), removes the policy unless a host-owned thread is still alive (cleanup_mask HV_REG_CL_HOST_THREAD), and decides the final
                 result in one registry critical section (cleanup included). Returns an HvRegReport, or HV_REG_R_NONE (0) when nothing could be decided (the engine is still
                 inside run(), wrong thread). *may_publish is 1 for exactly one call per generation.
   publish       runs fn(arg) -- the callback, result variables -- outside every lock, with the one-time right from finalize; then PUBLISHING -> PUBLISHED.
   release_idle  CLOSED -> IDLE only after publish, with nothing unresolved and no confirmed cleanup failure (C15 (e)(f)). Not possible from inside the callback. */
uint64_t hv_restore_session_approve_successor(void);
int hv_restore_session_claim(uint64_t token);
int hv_restore_session_finalize(int restore_rc, int cleanup_mask, int *may_publish);
int hv_restore_session_publish(void (*fn)(void *), void *arg);
int hv_restore_session_release_idle(void);

#endif
