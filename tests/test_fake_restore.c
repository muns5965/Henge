/* S1 baseline (docs/fake-device-impl-scope.md, T2 (a)~(e) + control): the CURRENT restore_device() driven by a fake usbmuxd and a fake restored on loopback.
   It RECORDS what restore_device returns and logs; it makes no pass/fail judgement about the lifetime design (no product code is changed, no hook is used).
   usage: test_fake_restore <case>      case = 0 (control) | a | b | c1 | c2 | c3 | d | e                       (S1, T2)
                                        | t5s_bb | t5s_fdr | t5s_fdr_nr | t5a_free | t5a_nofree          (S2, T5)
                                        | t12 | t13                                                         (S3, T12/T13)
                                        | t12_conn | t12_conn_cancel                                           (I6: proxy connect blocked / cancelled)
                                        | t1b | t3_inject                                                   (T1b, T3 baselines)
   One case per process: a per-case watchdog thread records "HANG OBSERVED" and _exit()s when the time limit (env FAKE_CASE_LIMIT_S, default 150) passes. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <plist/plist.h>
#include "config.h"   /* third_party/restore_config/config.h: the same file the vendored restore.c is built with, so the macro below is the one restore_device sees */
#include "fake_device/fake_dev.h"
#include "../host/restore_session.h"
#include "../host/net_guard.h"
#include "../host/restore_registry.h"
#include "../third_party/idevicerestore/src/idevicerestore.h"
#include "../third_party/idevicerestore/src/restore.h"
#include "../third_party/idevicerestore/src/log.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
extern volatile int hv_test_fail_next_thread_new, hv_test_thread_new_calls, hv_test_thread_new_failed;   /* tests/fake_device/wrap_thread.c */
extern volatile int hv_test_thread_join_calls;   /* tests/fake_device/wrap_join.c */
extern int __lsan_do_recoverable_leak_check(void) __attribute__((weak));
static double t0;
static const char *g_case;

static void *watchdog(void *a) {
    int lim = (int)(intptr_t)a;
    sleep((unsigned)lim);
    printf("CASE %s: HANG OBSERVED after %.1f s (limit %d s); restore_device had not returned\n", g_case, now() - t0, lim);
    { HvNetStats ns; FakeStats fs; hv_net_guard_stats(&ns); fake_stats(&fs);
      printf("AT_HANG netguard attempts=%d allowed=%d denied=%d | fake mux_accepted=%d connect_restored=%d data_port_connects=%d proxy_ctrl=%d proxy_conn=%d proxy_cmd_acked=%d\n",
             ns.attempts, ns.allowed, ns.denied, fs.accepted, fs.connect_restored, fs.dp_connect, fs.px_ctrl, fs.px_conn, fs.px_cmd_acked); }
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

static plist_t data_req(const char *msgtype, const char *dtype, int with_port) {
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "MsgType", plist_new_string(msgtype));
    plist_dict_set_item(d, "DataType", plist_new_string(dtype));
    if (with_port) plist_dict_set_item(d, "DataPort", plist_new_uint(FAKE_DATA_PORT));
    return d;
}
static plist_t urlasset_req(void) {   /* the host's URLAsset policy refuses this URL before anything is opened; the reply still goes to DataPort */
    plist_t d = data_req("AsyncDataRequestMsg", "URLAsset", 1);
    plist_t a = plist_new_dict();
    plist_dict_set_item(a, "RequestMethod", plist_new_string("GET"));
    plist_dict_set_item(a, "RequestURL", plist_new_string("https://fake-asset.invalid/x"));
    plist_dict_set_item(d, "Arguments", a);
    return d;
}
static int count_fds(void) {
    int n = 0;
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    while (readdir(dir)) n++;
    closedir(dir);
    return n - 3;   /* ".", ".." and the directory's own fd */
}
/* "the external host" the fake device asks the host to open (T12/T13): a loopback listener that accepts and says nothing */
static void *hold_conn(void *a) { int fd = (int)(intptr_t)a; char b[256]; while (recv(fd, b, sizeof b, 0) > 0) {} close(fd); return NULL; }
static int g_ext = -1;
static void *ext_accept(void *a) { (void)a; for (;;) { int c = accept(g_ext, NULL, NULL); if (c < 0) break; pthread_t t; pthread_create(&t, NULL, hold_conn, (void *)(intptr_t)c); pthread_detach(t); } return NULL; }
static int ext_start(void) {
    g_ext = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_ext, (struct sockaddr *)&a, sizeof a) || listen(g_ext, 8)) return -1;
    socklen_t l = sizeof a;
    getsockname(g_ext, (struct sockaddr *)&a, &l);
    pthread_t t; pthread_create(&t, NULL, ext_accept, NULL); pthread_detach(t);
    return ntohs(a.sin_port);
}


/* I6: an "external host" whose accept queue is full, so a further connect() sits in SYN_SENT (the S3lib situation) */
static int ext_start_full(void) {
    g_ext = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_ext, (struct sockaddr *)&a, sizeof a) || listen(g_ext, 0)) return -1;
    socklen_t l = sizeof a;
    getsockname(g_ext, (struct sockaddr *)&a, &l);
    for (int i = 0; i < 2; i++) { int f = socket(AF_INET, SOCK_STREAM, 0); fcntl(f, F_SETFL, O_NONBLOCK); connect(f, (struct sockaddr *)&a, sizeof a); }   /* fillers (kept open) */
    usleep(100000);
    return ntohs(a.sin_port);
}
static int syn_sent_to(int port) {   /* sockets of this host in SYN_SENT towards 127.0.0.1:<port>, from /proc/net/tcp */
    FILE *f = fopen("/proc/net/tcp", "r"); if (!f) return -1;
    char line[512]; int n = 0; fgets(line, sizeof line, f);
    while (fgets(line, sizeof line, f)) {
        char rem[64], st[8]; unsigned x;
        if (sscanf(line, " %*d: %*64[0-9A-Fa-f:] %63[0-9A-Fa-f:] %7s", rem, st) == 2 && sscanf(rem, "%*x:%x", &x) == 1 && (int)x == port && !strcmp(st, "02")) n++;
    }
    fclose(f); return n;
}
static int g_ext_port_s;
static int count_threads(void) { int n = 0; DIR *d = opendir("/proc/self/task"); if (!d) return -1; struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') n++; closedir(d); return n; }
static void *cancel_probe(void *a) {   /* waits for the proxy connect to sit in SYN_SENT, cancels through the host API, measures when it is gone */
    (void)a; double w = now();
    while (syn_sent_to(g_ext_port_s) < 1 && now() - w < 8) usleep(20000);
    if (syn_sent_to(g_ext_port_s) < 1) { printf("CONNECT_CANCEL no connect reached SYN_SENT (nothing to cancel)\n"); fflush(stdout); _exit(0); }
    double tc = now();
    int r = hv_restore_session_cancel();
    while (syn_sent_to(g_ext_port_s) > 0 && now() - tc < 12) usleep(10000);
    double gone = now() - tc;
    printf("CONNECT_CANCEL cancel()=%d syn_sent_left=%d gone_after=%.3fs (%s)\n", r, syn_sent_to(g_ext_port_s), gone, syn_sent_to(g_ext_port_s) ? "STILL BLOCKED" : "released");
    fflush(stdout); _exit(0);
}

typedef struct { struct idevicerestore_client_t *c; plist_t bi; } Arg;
static int run_fn(void *p) { Arg *a = p; return restore_device(a->c, a->bi); }

int main(int argc, char **argv) {
    g_case = argc > 1 ? argv[1] : "0";
    setvbuf(stdout, NULL, _IONBF, 0);
    plist_t msgs[4] = {0};
    FakeScenario scn = {.msgs = msgs, .nmsgs = 0, .end = FAKE_END_HOLD};
    const char *desc = "";
    int free_client = 0, after_s = 0, ext_port = 0, arm_fail = 0;
    if (!strcmp(g_case, "0"))  { msgs[0] = status(0, 0, 0); scn.nmsgs = 1; desc = "control: StatusMsg Status=0 (restore finished)"; }
    else if (!strcmp(g_case, "a"))  { msgs[0] = status(14, 1, 1); scn.nmsgs = 1; desc = "negative StatusMsg (AMRError=1), then nothing (restored stays silent)"; }
    else if (!strcmp(g_case, "b"))  { msgs[0] = status(14, 1, 1); msgs[1] = progress(); scn.nmsgs = 2; desc = "negative StatusMsg then a valid ProgressMsg (back to back)"; }
    else if (!strcmp(g_case, "c1")) { msgs[0] = status(14, 1, 1); msgs[1] = plist_new_dict(); scn.nmsgs = 2; desc = "negative StatusMsg then a message without MsgType"; }
    else if (!strcmp(g_case, "c2")) { msgs[0] = status(14, 1, 1); msgs[1] = plist_new_dict(); plist_dict_set_item(msgs[1], "MsgType", plist_new_uint(5)); scn.nmsgs = 2; desc = "negative StatusMsg then a message with non-string MsgType"; }
    else if (!strcmp(g_case, "c3")) { msgs[0] = status(14, 1, 1); msgs[1] = plist_new_dict(); plist_dict_set_item(msgs[1], "MsgType", plist_new_string("FooMsg")); scn.nmsgs = 2; desc = "negative StatusMsg then an unknown MsgType"; }
    else if (!strcmp(g_case, "d"))  { msgs[0] = status(14, 1, 1); scn.nmsgs = 1; scn.end = FAKE_END_CLOSE; desc = "negative StatusMsg then the connection is closed"; }
    else if (!strcmp(g_case, "e"))  { msgs[0] = plist_new_dict(); plist_dict_set_item(msgs[0], "MsgType", plist_new_string("CheckpointMsg")); scn.nmsgs = 1; desc = "CheckpointMsg without CHECKPOINT_ID/RESULT"; }
    /* S2 (T5): a data request whose handler needs only the DataPort (no IPSW, no build identity) */
    else if (!strcmp(g_case, "t5s_bb"))      { msgs[0] = data_req("DataRequestMsg", "BasebandUpdaterOutputData", 1); scn.nmsgs = 1; scn.data_port_mode = FAKE_DP_OPEN_SILENT; desc = "T5 sync: BasebandUpdaterOutputData, data port accepts and stays silent"; }
    else if (!strcmp(g_case, "t5s_fdr")) { msgs[0] = data_req("DataRequestMsg", "FDRTrustData", 1); msgs[1] = status(0, 0, 0); scn.nmsgs = 2; scn.data_port_mode = FAKE_DP_OPEN_SILENT; desc = "T5 sync: FDRTrustData (connect + send), data port accepts; control: restore then finishes"; }
    else if (!strcmp(g_case, "t5s_fdr_nr")) { msgs[0] = data_req("DataRequestMsg", "FDRTrustData", 1); msgs[1] = status(0, 0, 0); scn.nmsgs = 2; scn.data_port_mode = FAKE_DP_NOREPLY; desc = "T5 sync: FDRTrustData, the mux never answers the Connect to the data port"; }
    else if (!strcmp(g_case, "t5a_free") || !strcmp(g_case, "t5a_nofree")) {
        msgs[0] = urlasset_req(); msgs[1] = status(14, 1, 1); msgs[2] = plist_new_dict(); plist_dict_set_item(msgs[2], "MsgType", plist_new_string("FooMsg")); scn.nmsgs = 3;
        scn.data_port_mode = FAKE_DP_DELAY_REFUSE; scn.data_port_delay_ms = 3000; after_s = 6;
        free_client = !strcmp(g_case, "t5a_free");
        desc = free_client ? "T5 async: URLAsset thread is waiting for its data-port Connect when restore_device ends; the client is then freed the way the host does"
                           : "T5 async: same, but the client is NOT freed (shows what the thread does when nothing is freed)";
    }
    /* S3 (T12/T13): fake reverse-proxy device */
    else if (!strcmp(g_case, "t12") || !strcmp(g_case, "t12_close") || !strcmp(g_case, "t13") || !strcmp(g_case, "t12_conn") || !strcmp(g_case, "t12_conn_cancel")) {
        ext_port = (!strcmp(g_case, "t12_conn") || !strcmp(g_case, "t12_conn_cancel")) ? ext_start_full() : ext_start(); g_ext_port_s = ext_port;
        scn.proxy = 1; scn.msg_delay_ms = 1500; scn.proxy_target_port = (uint16_t)ext_port;
        msgs[0] = status(0, 0, 0); scn.nmsgs = 1; after_s = 4;
        if (!strcmp(g_case, "t12_close")) { scn.proxy_connect_reqs = 1; scn.proxy_do_cmd = 1; scn.proxy_close_after_ms = 1000; desc = "T12 variant: the device closes the proxy connection channel 1 s after the proxy command; restored finishes 1.5 s after StartRestore"; }
        else if (!strcmp(g_case, "t12_conn")) { scn.proxy_connect_reqs = 1; scn.proxy_do_cmd = 1; desc = "I6: the external host never completes the connect (SYN_SENT); restored finishes 1.5 s after StartRestore"; }
        else if (!strcmp(g_case, "t12_conn_cancel")) { scn.proxy_connect_reqs = 1; scn.proxy_do_cmd = 1; scn.msg_delay_ms = 40000; desc = "I6: same, restored stays busy; the host cancels while the proxy connect is in SYN_SENT"; }
        else if (!strcmp(g_case, "t12")) { scn.proxy_connect_reqs = 1; scn.proxy_do_cmd = 1; desc = "T12: one proxy connection is open (and silent) when restored finishes"; }
        else { scn.proxy_connect_reqs = 2; scn.proxy_do_cmd = 0; desc = "T13: two connection requests back to back; restored then finishes"; }
    }
    /* T1b: the loop has ended; the device sends an async data request afterwards */
    else if (!strcmp(g_case, "t1b")) {
        msgs[0] = status(0, 0, 0); scn.nmsgs = 1; scn.late_msg = urlasset_req(); scn.data_port_mode = FAKE_DP_OPEN_SILENT; after_s = 2;
        desc = "T1b: restore finishes normally; after the host's Goodbye the device sends an AsyncDataRequestMsg (URLAsset)";
    }
    /* T3: thread creation for the async handler fails (link-time wrapper, one injected failure) */
    else if (!strcmp(g_case, "t3_inject")) {
        msgs[0] = urlasset_req(); msgs[1] = plist_new_dict(); plist_dict_set_item(msgs[1], "MsgType", plist_new_string("FooMsg")); scn.nmsgs = 2;
        scn.data_port_mode = FAKE_DP_OPEN_SILENT; after_s = 2; arm_fail = 1;
        desc = "T3: thread_new for the async handler returns -1 (one injected failure); then an unknown message ends the loop";
    }
    else { printf("unknown case %s\n", g_case); return 2; }

    int lim = getenv("FAKE_CASE_LIMIT_S") ? atoi(getenv("FAKE_CASE_LIMIT_S")) : (!strncmp(g_case, "t5s_bb", 6) || !strncmp(g_case, "t12", 3) ? 45 : 150);
    { char tmpl[] = "/tmp/fake_restore_XXXXXX"; if (mkdtemp(tmpl) && chdir(tmpl) != 0) {} }   /* BasebandUpdaterOutputData writes a file named after the (fake) UDID into the working directory */
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
    if (ext_port) hv_net_guard_allow_local("127.0.0.1", (uint16_t)ext_port);   /* test-side policy entry: the "external host" the fake device names (the reverse-proxy target is checked with the DEFAULT purpose) */
    int fds_before = count_fds(), thr_before = count_threads();
    if (!strcmp(g_case, "t12_conn_cancel")) { pthread_t cp; pthread_create(&cp, NULL, cancel_probe, NULL); pthread_detach(cp); }

    struct idevicerestore_client_t *c = idevicerestore_client_new();
    idevicerestore_set_ecid(c, FAKE_ECID);
    idevicerestore_set_udid(c, FAKE_UDID);
    Arg a = {c, plist_new_dict()};
    if (arm_fail) hv_test_fail_next_thread_new = 1;
    int rc = 12345, ret = hv_restore_session_run(run_fn, &a, &rc);
    double dt = now() - t0;
    printf("RETURNED restore_device=%d at %.1fs\n", rc, dt);
    if (free_client) { idevicerestore_client_free(c); printf("CLIENT FREED (as the host does after restore_device returns)\n"); }
    if (after_s) { printf("WAITING %d s for threads that outlive restore_device\n", after_s); sleep((unsigned)after_s); }
    int fds_after = count_fds();

    HvNetStats ns; hv_net_guard_stats(&ns);
    FakeStats fs; fake_stats(&fs);
    printf("RESULT case=%s run()=%d restore_device=%d elapsed=%.1fs\n", g_case, ret, rc, dt);
    printf("NETGUARD attempts=%d allowed=%d denied=%d (only the fake mux 127.0.0.1:<ephemeral> may be allowed)\n", ns.attempts, ns.allowed, ns.denied);
    printf("FAKE mux_accepted=%d list_devices=%d connect_restored=%d connect_other=%d(last port %d) start_restore=%d goodbye=%d\n",
           fs.accepted, fs.list_devices, fs.connect_restored, fs.connect_other, fs.last_other_port, fs.start_restore, fs.goodbye);
    printf("FAKE data_port_connects=%d data_port_bytes=%d proxy_ctrl=%d proxy_conn=%d proxy_cmd_acked=%d\n", fs.dp_connect, fs.dp_bytes, fs.px_ctrl, fs.px_conn, fs.px_cmd_acked);
    printf("FDS before=%d after=%d (after the extra wait)\n", fds_before, fds_after);
    printf("REG proxy_ctrl_active=%d proxy_conn_active=%d conns_registered=%d THREADS before=%d after=%d\n", hv_reg_active(HV_REG_K_PROXY_CTRL), hv_reg_active(HV_REG_K_PROXY_CONN), hv_reg_conn_count(), thr_before, count_threads());
    printf("THREAD_NEW calls=%d injected_failures=%d joins=%d\n", hv_test_thread_new_calls, hv_test_thread_new_failed, hv_test_thread_join_calls);
    if (fs.late_attempts || !strcmp(g_case, "t1b")) printf("LATE_SEND attempts=%d first rc=%d errno=%d second rc=%d errno=%d (rc 0 = the kernel accepted the bytes; it does not mean the host read them)\n", fs.late_attempts, fs.late_rc1, fs.late_errno1, fs.late_rc2, fs.late_errno2);
    if (getenv("FAKE_LEAKCHECK") && __lsan_do_recoverable_leak_check) { printf("LEAKCHECK running (report on stderr)\n"); fflush(stdout); __lsan_do_recoverable_leak_check(); }
    printf("ENGINE_THREADS_AFTER=%d\n", hv_restore_session_engine_threads());
    fflush(stdout);
    _exit(0);   /* the usbmuxd thread never returns on its own; the verdict is the RESULT line, not the exit status */
}
