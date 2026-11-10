/* See restore_session.h. */
#include "restore_session.h"

#include "net_guard.h"

#include "idevicerestore.h"

extern int hv_no_ticket_files;   /* third_party/idevicerestore/src/idevicerestore.c */

int hv_restore_session_begin(void) {
    /* resetting the lists under a running engine thread would change the rules it is subject to; an earlier run that did not end is finished first */
    if (hv_net_guard_engine_count() > 0) return -1;
    hv_net_guard_reset();
    hv_net_guard_install();
    hv_no_ticket_files = 1;
    return hv_net_guard_hooks_intact() ? 0 : -1;
}

void hv_restore_session_allow_usbmuxd(uint16_t port) { hv_net_guard_allow_local("127.0.0.1", port); }

int hv_restore_session_run(int (*fn)(void *), void *arg, int *rc) {
    if (!fn || hv_net_guard_engine_enter() != 0) return -1;
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

int hv_restore_session_end(void) { return hv_net_guard_uninstall(); }
int hv_restore_session_engine_threads(void) { return hv_net_guard_engine_count(); }
