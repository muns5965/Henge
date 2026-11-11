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

#endif
