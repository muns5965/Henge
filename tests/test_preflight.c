/* host/restore_preflight.c: the real restore tool (idevicerestore, libirecovery, usbmuxd, libusb stand-in) against the stand-in ROM of tests/test_irecv.c, with a FAKE IPSW
   (a small ZIP with no BuildManifest). Linux only. Scenario in argv[1]:
     ok         - a valid ZIP: the table must say preflight_completed (exit 0)
     no-ipsw    - the path does not exist: ipsw_open must FAIL and the result must be FAILED
     curl       - the ROM stand-in makes one HTTPS request through the curl stand-in while the tool runs: deny mode must refuse it, network_requests must FAIL, and
                  the server (a listener on the port it asked for) must see no connection
     dnload     - the ROM stand-in's side thread sends a DFU DNLOAD during the run: data_transfers must FAIL
     progress   - (no tool) injected progress sequences for hv_restore_preflight_progress_verdict
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
    return fails;
}

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "ok";
    if (!strcmp(sc, "progress")) return progress_tests();
    g_sc = "ok";
    uint64_t ecid = 0x2d2a8d94f32ba4d2ull;
    g_dev_ecid = ecid;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    pthread_t t, ta;
    if (!strcmp(sc, "curl")) {
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
    if (!strcmp(sc, "dnload")) g_hook_serial = hook_dnload;
    pthread_create(&t, NULL, rom, NULL);
    hv_vusb_set_online(g_u, 1);
    const char *ipsw = !strcmp(sc, "no-ipsw") ? "/nonexistent/fake.ipsw" : (argc > 2 ? argv[2] : "fake.ipsw");
    const char *work = argc > 3 ? argv[3] : ".";
    int r = hv_restore_preflight_run(g_u, ipsw, work, ecid, stdout, 20000);
    g_stop = 1;
    pthread_join(t, NULL);
    if (!strcmp(sc, "curl")) { pthread_join(ta, NULL); printf("scenario curl: the local server saw %d connection(s)\n", g_conn_seen); }
    int bad = 0;
    if (!strcmp(sc, "ok")) bad = r != 0;
    else if (!strcmp(sc, "no-ipsw")) bad = r < 1;
    else if (!strcmp(sc, "curl")) bad = !(r >= 1 && g_conn_seen == 0);
    else if (!strcmp(sc, "dnload")) bad = r < 1;
    printf("scenario %s: preflight returned %d -> %s\n", sc, r, bad ? "EXPECTATION NOT MET" : "as expected");
    return bad ? 100 + (r > 0 ? r : 0) : 0;
}
