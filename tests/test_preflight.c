/* host/restore_preflight.c: the real restore tool (idevicerestore, libirecovery, usbmuxd, libusb stand-in) against the stand-in ROM of tests/test_irecv.c, with a FAKE IPSW
   (a small ZIP with no BuildManifest). Linux only. Scenario in argv[1]:
     ok         - a valid ZIP: the table must say preflight_completed (exit 0)
     no-ipsw    - the path does not exist: ipsw_open must FAIL and the result must be FAILED
     curl       - the ROM stand-in makes one HTTPS request through the curl stand-in while the tool runs: deny mode must refuse it, network_requests must FAIL, and
                  the server (a listener on the port it asked for) must see no connection
     dnload     - the ROM stand-in's side thread sends a DFU DNLOAD during the run: data_transfers must FAIL
     hang       - the ROM stand-in does not answer for 9 s: the run must end FAILED on its cap without freeing the client under a running thread
     progress   - (no tool) injected progress sequences for hv_restore_preflight_progress_verdict, both stages
   Names with the prefix "a-" (a-ok, a-curl, a-dnload, a-no-ipsw) run stop A (stage 1) instead of FLAG_NOACTION; the shell script gives them the fake IPSW that fits.
   The negative control is a second build of this program with -DHV_PREFLIGHT_NEGATIVE_CONTROL (the stop is not set): scenario ok must then FAIL (init/progress).
   Exit status: 0 when the scenario's expectation is met, else 100+. */
#define main irecv_main
#include "test_irecv.c"
#undef main

#include <arpa/inet.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../host/restore_preflight.h"
#include "../host/restore_session.h"
#include "../host/net_guard.h"

void hv_curl_deny_network(int on);

static int g_listen = -1, g_conn_seen;
static void *acceptor(void *arg) {
    (void)arg;
    for (int i = 0; i < 300 && !g_stop; i++) {
        struct timeval tv = {0, 10000};
        fd_set f; FD_ZERO(&f); FD_SET(g_listen, &f);
        if (select(g_listen + 1, &f, NULL, NULL, &tv) > 0) { int c = accept(g_listen, NULL, NULL); if (c >= 0) { g_conn_seen++; close(c); } }
    }
    return NULL;
}
static int g_port;
static void hook_curl(void) {
    char url[96];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/x", g_port);
    CURL *c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 3L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
}
static void *inject_dnload(void *arg) {
    (void)arg;
    libusb_device **l = NULL;
    ssize_t n = libusb_get_device_list(NULL, &l);
    libusb_device_handle *h = NULL;
    if (n > 0 && libusb_open(l[0], &h) == 0) { unsigned char b[16] = {0}; libusb_control_transfer(h, 0x21, 1, 0, 0, b, sizeof b, 2000); libusb_close(h); }
    if (l) libusb_free_device_list(l, 1);
    return NULL;
}
/* runs on the ROM thread, which must answer the injected request; so it only waits until the request is on the log, then returns */
static void hook_dnload(void) {
    static int done;
    if (done++) return;
    int c0 = hv_libusb_ctrl_count();
    pthread_t t;
    if (pthread_create(&t, NULL, inject_dnload, NULL) == 0) pthread_detach(t);
    for (int i = 0; i < 500 && hv_libusb_ctrl_count() == c0; i++) usleep(2000);
}

static void hook_hang(void) { usleep(9000000); }

/* failure injection: things a run must not leave behind, done on the ROM thread while the tool runs */
static const char *g_work;
static void hook_leakfd(void) { int p[2]; if (pipe(p) == 0) { /* never closed */ } }
static void *sleeper(void *a) { (void)a; pthread_setname_np(pthread_self(), "leakthr"); sleep(25); return NULL; }
static void hook_leakthread(void) { pthread_t t; if (pthread_create(&t, NULL, sleeper, NULL) == 0) pthread_detach(t); }
static void wfile(const char *name, const char *content) { char p[1200]; snprintf(p, sizeof p, "%s/%s", g_work, name); FILE *f = fopen(p, "w"); if (f) { fputs(content, f); fclose(f); } }
static void hook_wnew(void) { wfile("new.txt", "x"); }
static void hook_wmod(void) { wfile("pre.txt", "a much longer content than before"); }
static void hook_wdel(void) { char p[1200]; snprintf(p, sizeof p, "%s/pre.txt", g_work); unlink(p); }
static void hook_w513(void) { char n[32]; for (int i = 0; i < 513; i++) { snprintf(n, sizeof n, "many%03d.txt", i); wfile(n, "x"); } }

static int snapshot_tests(void) {
    int fails = 0, f, u, o;
    char d[] = "/tmp/hvsnap-XXXXXX";
    if (!mkdtemp(d)) return 1;
    g_work = d;
    #define SCHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
    int inc = hv_restore_preflight_snapshot_probe(d, &f, &u, &o);
    SCHECK(!inc && f == 0 && !u && !o, "snapshot of an empty directory: complete, 0 files");
    wfile("a.txt", "x");
    inc = hv_restore_preflight_snapshot_probe(d, &f, &u, &o);
    SCHECK(!inc && f == 1, "snapshot sees a file");
    inc = hv_restore_preflight_snapshot_probe("/nonexistent-hvsnap-dir", &f, &u, &o);
    SCHECK(inc && u == 1, "snapshot of a directory that cannot be opened: incomplete, unreadable=%d", u);
    hook_w513();
    inc = hv_restore_preflight_snapshot_probe(d, &f, &u, &o);
    SCHECK(inc && o >= 2 && f == 512, "snapshot with 514 files: incomplete, records 512, overflow=%d", o);
    char sub[300]; snprintf(sub, sizeof sub, "%s/locked", d); mkdir(sub, 0755); chmod(sub, 0);
    if (geteuid() != 0) {
        for (int i = 0; i < 514; i++) { char n[320]; snprintf(n, sizeof n, "%s/many%03d.txt", d, i); unlink(n); }
        inc = hv_restore_preflight_snapshot_probe(d, &f, &u, &o);
        SCHECK(inc && u == 1, "snapshot with a directory without permission: incomplete, unreadable=%d", u);
    } else printf("SKIP unreadable-by-permission case (running as root: chmod 000 does not stop root; the nonexistent-directory case above covers the same opendir failure)\n");
    chmod(sub, 0755);
    return fails;
}

static int progress_tests(void) {
    struct { const char *name; int calls, maxs, lasts; double lastp; int want; } t[] = {
        {"DETECT 0.6 last, nothing later", 7, 0, 0, 0.6, 1},
        {"no callback at all", 0, -1, -1, -1, 0},
        {"PREPARE reached", 9, 1, 1, 0.0, 0},
        {"DETECT but stopped at 0.2", 3, 0, 0, 0.2, 0},
        {"DETECT 0.6 then a later step once", 8, 1, 0, 0.6, 0},
    };
    int fails = 0;
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
        char note[240];
        int got = hv_restore_preflight_progress_verdict(t[k].calls, t[k].maxs, t[k].lasts, t[k].lastp, note, sizeof note);
        printf("%s progress verdict %-36s -> %d (%s)\n", got == t[k].want ? "PASS" : "FAIL", t[k].name, got, note);
        if (got != t[k].want) fails++;
    }
    struct { const char *name; int calls, maxs, lasts; double lastp; int want; } ta[] = {
        {"PREPARE 0.0 last, nothing later", 8, 1, 1, 0.0, 1},
        {"no callback at all", 0, -1, -1, -1, 0},
        {"stopped in DETECT (never reached PREPARE)", 4, 0, 0, 0.6, 0},
        {"PREPARE 0.2 (root filesystem step passed)", 9, 1, 1, 0.2, 0},
        {"a later step (flash firmware)", 12, 4, 4, 0.1, 0},
    };
    for (unsigned k = 0; k < sizeof ta / sizeof ta[0]; k++) {
        char note[240];
        int got = hv_restore_preflight_progress_verdict_a(ta[k].calls, ta[k].maxs, ta[k].lasts, ta[k].lastp, note, sizeof note);
        printf("%s progress verdict (stop A) %-36s -> %d (%s)\n", got == ta[k].want ? "PASS" : "FAIL", ta[k].name, got, note);
        if (got != ta[k].want) fails++;
    }
    return fails;
}

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "ok";
    if (!strcmp(sc, "progress")) return progress_tests();
    if (!strcmp(sc, "snapshot")) return snapshot_tests();
    g_sc = "ok";
    int stage = !strncmp(sc, "a-", 2) ? HV_PF_STOP_A : HV_PF_NOACTION;
    const char *kind = stage == HV_PF_STOP_A ? sc + 2 : sc;
    uint64_t ecid = 0x2d2a8d94f32ba4d2ull;
    g_dev_ecid = ecid;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    pthread_t t, ta;
    if (!strcmp(kind, "curl")) {
        g_listen = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET};
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        bind(g_listen, (struct sockaddr *)&a, sizeof a);
        listen(g_listen, 4);
        socklen_t l = sizeof a;
        getsockname(g_listen, (struct sockaddr *)&a, &l);
        g_port = ntohs(a.sin_port);
        g_hook_serial = hook_curl;
        pthread_create(&ta, NULL, acceptor, NULL);
    }
    if (!strcmp(kind, "dnload")) g_hook_serial = hook_dnload;
    if (!strcmp(kind, "hang")) g_hook_serial = hook_hang;
    g_work = argc > 3 ? argv[3] : ".";
    if (!strcmp(kind, "leakfd")) g_hook_serial = hook_leakfd;
    if (!strcmp(kind, "leakthread")) g_hook_serial = hook_leakthread;
    if (!strcmp(kind, "wnew")) g_hook_serial = hook_wnew;
    if (!strcmp(kind, "wmod")) { g_hook_serial = hook_wmod; wfile("pre.txt", "short"); }
    if (!strcmp(kind, "wdel")) { g_hook_serial = hook_wdel; wfile("pre.txt", "short"); }
    if (!strcmp(kind, "w513")) g_hook_serial = hook_w513;
    pthread_create(&t, NULL, rom, NULL);
    hv_vusb_set_online(g_u, 1);
    const char *ipsw = !strcmp(kind, "no-ipsw") ? "/nonexistent/fake.ipsw" : (argc > 2 ? argv[2] : "fake.ipsw");
    const char *work = argc > 3 ? argv[3] : ".";
    int r = hv_restore_preflight_run(g_u, ipsw, work, ecid, stdout, !strcmp(kind, "hang") ? 1500 : 20000, stage);
    g_stop = 1;
    pthread_join(t, NULL);
    int hang_bad = 0;
    if (!strcmp(kind, "hang")) {
        /* the restore thread did not return (the stand-in ROM sleeps 9 s inside it): the policy must still be in force, a second run must be refused, and the policy
           may be removed only after the thread has ended */
        printf("scenario hang: engine threads after the run: %d, hooks intact: %d\n", hv_restore_session_engine_threads(), hv_net_guard_hooks_intact());
        if (hv_restore_session_engine_threads() != 1 || !hv_net_guard_hooks_intact()) { printf("scenario hang: EXPECTATION NOT MET (policy must stay while the thread runs)\n"); hang_bad = 1; }
        if (hv_restore_session_end() == 0) { printf("scenario hang: EXPECTATION NOT MET (end() removed the policy under a running thread)\n"); hang_bad = 1; }
        int r2 = hv_restore_preflight_run(g_u, ipsw, work, ecid, stdout, 1500, stage);
        printf("scenario hang: second run while the thread runs returned %d (must be refused)\n", r2);
        if (r2 == 0) hang_bad = 1;
        for (int i = 0; i < 300 && hv_restore_session_engine_threads() > 0; i++) usleep(50000);
        printf("scenario hang: thread ended, engine threads %d\n", hv_restore_session_engine_threads());
        if (hv_restore_session_engine_threads() != 0) hang_bad = 1;
        if (hv_restore_session_end() != 0 || hv_net_guard_hooks_intact()) { printf("scenario hang: EXPECTATION NOT MET (policy not removable after the thread ended)\n"); hang_bad = 1; }
        int r3 = hv_restore_preflight_run(g_u, ipsw, work, ecid, stdout, 1500, stage);
        printf("scenario hang: third run (the private usbmuxd of the first one never stopped) returned %d (must be refused)\n", r3);
        if (r3 != -1) hang_bad = 1;
    }
    if (!strcmp(kind, "curl")) { pthread_join(ta, NULL); printf("scenario curl: the local server saw %d connection(s)\n", g_conn_seen); }
    int bad = 0;
    if (!strcmp(kind, "ok")) bad = r != 0;
    else if (!strcmp(kind, "no-ipsw") || !strcmp(kind, "dnload") || !strcmp(kind, "hang")) bad = r < 1;
    else if (!strcmp(kind, "curl")) bad = !(r >= 1 && g_conn_seen == 0);
    else if (!strncmp(kind, "bad-", 4)) bad = r < 1;
    else if (!strcmp(kind, "leakfd") || !strcmp(kind, "leakthread") || !strcmp(kind, "wnew") || !strcmp(kind, "wmod") || !strcmp(kind, "wdel") || !strcmp(kind, "w513")) bad = r < 1;   /* a-bad-component, a-bad-identity: the fake IPSW is wrong on purpose */
    if (hang_bad) bad = 1;
    printf("scenario %s: preflight returned %d -> %s\n", sc, r, bad ? "EXPECTATION NOT MET" : "as expected");
    return bad ? 100 + (r > 0 ? r : 0) : 0;
}
