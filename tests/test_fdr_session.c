/* The FDR proxy and the URLAsset step of the real restore tool (third_party/idevicerestore fdr.c / restore.c) against a FAKE DEVICE, with host/net_guard.c and
   host/restore_session.c in force; and the session's own rules (the tool cannot start without the policy; the policy cannot go while a thread runs).
   Linux. Nothing leaves the machine: every listener is on the loopback interface, and the device is one end of a socketpair.
     fdr      - device-chosen proxy targets: the private usbmuxd's endpoint, malformed packets, a 130-byte name (the unsigned length)
     urlasset - device-chosen URLs: http, user info, loopback literal, an approved NAME that resolves to loopback
     session  - begin/run/end rules and a race between a running engine thread and end() */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <plist/plist.h>
#include <libimobiledevice-glue/socket.h>

#include "idevice.h"        /* third_party/libimobiledevice/src: struct idevice_connection_private */
#include "common.h"
#include "fdr.h"
#include "idevicerestore.h"
#include "log.h"

#include "../devices/pcie.h"
#include "../devices/virtio_usb.h"
#include "../host/net_guard.h"
#include "../host/restore_session.h"

/* virtio_usb.c's links to the guest-memory side of the machine: never reached here (the restore stack's libusb stand-in pulls it in) */
int hv_virtq_pop(HvVirtio *v, int q, HvVirtqElem *e) { (void)v, (void)q, (void)e; return 0; }
void hv_virtq_push(HvVirtio *v, int q, const HvVirtqElem *e, uint32_t w) { (void)v, (void)q, (void)e, (void)w; }
void hv_virtio_interrupt(HvVirtio *v) { (void)v; }
void hv_virtio_init(HvVirtio *v, HvMachine *m, int id, uint32_t cc, int nq, uint64_t f) { (void)v, (void)m, (void)id, (void)cc, (void)nq, (void)f; }
uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t o, void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t o, const void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_writable(const HvVirtqElem *e) { (void)e; return 0; }
int hv_pcie_attach(HvPcie *p, HvPciDev *d) { (void)p, (void)d; return -1; }
void hv_machine_wake(HvMachine *m) { (void)m; }

int restore_send_url_asset(struct idevicerestore_client_t *client, plist_t message);

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static int listener(int *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET}; inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(s, (void *)&a, sizeof a); listen(s, 4);
    socklen_t l = sizeof a; getsockname(s, (void *)&a, &l); *port = ntohs(a.sin_port);
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}
static int pending(int ls) { usleep(200000); int c = accept(ls, NULL, NULL); if (c >= 0) { close(c); return 1; } return 0; }

/* what the tool logs (to find "malformed") */
static char g_logbuf[4096];
static void log_cap(enum loglevel level, const char *fmt, va_list ap) {
    char line[512]; vsnprintf(line, sizeof line, fmt, ap); (void)level;
    if (strlen(g_logbuf) + strlen(line) < sizeof g_logbuf - 1) strcat(g_logbuf, line);
}

/* ---- the fake device: one end of a socketpair; it sends the FDR proxy command and a packet, then closes after a moment ---- */
struct dev { int fd; unsigned char pkt[600]; size_t n; };
static void *dev_thread(void *p) {
    struct dev *d = p;
    uint16_t cmd = 0x105;
    send(d->fd, &cmd, 2, 0);
    usleep(30000);
    if (d->n) send(d->fd, d->pkt, d->n, 0);
    usleep(600000);   /* long enough for a connect attempt; then the device goes away, which ends the proxy loop */
    close(d->fd);
    return NULL;
}
static size_t mkpkt(unsigned char *o, const char *host, size_t hostlen, unsigned port, size_t declared_len) {
    o[0] = 0; o[1] = 3; o[2] = (unsigned char)declared_len;
    memcpy(o + 3, host, hostlen);
    o[3 + hostlen] = (unsigned char)(port >> 8); o[4 + hostlen] = (unsigned char)port;
    return 5 + hostlen;
}
static int run_fdr(const unsigned char *pkt, size_t n) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct idevice_connection_private conn; memset(&conn, 0, sizeof conn);
    conn.type = CONNECTION_NETWORK; conn.data = (void *)(uintptr_t)sv[0]; conn.ssl_recv_timeout = (unsigned)-1;
    struct fdr_client fdr; memset(&fdr, 0, sizeof fdr); fdr.connection = &conn;
    struct dev d = {.fd = sv[1], .n = n}; memcpy(d.pkt, pkt, n);
    pthread_t t; pthread_create(&t, NULL, dev_thread, &d);
    g_logbuf[0] = 0;
    int rc = fdr_poll_and_handle_message(&fdr);
    pthread_join(t, NULL);
    close(sv[0]);
    return rc;
}

static int fdr_tests(void) {
    int port, lp; lp = listener(&port);
    logger_set_print_func(log_cap);
    setenv("RES_OPTIONS", "timeout:1 attempts:1", 1);   /* a name lookup that cannot succeed here ends quickly */
    CHECK(hv_restore_session_begin() == 0, "session begun: the policy is in force");
    hv_restore_session_allow_usbmuxd((uint16_t)port);
    unsigned char p[600]; size_t n;
    HvNetStats s0, s1;

    /* the registered endpoint works for the DEFAULT purpose (it is what libusbmuxd uses)... */
    int c0 = socket_connect("127.0.0.1", (uint16_t)port);
    CHECK(c0 >= 0 && pending(lp), "control: a DEFAULT-purpose connection to the registered usbmuxd endpoint is allowed");
    if (c0 >= 0) close(c0);

    /* ...but a proxy target the DEVICE names is refused even when it is exactly that endpoint */
    n = mkpkt(p, "127.0.0.1", 9, (unsigned)port, 9);
    hv_net_guard_stats(&s0);
    int rc = run_fdr(p, n);
    hv_net_guard_stats(&s1);
    char dl[400]; hv_net_guard_denied_log(dl, sizeof dl);
    CHECK(rc == -1 && !pending(lp), "FDR proxy to the private usbmuxd's own address:port: refused (rc %d) and the listener saw no connection", rc);
    CHECK(s1.denied > s0.denied && strstr(dl, "loopback address"), "the refusal is counted and logged as a loopback address");
    n = mkpkt(p, "127.0.0.1", 9, (unsigned)port + 1, 9);
    rc = run_fdr(p, n);
    CHECK(rc == -1, "FDR proxy to another loopback port: refused (rc %d)", rc);

    /* malformed packets: no connection is even attempted */
    struct { const char *name; unsigned char b[64]; size_t n; } bad[] = {
        {"3 bytes only (0 3 5)", {0, 3, 5}, 3},
        {"name length larger than the packet", {0, 3, 200, 'a', 'b', 0, 80}, 7},
        {"name length smaller than the packet", {0, 3, 1, 'a', 'b', 'c', 'd', 0, 80}, 9},
        {"zero name length", {0, 3, 0, 0, 80}, 5},
        {"255 declared, 8 received", {0, 3, 255, 'a', 'b', 'c', 0, 80}, 8},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        hv_net_guard_stats(&s0);
        rc = run_fdr(bad[i].b, bad[i].n);
        hv_net_guard_stats(&s1);
        CHECK(rc == 0 && s1.attempts == s0.attempts && !pending(lp), "malformed (%s): ignored (rc %d), no connection attempted", bad[i].name, rc);
    }

    /* a name of 130 bytes is a valid length (it was read as negative before the unsigned fix) and is NOT reported as malformed */
    char longname[131]; memset(longname, 'a', 130); longname[130] = 0;
    n = mkpkt(p, longname, 130, 443, 130);
    rc = run_fdr(p, n);
    CHECK(!strstr(g_logbuf, "malformed"), "a 130-byte name with a matching packet length is not treated as malformed (rc %d)", rc);

    /* control: with the policy removed the same proxy command does connect, so the refusals above are the policy's doing */
    CHECK(hv_restore_session_end() == 0, "session ended: the hooks are removed");
    n = mkpkt(p, "127.0.0.1", 9, (unsigned)port, 9);
    rc = run_fdr(p, n);
    CHECK(pending(lp), "control: without the policy the same FDR packet reaches the listener (rc %d)", rc);
    close(lp);
    return 0;
}

/* ---- URLAsset ---- */
/* a listener that counts connections and closes each at once (so the control run's TLS handshake fails promptly instead of waiting for a server that never answers) */
static volatile int g_acc_count, g_acc_stop;
static int g_acc_fd;
static void *acceptor(void *a) {
    (void)a;
    while (!g_acc_stop) {
        int c = accept(g_acc_fd, NULL, NULL);
        if (c >= 0) { g_acc_count++; close(c); } else usleep(10000);
    }
    return NULL;
}
static plist_t msg(const char *method, const char *url) {
    plist_t m = plist_new_dict(), a = plist_new_dict();
    plist_dict_set_item(a, "RequestMethod", plist_new_string(method));
    plist_dict_set_item(a, "RequestURL", plist_new_string(url));
    plist_dict_set_item(m, "Arguments", a);
    return m;
}
static int urlasset_tests(void) {
    int port, lp = listener(&port);
    char url[160];
    g_acc_fd = lp; g_acc_count = 0; g_acc_stop = 0;
    pthread_t acc; pthread_create(&acc, NULL, acceptor, NULL);
    struct idevicerestore_client_t *cl = calloc(1, sizeof *cl);   /* no restore session: the reply cannot be sent, the request is what is tested */
    CHECK(hv_restore_session_begin() == 0, "session begun");
    hv_restore_session_allow_usbmuxd((uint16_t)port);
    char nm[] = "localhost";
    hv_net_guard_allow_host(nm, (uint16_t)port);   /* an approved NAME... */
    HvNetStats s0, s1;
    const char *urls[] = {"http://127.0.0.1:%d/x", "https://127.0.0.1:%d/x", "https://user:pw@localhost:%d/x", "https://[::1]:%d/x"};
    const char *what[] = {"http URL to the usbmuxd endpoint", "https literal 127.0.0.1 at the usbmuxd endpoint", "user info", "IPv6 literal"};
    for (int i = 0; i < 4; i++) {
        snprintf(url, sizeof url, urls[i], port);
        plist_t m = msg("GET", url);
        hv_net_guard_stats(&s0);
        int rc = restore_send_url_asset(cl, m);
        hv_net_guard_stats(&s1);
        CHECK(s1.url_denied == s0.url_denied + 1 && s1.attempts == s0.attempts && g_acc_count == 0, "URLAsset %s: refused by the URL policy, nothing opened (rc %d)", what[i], rc);
        plist_free(m);
    }
    plist_t mp = msg("POST", "https://localhost/x");
    hv_net_guard_stats(&s0);
    int rc = restore_send_url_asset(cl, mp);
    hv_net_guard_stats(&s1);
    CHECK(rc == -1 && s1.url_attempts == s0.url_attempts, "URLAsset with a method other than GET: rejected before the policy is even asked (rc %d)", rc);
    plist_free(mp);
    /* ...that resolves to loopback: the URL policy lets the name through, the connection check does not (the resolved address decides) */
    snprintf(url, sizeof url, "https://localhost:%d/x", port);
    plist_t m = msg("GET", url);
    hv_net_guard_stats(&s0);
    rc = restore_send_url_asset(cl, m);
    hv_net_guard_stats(&s1);
    CHECK(s1.url_denied == s0.url_denied && s1.denied > s0.denied && g_acc_count == 0, "an approved NAME that resolves to the registered usbmuxd endpoint: the URL is allowed, the connection is refused, the listener saw nothing (rc %d)", rc);
    /* control */
    CHECK(hv_restore_session_end() == 0, "session ended");
    hv_net_guard_stats(&s0);
    rc = restore_send_url_asset(cl, m);
    CHECK(usleep(300000) == 0 && g_acc_count > 0, "control: without the policy the same request reaches the listener");
    plist_free(m);
    g_acc_stop = 1; pthread_join(acc, NULL);
    free(cl); close(lp);
    return 0;
}

/* ---- the session ---- */
static int g_ran;
static int fn_quick(void *a) { (void)a; g_ran++; return 7; }
struct blk { int rd; };
static int fn_block(void *a) { struct blk *b = a; char c; g_ran++; return read(b->rd, &c, 1) == 1 ? 9 : -9; }
static volatile int g_race_bad;
static int fn_check(void *a) { (void)a; if (!hv_net_guard_hooks_intact()) g_race_bad++; usleep(50); if (!hv_net_guard_hooks_intact()) g_race_bad++; return 0; }
struct rt { int ran; };
static void *race_thread(void *a) { struct rt *r = a; int rc; r->ran = hv_restore_session_run(fn_check, NULL, &rc) == 0; return NULL; }
struct bt { int ret, rc; int (*fn)(void *); void *arg; };
static void *run_thread(void *a) { struct bt *b = a; b->ret = hv_restore_session_run(b->fn, b->arg, &b->rc); return NULL; }

static int session_tests(void) {
    int rc = 0;
    g_ran = 0;
    CHECK(hv_restore_session_run(fn_quick, NULL, &rc) == -1 && g_ran == 0, "run without begin: refused, the function was not called");
    CHECK(hv_restore_session_begin() == 0 && hv_net_guard_hooks_intact(), "begin installs the policy and verifies it");
    CHECK(hv_restore_session_run(fn_quick, NULL, &rc) == 0 && rc == 7 && g_ran == 1 && hv_restore_session_engine_threads() == 0, "run: the function ran, its result came back, the thread is unregistered again");
    /* a replaced hook is not "intact" */
    int (*saved)(const struct sockaddr *, socklen_t) = limd_glue_connect_guard;
    limd_glue_connect_guard = NULL;
    CHECK(hv_restore_session_run(fn_quick, NULL, &rc) == -1 && g_ran == 1, "run with one hook removed from outside: refused, the function was not called");
    limd_glue_connect_guard = saved;
    CHECK(hv_net_guard_hooks_intact(), "hook put back: intact again");

    /* a thread that is still running keeps the policy */
    int pf[2]; pipe(pf);
    struct blk b = {pf[0]};
    struct bt t1 = {.fn = fn_block, .arg = &b};
    pthread_t th; pthread_create(&th, NULL, run_thread, &t1);
    for (int i = 0; i < 100 && hv_restore_session_engine_threads() == 0; i++) usleep(10000);
    CHECK(hv_restore_session_engine_threads() == 1, "an engine thread is running (registered)");
    CHECK(hv_restore_session_end() == -1 && hv_net_guard_hooks_intact(), "end() while it runs: refused, the policy stays");
    CHECK(hv_restore_session_begin() == -1 && hv_net_guard_hooks_intact(), "begin() while it runs: refused (the lists are not reset under it)");
    int pt; int lp = listener(&pt);
    hv_net_guard_allow_local("127.0.0.1", (uint16_t)pt);
    limd_glue_connect_purpose = LIMD_GLUE_PURPOSE_FDR;
    int sfd = socket_connect("127.0.0.1", (uint16_t)pt);
    limd_glue_connect_purpose = LIMD_GLUE_PURPOSE_DEFAULT;
    CHECK(sfd < 0 && !pending(lp), "meanwhile an FDR-purpose connection to a registered endpoint is still refused");
    close(lp);
    write(pf[1], "x", 1);
    pthread_join(th, NULL);
    CHECK(t1.ret == 0 && t1.rc == 9 && hv_restore_session_engine_threads() == 0, "released: the function returned 9, the thread is unregistered");
    CHECK(hv_restore_session_end() == 0 && !hv_net_guard_hooks_intact() && limd_glue_connect_guard == NULL, "end() after it returned: the policy is removed");
    close(pf[0]); close(pf[1]);

    /* a race between a starting engine thread and end(): whenever the function runs, the policy is intact */
    int ran = 0, refused = 0;
    for (int i = 0; i < 400; i++) {
        hv_restore_session_begin();
        struct rt r = {0};
        pthread_t t; pthread_create(&t, NULL, race_thread, &r);
        if (i % 2) usleep(i % 7);
        int first = hv_restore_session_end();
        pthread_join(t, NULL);
        if (first != 0) { if (hv_restore_session_end() != 0) g_race_bad += 1000; }   /* after the join it must be removable */
        if (r.ran) ran++; else refused++;
        if (hv_restore_session_engine_threads() != 0) g_race_bad += 1000;
    }
    CHECK(g_race_bad == 0, "400 rounds of run vs end(): the policy was intact whenever the function ran (ran %d, refused %d), no thread left registered", ran, refused);
    return 0;
}

int main(int argc, char **argv) {
    const char *w = argc > 1 ? argv[1] : "all";
    if (!strcmp(w, "all") || !strcmp(w, "fdr")) fdr_tests();
    if (!strcmp(w, "all") || !strcmp(w, "urlasset")) urlasset_tests();
    if (!strcmp(w, "all") || !strcmp(w, "session")) session_tests();
    printf("%s (%d failed)\n", g_fail ? "FDR/SESSION FAILED" : "FDR/SESSION OK", g_fail);
    return g_fail ? 1 : 0;
}
