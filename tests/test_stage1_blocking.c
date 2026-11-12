/* STAGE 1 (docs/i3-i6-decision-table-2026-11-12.md, sections 6~9): evidence gathering BEFORE any vendored change. Linux only. RECORDS observed behaviour: the exit status says the
   case ran, not that the behaviour is good. Nothing here is linked into the app; no product or vendored source is changed.
     pf_*   partial frames: what internal_plist_receive_timeout (property_list_service) does when a frame arrives in pieces, with a 1 s receive timeout
     s1/s5  shutdown() of a socket another thread is blocked in recv on (untimed / timed);  s1b the same through an idevice connection (the ASR path)
     s2     shutdown() with a thread blocked in send;  s3 shutdown() of a socket in connect();  s3lib the library's socket_connect (is the fd reachable? any timeout?)
     s4     libusbmuxd device monitor: unsubscribe against a silent usbmuxd;  s4j the same with ONE injected pthread_join failure (link-time wrap)
   Usage: test_stage1_blocking <case> */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <plist/plist.h>
#include <libimobiledevice-glue/socket.h>
#include <libimobiledevice-glue/thread.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/restore.h>
#include <usbmuxd.h>
#include "property_list_service.h"
#include "restore.h"
#include "fake_device/fake_dev.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static void *watchdog(void *a) { sleep((unsigned)(intptr_t)a); printf("WATCHDOG: the case did not finish\n"); fflush(stdout); _exit(3); return NULL; }

/* ---------------- partial frames ---------------- */
static plist_t msg(const char *type, int n) { plist_t d = plist_new_dict(); plist_dict_set_item(d, "MsgType", plist_new_string(type)); plist_dict_set_item(d, "Status", plist_new_uint(n)); return d; }
static int pf_case(const char *c) {
    FakeScenario scn; memset(&scn, 0, sizeof scn);
    plist_t msgs[2]; msgs[0] = msg("StatusMsg", 11); msgs[1] = msg("StatusMsg", 22); scn.msgs = msgs; scn.nmsgs = 2; scn.end = FAKE_END_HOLD;
    const char *what = "";
    if (!strcmp(c, "pf_full")) { what = "control: two whole frames, no delay"; }
    else if (!strcmp(c, "pf_idle")) { scn.nmsgs = 0; what = "control: nothing is sent (idle)"; }
    else if (!strncmp(c, "pf_p", 4)) { scn.split_prefix = c[4] - '0'; scn.split_delay_ms = 2500; what = "the first frame's 4-byte length is cut after N bytes, 2500 ms pause, rest"; }
    else if (!strcmp(c, "pf_b")) { scn.split_body = 10; scn.split_delay_ms = 2500; what = "the length is whole, the body is cut after 10 bytes, 2500 ms pause, rest"; }
    else return 2;
    int port = fake_mux_start(&scn);
    char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%d", port);
    setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);
    idevice_t dev = NULL; restored_client_t rc = NULL;
    if (idevice_new_with_options(&dev, FAKE_UDID, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS) { printf("PF_SETUP idevice_new failed\n"); return 2; }
    if (restored_client_new(dev, &rc, "stage1") != RESTORE_E_SUCCESS) { printf("PF_SETUP restored_client_new failed\n"); return 2; }
    plist_t opts = plist_new_dict();
    restored_start_restore(rc, opts, 15);
    plist_free(opts);
    printf("PF_CASE %s: %s (receive timeout 1000 ms)\n", c, what);
    property_list_service_client_t pl = ((struct restored_client_private *)rc)->parent;
    double t0 = now(); int got = 0; int seq[2] = {0, 0};
    for (int i = 1; i <= 8 && got < 2; i++) {
        plist_t p = NULL;
        int r = property_list_service_receive_plist_with_timeout(pl, &p, 1000);
        uint64_t st = 0; char *mt = NULL;
        if (p) { plist_t s = plist_dict_get_item(p, "Status"); if (s) plist_get_uint_val(s, &st); plist_t m = plist_dict_get_item(p, "MsgType"); if (m) plist_get_string_val(m, &mt); }
        printf("PF_CALL %d: t=%.0f ms rc=%d (%s) plist=%s%s%s status=%llu\n", i, now() - t0, r, r == 0 ? "ok" : r == -5 ? "timeout" : "error", p ? "yes" : "no", mt ? " MsgType=" : "", mt ? mt : "", (unsigned long long)st);
        if (p && mt && !strcmp(mt, "StatusMsg") && got < 2) { seq[got++] = (int)st; }
        free(mt); if (p) plist_free(p);
        if (!p && r != -5 && i >= 3) break;   /* a hard error, stop */
    }
    int expect = scn.nmsgs;
    printf("PF_VERDICT %s: expected %d frame(s), received %d, statuses [%d,%d] -> %s\n", c, expect, got, seq[0], seq[1],
           got == expect && (expect < 1 || seq[0] == 11) && (expect < 2 || seq[1] == 22) ? "FRAMES_INTACT" : "FRAMES_LOST_OR_DESYNCED");
    return 0;
}

/* ---------------- shutdown cases ---------------- */
struct blk { int fd; int op; int done; long ret; int err; double t_end; unsigned timeout; idevice_connection_t conn; };
static void *blk_thread(void *a) {
    struct blk *b = a; char buf[64]; static char big[65536];
    b->ret = 0; errno = 0;
    if (b->op == 0) b->ret = socket_receive(b->fd, buf, 1);
    else if (b->op == 1) b->ret = socket_receive_timeout(b->fd, buf, 1, 0, b->timeout);
    else if (b->op == 2) { long total = 0; for (;;) { int r = socket_send(b->fd, big, sizeof big); if (r <= 0) { b->ret = r; break; } total += r; if (total > (1L << 30)) { b->ret = -999; break; } } b->err = errno; }
    else if (b->op == 3) { struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)b->timeout); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); b->ret = connect(b->fd, (struct sockaddr *)&sa, sizeof sa); }
    else if (b->op == 4) { uint32_t n = 0; b->ret = idevice_connection_receive(b->conn, buf, 1, &n); }
    if (b->op != 2) b->err = errno;
    b->t_end = now(); b->done = 1;
    return NULL;
}
static int wait_done(struct blk *b, int ms) { for (int i = 0; i < ms / 10 && !b->done; i++) usleep(10000); return b->done; }
/* runs one blocking operation, first WITHOUT shutdown (control: must stay blocked), then shutdown, and reports */
static void run_blk(const char *name, struct blk *b, int do_shutdown_fd, int (*shut)(int, int)) {
    pthread_t t; double t0 = now();
    pthread_create(&t, NULL, blk_thread, b);
    int ctl = wait_done(b, 1500);
    printf("SD_CONTROL %s: after 1500 ms without shutdown the call has %s\n", name, ctl ? "RETURNED on its own (not a valid blocking control)" : "not returned (blocked, as intended)");
    if (!ctl) {
        double ts = now();
        shut(do_shutdown_fd, SHUT_RDWR);
        int ok = wait_done(b, 5000);
        if (ok) { pthread_join(t, NULL); printf("SD_RESULT %s: shutdown() -> the blocked call RETURNED after %.0f ms, ret=%ld errno=%d (%s)\n", name, b->t_end - ts, b->ret, b->err, strerror(b->err)); }
        else printf("SD_RESULT %s: shutdown() -> the blocked call did NOT return within 5000 ms\n", name);
    } else pthread_join(t, NULL);
    (void)t0;
}
static int plain_shutdown(int fd, int how) { return shutdown(fd, how); }
static int listen_local(int backlog, int *port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&sa, sizeof sa); listen(ls, backlog);
    socklen_t l = sizeof sa; getsockname(ls, (struct sockaddr *)&sa, &l); *port = ntohs(sa.sin_port);
    return ls;
}
static void *accept_silent(void *a) { int ls = (int)(intptr_t)a; for (;;) { int c = accept(ls, NULL, NULL); if (c < 0) break; /* keep it open, never read, never write */ } return NULL; }

struct cfarg { int p; int fd; int done; };
static void *cf(void *x) { struct cfarg *q = x; q->fd = socket_connect("127.0.0.1", (uint16_t)q->p); q->done = 1; return NULL; }
/* S4: a minimal fake usbmuxd that answers Listen and then says nothing */
static int mux_rd(int fd, void *b, size_t n) { size_t g = 0; while (g < n) { ssize_t r = recv(fd, (char *)b + g, n - g, 0); if (r <= 0) return -1; g += (size_t)r; } return 0; }
static volatile int g_nconn; static volatile int g_conn_closed[16];
static void *silent_mux_conn(void *a) {
    int fd = (int)(intptr_t)a; int idx = __atomic_fetch_add(&g_nconn, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        uint32_t h[4]; if (mux_rd(fd, h, 16) || h[0] < 16 || h[0] > 65536) break;
        char *body = malloc(h[0] - 16 + 1); if (mux_rd(fd, body, h[0] - 16)) { free(body); break; } free(body);
        plist_t d = plist_new_dict(); plist_dict_set_item(d, "MessageType", plist_new_string("Result")); plist_dict_set_item(d, "Number", plist_new_uint(0));
        char *xml = NULL; uint32_t len = 0; plist_to_xml(d, &xml, &len); plist_free(d);
        uint32_t rh[4] = {16 + len, h[1], 8, h[3]}; send(fd, rh, 16, MSG_NOSIGNAL); send(fd, xml, len, MSG_NOSIGNAL); free(xml);
        /* after the Listen result: silence (no device events) */
    }
    close(fd); if (idx < 16) __atomic_store_n(&g_conn_closed[idx], 1, __ATOMIC_SEQ_CST); return NULL;   /* the client closed (or shut down) this connection */
}
static void *silent_mux_accept(void *a) { int ls = (int)(intptr_t)a; for (;;) { int c = accept(ls, NULL, NULL); if (c < 0) break; pthread_t t; pthread_create(&t, NULL, silent_mux_conn, (void *)(intptr_t)c); pthread_detach(t); } return NULL; }
static void ev_cb(const usbmuxd_event_t *e, void *u) { (void)e; (void)u; }
struct unsub { usbmuxd_subscription_context_t ctx; int rc; int done; double t_end; };
static void *unsub_thread(void *a) { struct unsub *u = a; u->rc = usbmuxd_events_unsubscribe(u->ctx); u->t_end = now(); u->done = 1; return NULL; }

/* S4j: one injected pthread_join failure, and a look at whether thread_free is then called on a thread that is still alive */
int __real_thread_join(THREAD_T t);
void __real_thread_free(THREAD_T t);
static volatile int g_join_inject = 0, g_join_injected = 0, g_free_calls = 0, g_free_while_alive = 0;
static THREAD_T g_injected_thread;
int __wrap_thread_join(THREAD_T t) {
    if (__atomic_exchange_n(&g_join_inject, 0, __ATOMIC_SEQ_CST)) { g_injected_thread = t; g_join_injected = 1; return EINVAL; }   /* the thread is NOT joined */
    return __real_thread_join(t);
}
void __wrap_thread_free(THREAD_T t) {
    g_free_calls++;
    if (g_join_injected && t == g_injected_thread && thread_alive(t)) g_free_while_alive = 1;
    __real_thread_free(t);
}

/* S4o: the old monitor thread is HELD inside its cleanup (its first socket_close while armed) so that it is certainly alive when unsubscribe returns after the injected join failure */
int __real_socket_close(int fd);
static volatile int g_park_arm, g_parked, g_release;
int __wrap_socket_close(int fd) {
    if (__atomic_exchange_n(&g_park_arm, 0, __ATOMIC_SEQ_CST)) { __atomic_store_n(&g_parked, 1, __ATOMIC_SEQ_CST); while (!__atomic_load_n(&g_release, __ATOMIC_SEQ_CST)) usleep(1000); }
    return __real_socket_close(fd);
}

int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t w; pthread_create(&w, NULL, watchdog, (void *)(intptr_t)120); pthread_detach(w);
    signal(SIGPIPE, SIG_IGN);
    if (!strncmp(c, "pf_", 3)) return pf_case(c);
    int port = 0; int ls = listen_local(!strcmp(c, "s3") || !strcmp(c, "s3lib") ? 0 : 16, &port);
    pthread_t at; struct blk b; memset(&b, 0, sizeof b);
    if (!strcmp(c, "s1") || !strcmp(c, "s5") || !strcmp(c, "s2")) {
        pthread_create(&at, NULL, accept_silent, (void *)(intptr_t)ls); pthread_detach(at);
        int fd = socket_connect("127.0.0.1", (uint16_t)port);
        if (!strcmp(c, "s2")) { int sz = 4096; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz); }
        b.fd = fd; b.op = !strcmp(c, "s1") ? 0 : !strcmp(c, "s5") ? 1 : 2; b.timeout = 30000;
        printf("SD_CASE %s: %s\n", c, !strcmp(c, "s1") ? "untimed recv on a connected, silent socket (library socket_receive)" : !strcmp(c, "s5") ? "timed recv (30000 ms) on a connected, silent socket (library socket_receive_timeout, select/poll based)" : "send() blocked on a full send buffer: the peer accepts and never reads");
        run_blk(c, &b, fd, plain_shutdown);
        close(fd);
    } else if (!strcmp(c, "s1b")) {
        FakeScenario scn; memset(&scn, 0, sizeof scn); scn.data_port_mode = FAKE_DP_OPEN_SILENT;
        int mp = fake_mux_start(&scn); char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%d", mp); setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);
        idevice_t dev = NULL; idevice_connection_t conn = NULL; int fd = -1;
        if (idevice_new_with_options(&dev, FAKE_UDID, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS || idevice_connect(dev, FAKE_DATA_PORT, &conn) != IDEVICE_E_SUCCESS || idevice_connection_get_fd(conn, &fd) != IDEVICE_E_SUCCESS) { printf("SD_SETUP s1b failed\n"); return 2; }
        b.conn = conn; b.op = 4;
        printf("SD_CASE s1b: idevice_connection_receive (the call asr.c uses) on a silent connection; shutdown() of the connection's fd\n");
        run_blk(c, &b, fd, plain_shutdown);
    } else if (!strcmp(c, "s3")) {
        /* listener with backlog 0 that never accepts: fill the queue so that the next connect() hangs in SYN_SENT */
        int fillers[8], nf = 0;
        for (int i = 0; i < 8; i++) { int f = socket(AF_INET, SOCK_STREAM, 0); fcntl(f, F_SETFL, O_NONBLOCK); struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); connect(f, (struct sockaddr *)&sa, sizeof sa); fillers[nf++] = f; }
        usleep(200000);
        int fd = socket(AF_INET, SOCK_STREAM, 0); b.fd = fd; b.op = 3; b.timeout = (unsigned)port;
        printf("SD_CASE s3: raw connect() that hangs (listener queue full); shutdown() of the not-yet-connected fd from another thread\n");
        run_blk(c, &b, fd, plain_shutdown);
        close(fd); for (int i = 0; i < nf; i++) close(fillers[i]);
    } else if (!strcmp(c, "s3lib")) {
        /* the library's socket_connect: does it have a timeout, and could anyone shut the fd down before it returns? */
        int fillers[8], nf = 0;
        for (int i = 0; i < 8; i++) { int f = socket(AF_INET, SOCK_STREAM, 0); fcntl(f, F_SETFL, O_NONBLOCK); struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); connect(f, (struct sockaddr *)&sa, sizeof sa); fillers[nf++] = f; }
        usleep(200000);
        double t0 = now(); int fd = -2;
        struct cfarg a = { port, -2, 0 };
        pthread_t t; pthread_create(&t, NULL, cf, &a);
        for (int i = 0; i < 300 && !a.done; i++) usleep(10000);
        printf("SD_CASE s3lib: library socket_connect against a listener whose queue is full\n");
        if (a.done) printf("SD_RESULT s3lib: socket_connect RETURNED after %.0f ms, fd=%d (so it has its own limit or failed)\n", now() - t0, a.fd);
        else printf("SD_RESULT s3lib: socket_connect still blocked after 3000 ms; its fd is not published to any other thread before it returns, so shutdown() cannot reach it\n");
        (void)fd; for (int i = 0; i < nf; i++) close(fillers[i]); close(ls);
        if (!a.done) _exit(0);
    } else if (!strcmp(c, "s4o")) {
        pthread_create(&at, NULL, silent_mux_accept, (void *)(intptr_t)ls); pthread_detach(at);
        char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%d", port); setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);
        usbmuxd_subscription_context_t c1 = NULL, c2 = NULL;
        int r1 = usbmuxd_events_subscribe(&c1, ev_cb, NULL); usleep(500000);
        printf("SD_CASE s4o: unsubscribe with ONE injected join failure while the old monitor thread is held in its cleanup, then an immediate new subscription\n");
        printf("SD_INFO first subscribe rc=%d, mux connections %d\n", r1, g_nconn);
        g_join_inject = 1; g_park_arm = 1;
        struct unsub u = { c1, -99, 0, 0 };
        pthread_t t; pthread_create(&t, NULL, unsub_thread, &u);
        for (int i = 0; i < 300 && !u.done; i++) usleep(10000);
        if (u.done) pthread_join(t, NULL);
        printf("SD_RESULT s4o: unsubscribe done=%d rc=%d, old monitor held in cleanup=%d, thread_free called while it was alive=%d\n", u.done, u.rc, g_parked, g_free_while_alive);
        int r2 = usbmuxd_events_subscribe(&c2, ev_cb, NULL); usleep(500000);
        printf("SD_RESULT s4o: second subscribe rc=%d, mux connections %d, closed-by-client before release: conn0=%d conn1=%d\n", r2, g_nconn, g_conn_closed[0], g_conn_closed[1]);
        g_release = 1; usleep(800000);
        printf("SD_RESULT s4o: after the old thread finished its cleanup: conn0 closed=%d, conn1 (the NEW monitor's connection) closed=%d, connections %d\n", g_conn_closed[0], g_conn_closed[1], g_nconn);
        /* the old cleanup ends with "listenfd = -1" on the SHARED variable: if that overwrote the new monitor's descriptor, unsubscribing the new subscription can no longer wake the new monitor */
        struct unsub u2 = { c2, -99, 0, 0 }; pthread_t t2; double ts2 = now(); pthread_create(&t2, NULL, unsub_thread, &u2);
        for (int i = 0; i < 400 && !u2.done; i++) usleep(10000);
        if (u2.done) { pthread_join(t2, NULL); printf("SD_RESULT s4o: unsubscribe of the NEW subscription returned after %.0f ms, rc=%d\n", u2.t_end - ts2, u2.rc); }
        else printf("SD_RESULT s4o: unsubscribe of the NEW subscription did NOT return within 4000 ms\n");
        printf("SD_VERDICT s4o: %s\n", !u2.done ? "OVERLAP HARMFUL: the old thread's late cleanup left the new monitor impossible to stop (shared listenfd overwritten)" : (g_conn_closed[1] ? "OVERLAP HARMFUL: the new connection was closed" : "no functional harm observed (not proof; see sanitizer reports)"));
    } else if (!strcmp(c, "s4") || !strcmp(c, "s4j")) {
        int inject = !strcmp(c, "s4j");
        pthread_create(&at, NULL, silent_mux_accept, (void *)(intptr_t)ls); pthread_detach(at);
        char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%d", port); setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);
        usbmuxd_subscription_context_t ctx = NULL;
        int rc = usbmuxd_events_subscribe(&ctx, ev_cb, NULL);
        printf("SD_CASE %s: libusbmuxd device monitor against a usbmuxd that answers Listen and then stays silent%s\n", c, inject ? "; ONE pthread_join failure is injected (the monitor thread is not joined)" : "");
        usleep(500000);
        printf("SD_INFO subscribe rc=%d\n", rc);
        if (inject) g_join_inject = 1;
        struct unsub u = { ctx, -99, 0, 0 };
        pthread_t t; double ts = now(); pthread_create(&t, NULL, unsub_thread, &u);
        for (int i = 0; i < 500 && !u.done; i++) usleep(10000);
        if (u.done) { pthread_join(t, NULL); printf("SD_RESULT %s: usbmuxd_events_unsubscribe RETURNED after %.0f ms, rc=%d\n", c, u.t_end - ts, u.rc); }
        else printf("SD_RESULT %s: usbmuxd_events_unsubscribe did NOT return within 5000 ms (join waits for a monitor thread that does not exit)\n", c);
        if (inject) {
            printf("SD_RESULT %s: join injected=%d, thread_free called while the monitor thread was still alive=%d (free calls %d)\n", c, g_join_injected, g_free_while_alive, g_free_calls);
            if (g_join_injected) { printf("SD_INFO the monitor thread keeps running after unsubscribe returned; cleaning up\n"); }
        }
    } else { printf("unknown case %s\n", c); return 2; }
    fflush(stdout);
    _exit(0);
}
