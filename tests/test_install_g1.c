/* G1 (host/install_g1.c): the real host/install.c (-D__ANDROID__ -DHV_INSTALL_G1), the real session and connection policy, the real restore tool, the stand-in ROM of
   tests/test_irecv.c and a fake IPSW, against the synthetic signing server tests/g1_tss_server.py (TLS on 127.0.0.1, a test CA). Linux only.
   Usage: t SCENARIO IPSW WORKDIR PORT      (HV_CA_PATH is set by tools/install-g1-test.sh)
   Scenarios (each prints the G1 table; exit 0 = the table passed, 1 = a row FAILED, 2 = setup):
     ok        three good replies: the tool returns 0
     refuse1   the first reply is a refusal (STATUS=8): -1 after one request
     refuse2   the second request is refused: -1 after two requests
     bad       a reply without STATUS: -1
     connfail  nothing listens on the port: -1
     timeout   the server stays silent: -1 within the time limit
     badcert   the server's certificate is not trusted (HV_CA_PATH = another certificate): -1, the server sees nothing
     redirect  the server answers 302 to plain http: -1
     external  the signing URL points at a public address: the policy refuses it, nothing is sent
     noallow   the test port is not approved: the policy refuses it, the server sees nothing
   Controls (the checks must catch them: exit 1):
     c-noallow-asok   as noallow, but the table expects the "ok" outcome
     c-tickets        ok, with ticket files enabled: the tool writes its ticket file
     c-dnload         ok, and a DFU DNLOAD reaches the device while the tool runs
     c-injfile        ok, and a file named like a ticket appears in the work directory
     c-showlog        ok, with the tool's log filter switched off: the tool_log row must FAIL and the synthetic nonce must show
     filter           the log filter on boundary inputs (no run): allowed line shown, other line withheld, ECID/nonce/long hex withheld, whitelisted prefix + hex withheld
     c-filter         the same with the filter switched off: the cases must FAIL (it shows that the test notices a filter that lets everything through)
   Without a "c-" prefix an exit status other than 0 means the scenario's expectation was not met. */
#define main irecv_main
#include "test_irecv.c"
#undef main

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../host/install.h"
#include "../host/install_g1.h"
#include "../host/net_guard.h"

void tss_set_max_attempts(int n);
void tss_set_timeouts(long connect_s, long total_s);

static const char *g_work;
static void wfile(const char *name, const char *content) { char p[1200]; snprintf(p, sizeof p, "%s/%s", g_work, name); FILE *f = fopen(p, "w"); if (f) { fputs(content, f); fclose(f); } }
static void hook_injfile(void) { static int done; if (!done++) wfile("stray.shsh", "x"); }
static void *inject_dnload(void *arg) {
    (void)arg;
    libusb_device **l = NULL;
    ssize_t n = libusb_get_device_list(NULL, &l);
    libusb_device_handle *h = NULL;
    if (n > 0 && libusb_open(l[0], &h) == 0) { unsigned char b[16] = {0}; libusb_control_transfer(h, 0x21, 1, 0, 0, b, sizeof b, 2000); libusb_close(h); }
    if (l) libusb_free_device_list(l, 1);
    return NULL;
}
static void hook_dnload(void) {
    static int done;
    if (done++) return;
    int c0 = hv_libusb_ctrl_count();
    pthread_t t;
    if (pthread_create(&t, NULL, inject_dnload, NULL) == 0) pthread_detach(t);
    for (int i = 0; i < 500 && hv_libusb_ctrl_count() == c0; i++) usleep(2000);
}

/* Direct test of the log filter. The inputs are synthetic and never printed back; each case reports its number and the counters' change only. */
static int filter_cases(void) {
    struct { const char *line; int shown; int sens; const char *what; } c[] = {
        {"Identified device as vma2macosap, VirtualMac2,1\n", 1, 0, "a known-harmless line is shown"},
        {"Checking IPSW for required components...\n", 1, 0, "another known-harmless line is shown"},
        {"Some other message the tool might print\n", 0, 0, "a line outside the list is withheld"},
        {"ECID: 9999999999\n", 0, 1, "an ECID line is withheld"},
        {"Nonce: a1 b2 c3 d4 e5 f6 07 18 29 3a 4b 5c\n", 0, 1, "a nonce line is withheld"},
        {"a1 b2 c3 d4 e5 f6 07 18 29 3a 4b 5c 6d 7e 8f 90\n", 0, 1, "a spaced hex dump is withheld"},
        {"0123456789abcdef0123456789abcdef0123456789abcdef\n", 0, 1, "a long run of hex digits is withheld"},
        {"Identified device as a1 b2 c3 d4 e5 f6 07 18 29 3a 4b\n", 0, 1, "a known prefix followed by a hex dump is withheld"},
        {"Device Product Version: 0123456789abcdef0123456789abcdef0123\n", 0, 1, "a known prefix followed by a long hex run is withheld"},
        {"ApNonce 0102\n", 0, 1, "an ApNonce line is withheld"},
    };
    int bad = 0, n = (int)(sizeof c / sizeof c[0]);
    for (int k = 0; k < n; k++) {
        int s0, w0, e0, b0, s1, w1, e1, b1;
        hv_g1_log_counts(&s0, &w0, &e0, &b0);
        int r = hv_g1_filter_line(c[k].line);
        hv_g1_log_counts(&s1, &w1, &e1, &b1);
        int ok = r == c[k].shown && (s1 - s0) == (c[k].shown ? 1 : 0) && (w1 - w0) == (c[k].shown ? 0 : 1) && (e1 - e0) == c[k].sens && b1 == b0;
        printf("install g1 filter: case %2d %-6s %s\n", k + 1, ok ? "ok" : "FAILED", c[k].what);
        if (!ok) bad++;
    }
    printf("install g1 filter: %d case(s), %d failed\n", n, bad);
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s SCENARIO IPSW WORKDIR PORT\n", argv[0]); return 2; }
    const char *sc = argv[1], *ipsw = argv[2], *work = argv[3];
    if (!strcmp(sc, "filter") || !strcmp(sc, "c-filter")) {
        if (!strcmp(sc, "c-filter")) hv_g1_set_log_bypass(1);
        return filter_cases() ? 1 : 0;
    }
    int port = atoi(argv[4]);
    g_work = work;
    const char *kind = !strncmp(sc, "c-", 2) ? sc + 2 : sc;
    char url[200];
    const char *path = "/ok";
    HvG1Expect x = {1, 3, 3, 0, 0, HV_G1D_SUCCESS, -1};
    int approve = port;
    if (!strcmp(kind, "refuse1")) { path = "/refuse"; x = (HvG1Expect){0, 1, 1, 0, 0, HV_G1D_STATUS, 8}; }
    else if (!strcmp(kind, "refuse2")) { path = "/refuse2"; x = (HvG1Expect){0, 2, 2, 0, 0, HV_G1D_STATUS, 8}; }
    else if (!strcmp(kind, "bad")) { path = "/bad"; x = (HvG1Expect){0, 1, 2, 0, 0, HV_G1D_NOT_REPLY, -1}; tss_set_max_attempts(2); }
    else if (!strcmp(kind, "connfail")) { path = "/ok"; x = (HvG1Expect){0, 1, 2, 0, 0, HV_G1D_TRANSPORT, 7}; tss_set_max_attempts(2); }
    else if (!strcmp(kind, "timeout")) { path = "/slow"; x = (HvG1Expect){0, 1, 2, 0, 0, HV_G1D_TRANSPORT, 28}; tss_set_max_attempts(2); tss_set_timeouts(2, 3); }
    else if (!strcmp(kind, "badcert")) { path = "/ok"; x = (HvG1Expect){0, 1, 1, 0, 0, HV_G1D_TRANSPORT, 60}; }
    else if (!strcmp(kind, "redirect")) { path = "/redirect"; x = (HvG1Expect){0, 1, 2, 0, 0, HV_G1D_NOT_REPLY, -1}; tss_set_max_attempts(2); }
    else if (!strcmp(kind, "noallow")) { path = "/ok"; approve = 0; x = (HvG1Expect){0, 1, 1, 1, 1, HV_G1D_TRANSPORT, 1}; }
    else if (!strcmp(kind, "external")) { approve = port; x = (HvG1Expect){0, 1, 1, 1, 1, HV_G1D_TRANSPORT, 1}; }
    else if (!strcmp(kind, "noallow-asok")) { approve = 0; }   /* c-noallow-asok: expectations stay those of "ok" */
    else if (!strcmp(kind, "showlog")) { hv_g1_set_log_bypass(1); }
    else if (!strcmp(kind, "tickets") || !strcmp(kind, "dnload") || !strcmp(kind, "injfile") || !strcmp(kind, "ok")) {}
    else { fprintf(stderr, "unknown scenario %s\n", sc); return 2; }
    if (!strcmp(kind, "external")) snprintf(url, sizeof url, "https://192.0.2.1:443/ok");   /* TEST-NET-1: a bare public address; the policy refuses it before any packet */
    else snprintf(url, sizeof url, "https://127.0.0.1:%d%s", port, path);
    hv_g1_set_server(url, approve);
    if (!strcmp(kind, "tickets")) hv_g1_test_allow_tickets(1);
    if (!strcmp(kind, "dnload")) g_hook_serial = hook_dnload;
    if (!strcmp(kind, "injfile")) g_hook_serial = hook_injfile;

    g_sc = "ok";
    uint64_t ecid = 0x2d2a8d94f32ba4d2ull;
    g_dev_ecid = ecid;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    pthread_t t;
    pthread_create(&t, NULL, rom, NULL);
    hv_vusb_set_online(g_u, 1);
    int rc = 2;
    if (hv_g1_begin(work, ecid) == 0 && hv_g1_start(g_u, ipsw, work, ecid) == 0) {
        hv_g1_wait(120000);
        rc = hv_g1_finish(stdout, work, &x) ? 1 : 0;
    }
    g_stop = 1;
    pthread_join(t, NULL);
    printf("scenario %s: %s\n", sc, rc == 0 ? "the G1 table passed" : rc == 1 ? "a G1 row FAILED" : "setup problem");
    return rc;
}
