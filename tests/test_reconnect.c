/* The virtual USB reconnect, five ways, against the real libirecovery + host/libusb_shim.c + the stand-in ROM of tests/test_irecv.c, with NO image transfer:
   the stage change after an iBSS/iBEC is reproduced by the guest side leaving and re-joining the bus (hv_vusb_set_online), which is what devices/bdif.c and the
   kernel driver do. Linux. Scenario in argv[1]:
     normal   - the device leaves, comes back after 200 ms: left, arrived, a new client opens, the OLD client fails          -> HV_TR_OK
     nodisc   - the device never leaves                                                                                       -> HV_TR_NO_LEAVE
     noreturn - the device leaves and stays away                                                                              -> HV_TR_NO_ARRIVE
     fast     - leave and return back to back (faster than the shim polls)                                                    -> HV_TR_OK (seen as one look)
     enumfail - the device returns but every descriptor request STALLs (the shim cannot enumerate it)                         -> HV_TR_OPEN_FAILED
     verdict  - the pure verdict function on injected observations
   Exit 0 when the scenario produced the expected verdict. */
#define main irecv_test_main
#include "test_irecv.c"
#undef main

#include "../host/restore_transition.h"
#include <libirecovery.h>
#include <time.h>

static int msnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

static void *actor(void *arg) {
    const char *sc = arg;
    usleep(150000);
    if (!strcmp(sc, "normal")) { hv_vusb_set_online(g_u, 0); usleep(200000); hv_vusb_set_online(g_u, 1); }
    else if (!strcmp(sc, "noreturn")) hv_vusb_set_online(g_u, 0);
    else if (!strcmp(sc, "fast")) { hv_vusb_set_online(g_u, 0); hv_vusb_set_online(g_u, 1); }
    else if (!strcmp(sc, "enumfail")) { hv_vusb_set_online(g_u, 0); usleep(100000); g_stall_desc = 1; hv_vusb_set_online(g_u, 1); }
    return NULL;
}

static int verdict_tests(void) {
    HvTrSpec sp = {1000, 1000, 0x1227, 0x1283};
    HvTrObs o = {.left = 1, .left_after_ms = 10, .arrived = 1, .arrived_after_ms = 20, .opened = 1, .mode = 0x1227, .old_io_failed = 1};
    char n[200];
    int bad = 0;
#define T(label, want) do { int r = hv_transition_verdict(&o, &sp, n, sizeof n); int ok = r == (want); printf("%s %s -> %s (%s)\n", ok ? "PASS" : "FAIL", label, hv_transition_name(r), n); bad += !ok; } while (0)
    T("all conditions met", HV_TR_OK);
    o.mode = 0x1281; T("recovery product id is accepted", HV_TR_OK);
    o.mode = 0x12a8; T("a product id outside DFU/recovery", HV_TR_BAD_MODE);
    o.mode = 0x1227; o.old_io_failed = 0; T("the old client still works", HV_TR_OLD_HANDLE_USABLE);
    o.old_io_hung = 1; T("the old client hangs", HV_TR_OLD_HANDLE_HUNG);
    o.old_io_hung = 0; o.old_io_failed = 1; o.opened = 0; T("no new client", HV_TR_OPEN_FAILED);
    o.opened = 1; o.arrived = 0; T("never came back", HV_TR_NO_ARRIVE);
    o.left = 0; T("never left", HV_TR_NO_LEAVE);
    printf("%s\n", bad ? "RECONNECT VERDICT FAILED" : "RECONNECT VERDICT OK");
    return bad;
}

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "normal";
    if (!strcmp(sc, "verdict")) return verdict_tests();
    g_sc = sc;
    uint64_t ecid = 0x2d2a8d94f32ba4d2ull;
    g_dev_ecid = ecid;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    pthread_t rt, at;
    pthread_create(&rt, NULL, rom, NULL);
    hv_vusb_set_online(g_u, 1);
    hv_libusb_shim_attach(g_u);
    irecv_client_t old = NULL;
    irecv_error_t e = irecv_open_with_ecid_and_attempts(&old, ecid, 3);
    if (e != IRECV_E_SUCCESS) { printf("FAIL the first open failed (%d)\n", (int)e); return 2; }
    printf("opened the first client\n");

    HvTrSpec sp = {!strcmp(sc, "nodisc") ? 700 : 2000, !strcmp(sc, "noreturn") ? 700 : 2000, 0x1227, 0x1283};
    pthread_create(&at, NULL, actor, (void *)sc);
    HvTrObs o; memset(&o, 0, sizeof o);
    hv_transition_observe(g_u, &sp, &o);
    pthread_join(at, NULL);
    if (o.arrived) {
        irecv_client_t nw = NULL;
        int t0 = msnow();
        irecv_error_t e2 = irecv_open_with_ecid_and_attempts(&nw, ecid, 2);
        o.opened = e2 == IRECV_E_SUCCESS;
        if (o.opened) { int m = 0; irecv_get_mode(nw, &m); o.mode = m; }
        printf("second open: %s after %d ms\n", o.opened ? "ok" : "failed", msnow() - t0);
        /* the OLD client: DFU GET_STATE on it must fail quickly */
        unsigned char st[1] = {0};
        int t1 = msnow();
        int r = irecv_usb_control_transfer(old, 0xA1, 5, 0, 0, st, 1, 800);
        int took = msnow() - t1;
        o.old_io_hung = took > 3000;
        o.old_io_failed = r < 0;
        printf("old client GET_STATE: result %d after %d ms\n", r, took);
        if (nw) irecv_close(nw);
    }
    char note[240];
    int v = hv_transition_verdict(&o, &sp, note, sizeof note);
    printf("scenario %s: transition %s -- %s\n", sc, hv_transition_name(v), note);
    g_stop = 1;
    pthread_join(rt, NULL);
    int want = !strcmp(sc, "normal") || !strcmp(sc, "fast") ? HV_TR_OK : !strcmp(sc, "nodisc") ? HV_TR_NO_LEAVE : !strcmp(sc, "noreturn") ? HV_TR_NO_ARRIVE : HV_TR_OPEN_FAILED;
    int bad = v != want || (!strcmp(sc, "fast") && !o.fast);
    printf("%s scenario %s expected %s\n", bad ? "FAIL" : "PASS", sc, hv_transition_name(want));
    return bad ? 1 : 0;
}
