/* S1 baseline (docs/fake-device-impl-scope.md, T2 (a)~(e) + control): the CURRENT restore_device() driven by a fake usbmuxd and a fake restored on loopback.
   It RECORDS what restore_device returns and logs; it makes no pass/fail judgement about the lifetime design (no product code is changed, no hook is used).
   usage: test_fake_restore <case>      case = 0 (control) | a | b | c1 | c2 | c3 | d | e
   One case per process: a per-case watchdog thread records "HANG OBSERVED" and _exit()s when the time limit (env FAKE_CASE_LIMIT_S, default 150) passes. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <plist/plist.h>
#include "config.h"   /* third_party/restore_config/config.h: the same file the vendored restore.c is built with, so the macro below is the one restore_device sees */
#include "fake_device/fake_dev.h"
#include "../host/restore_session.h"
#include "../host/net_guard.h"
#include "../third_party/idevicerestore/src/idevicerestore.h"
#include "../third_party/idevicerestore/src/restore.h"
#include "../third_party/idevicerestore/src/log.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double t0;
static const char *g_case;

static void *watchdog(void *a) {
    int lim = (int)(intptr_t)a;
    sleep((unsigned)lim);
    printf("CASE %s: HANG OBSERVED after %.1f s (limit %d s); restore_device had not returned\n", g_case, now() - t0, lim);
    fflush(stdout);
    _exit(124);
}

static plist_t status(int64_t st, int has_amr, uint64_t amr) {
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "MsgType", plist_new_string("StatusMsg"));
    plist_dict_set_item(d, "Status", plist_new_uint((uint64_t)st));
    if (has_amr) plist_dict_set_item(d, "AMRError", plist_new_uint(amr));
    return d;
}
static plist_t progress(void) {
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "MsgType", plist_new_string("ProgressMsg"));
    plist_dict_set_item(d, "Operation", plist_new_uint(11));
    plist_dict_set_item(d, "Progress", plist_new_uint(50));
    return d;
}

typedef struct { struct idevicerestore_client_t *c; plist_t bi; } Arg;
static int run_fn(void *p) { Arg *a = p; return restore_device(a->c, a->bi); }

int main(int argc, char **argv) {
    g_case = argc > 1 ? argv[1] : "0";
    setvbuf(stdout, NULL, _IONBF, 0);
    plist_t msgs[4] = {0};
    FakeScenario scn = {.msgs = msgs, .nmsgs = 0, .end = FAKE_END_HOLD};
    const char *desc = "";
    if (!strcmp(g_case, "0"))  { msgs[0] = status(0, 0, 0); scn.nmsgs = 1; desc = "control: StatusMsg Status=0 (restore finished)"; }
    else if (!strcmp(g_case, "a"))  { msgs[0] = status(14, 1, 1); scn.nmsgs = 1; desc = "negative StatusMsg (AMRError=1), then nothing (restored stays silent)"; }
    else if (!strcmp(g_case, "b"))  { msgs[0] = status(14, 1, 1); msgs[1] = progress(); scn.nmsgs = 2; desc = "negative StatusMsg then a valid ProgressMsg (back to back)"; }
    else if (!strcmp(g_case, "c1")) { msgs[0] = status(14, 1, 1); msgs[1] = plist_new_dict(); scn.nmsgs = 2; desc = "negative StatusMsg then a message without MsgType"; }
    else if (!strcmp(g_case, "c2")) { msgs[0] = status(14, 1, 1); msgs[1] = plist_new_dict(); plist_dict_set_item(msgs[1], "MsgType", plist_new_uint(5)); scn.nmsgs = 2; desc = "negative StatusMsg then a message with non-string MsgType"; }
    else if (!strcmp(g_case, "c3")) { msgs[0] = status(14, 1, 1); msgs[1] = plist_new_dict(); plist_dict_set_item(msgs[1], "MsgType", plist_new_string("FooMsg")); scn.nmsgs = 2; desc = "negative StatusMsg then an unknown MsgType"; }
    else if (!strcmp(g_case, "d"))  { msgs[0] = status(14, 1, 1); scn.nmsgs = 1; scn.end = FAKE_END_CLOSE; desc = "negative StatusMsg then the connection is closed"; }
    else if (!strcmp(g_case, "e"))  { msgs[0] = plist_new_dict(); plist_dict_set_item(msgs[0], "MsgType", plist_new_string("CheckpointMsg")); scn.nmsgs = 1; desc = "CheckpointMsg without CHECKPOINT_ID/RESULT"; }
    else { printf("unknown case %s\n", g_case); return 2; }

    int lim = getenv("FAKE_CASE_LIMIT_S") ? atoi(getenv("FAKE_CASE_LIMIT_S")) : 150;
    t0 = now();
    pthread_t w; pthread_create(&w, NULL, watchdog, (void *)(intptr_t)lim); pthread_detach(w);

    int port = fake_mux_start(&scn);
    if (port <= 0) { printf("CASE %s: fake mux failed to start\n", g_case); return 2; }
    char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%d", port);
    setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);   /* the library's only route is the fake on loopback */

    printf("CASE %s: %s\n", g_case, desc);
#ifdef HAVE_RESTORE_E_RECEIVE_TIMEOUT
    printf("BUILD HAVE_RESTORE_E_RECEIVE_TIMEOUT=defined\n");
#else
    printf("BUILD HAVE_RESTORE_E_RECEIVE_TIMEOUT=undefined \n");
#endif
    if (hv_restore_session_begin() != 0) { printf("CASE %s: session begin refused\n", g_case); return 2; }
    hv_restore_session_allow_usbmuxd((uint16_t)port);
    log_level = LL_DEBUG;

    struct idevicerestore_client_t *c = idevicerestore_client_new();
    idevicerestore_set_ecid(c, FAKE_ECID);
    idevicerestore_set_udid(c, FAKE_UDID);
    Arg a = {c, plist_new_dict()};
    int rc = 12345, ret = hv_restore_session_run(run_fn, &a, &rc);
    double dt = now() - t0;

    HvNetStats ns; hv_net_guard_stats(&ns);
    FakeStats fs; fake_stats(&fs);
    printf("RESULT case=%s run()=%d restore_device=%d elapsed=%.1fs\n", g_case, ret, rc, dt);
    printf("NETGUARD attempts=%d allowed=%d denied=%d (only the fake mux 127.0.0.1:<ephemeral> may be allowed)\n", ns.attempts, ns.allowed, ns.denied);
    printf("FAKE mux_accepted=%d list_devices=%d connect_restored=%d connect_other=%d(last port %d) start_restore=%d goodbye=%d\n",
           fs.accepted, fs.list_devices, fs.connect_restored, fs.connect_other, fs.last_other_port, fs.start_restore, fs.goodbye);
    printf("ENGINE_THREADS_AFTER=%d\n", hv_restore_session_engine_threads());
    fflush(stdout);
    _exit(0);   /* the usbmuxd thread never returns on its own; the verdict is the RESULT line, not the exit status */
}
