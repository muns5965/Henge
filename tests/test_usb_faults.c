/* Failure injection for the virtual USB data path: devices/virtio_usb.c (request and response rings) and host/libusb_shim.c (bulk transfers), against a FAKE guest thread.
   No libirecovery, no device. One process per scenario (tools/usb-faults-test.sh), argv[1] = scenario name.

   What each scenario pins down (every one of these was wrong, or could not be told apart, before):
     in_full   an IN request that cannot be queued because the REQUEST ring is full is not "the device is gone": the transfer ends with an error (not NO_DEVICE, which makes
               usbmuxd drop the device), and the shim does not go on believing the guest holds a request: the next IN transfer asks again and gets its data.
     out_room  an OUT transfer whose pieces meet a full request ring waits for room (the guest will take its buffers), and no piece is sent twice.
     lost      a response the guest sent while the RESPONSE ring was full is not silently gone: it is counted, and the transfer that waits for it ends with an error instead of
               waiting for ever (usbmuxd's bulk reads have no timeout of their own); the next IN transfer works.
     deadline  an OUT transfer with several pieces honours its timeout between pieces (the time limit used to be counted in idle 200 ms slices only).
     late_ack, out_partial, cancel_next   the answers an OUT transfer that ended early leaves behind are not taken for the next transfer's: it waits for them, then sends.
     queue_wait0  a transfer with no time limit waits for room for a bounded time, then fails with an error; afterwards the endpoint works.
     session   a request waiting for room while the connection ends fails with NO_DEVICE; on the new connection IN works.
     workers   a worker thread that could not start is reported when a transfer is submitted to it; nothing is queued where nothing would ever run it.
   The link carries no transfer id, so the shim owes the endpoint the answers of pieces already queued by a transfer that ended early, and the next OUT transfer takes those first. */
#define _GNU_SOURCE 1
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../host/usb_host.h"

#define RD(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
static int bad;
#define EXPECT(cond, ...) do { int ok_ = !!(cond); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); fflush(stdout); if (!ok_) bad++; } while (0)

int hv_virtq_pop(HvVirtio *v, int q, HvVirtqElem *e) { (void)v, (void)q, (void)e; return 0; }
void hv_virtq_push(HvVirtio *v, int q, const HvVirtqElem *e, uint32_t w) { (void)v, (void)q, (void)e, (void)w; }
void hv_virtio_interrupt(HvVirtio *v) { (void)v; }
void hv_virtio_init(HvVirtio *v, HvMachine *m, int id, uint32_t cc, int nq, uint64_t f) { (void)v, (void)m, (void)id, (void)cc, (void)nq, (void)f; }
uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t o, void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t o, const void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_writable(const HvVirtqElem *e) { (void)e; return 0; }
void hv_machine_wake(HvMachine *m) { (void)m; }
int hv_pcie_attach(HvPcie *b, HvPciDev *p) { (void)b, (void)p; return 0; }

static int msnow(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int)(ts.tv_sec % 100000 * 1000 + ts.tv_nsec / 1000000); }

/* ---- the fake guest ---- */
static HvVirtioUsb *g_u;
static int g_stop, g_paused;            /* paused: the guest takes no request (its buffers are not posted) */
static int g_drop_acks;                 /* OUT pieces on ep 0x02 are taken and never answered */
static int g_ack_delay_ms;              /* each OUT piece is answered after this long */
static int g_in_len = 64;               /* an IN request is answered with this many bytes (0: not answered at all) */
__attribute__((weak)) void hv_libusb_set_queue_wait_ms(int ms) { (void)ms; } /* old sources have no hook */
static int g_out_bytes, g_out_pieces, g_acks_sent;   /* what the guest was given on the OUT endpoint 0x02 */
static int g_fail_threads;              /* the wrapped pthread_create fails (except for the fake guest itself) */
static pthread_t g_th;

static void *guest(void *arg) {
    (void)arg;
    while (!RD(g_stop)) {
        if (RD(g_paused)) { usleep(300); continue; }
        HvVusbMsg m;
        if (!hv_vusb_take_request(g_u, &m)) { usleep(300); continue; }
        if (m.type == HV_VUSB_REQ_RESET) { hv_vusb_put_response(g_u, HV_VUSB_RSP_RESET, 0, NULL, 0); free(m.data); continue; }
        if (m.ep == 0 && m.data && m.len >= 8) {
            const uint8_t *s = m.data;
            uint16_t val = (uint16_t)(s[2] | s[3] << 8);
            if (s[1] == 6 && val == 0x0100) {
                uint8_t d[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x6b, 0x10, 0x01, 0x12, 0, 1, 0, 0, 0, 1};
                hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0, d, 18);
            } else if (s[1] == 6 && (val >> 8) == 2) {
                uint8_t c[9] = {9, 2, 9, 0, 0, 1, 0, 0x80, 50};
                hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0, c, 9);
            }
        } else if (m.ep & 0x80) {   /* an IN request: answer with data */
            if (g_in_len > 0) {
                uint8_t *p = malloc((size_t)g_in_len);
                memset(p, 0x5A, (size_t)g_in_len);
                hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, m.ep, p, (uint32_t)g_in_len);
                free(p);
            }
        } else if (m.ep == 0x05) {   /* filler: taken, not answered (its answers would fill the response ring) */
        } else {   /* an OUT piece: count it, answer with the empty "done" */
            if (m.ep == 0x02) { __atomic_add_fetch(&g_out_bytes, (int)m.len, __ATOMIC_SEQ_CST); __atomic_add_fetch(&g_out_pieces, 1, __ATOMIC_SEQ_CST); }
            if (m.ep == 0x02 && RD(g_drop_acks)) { free(m.data); continue; }
            if (g_ack_delay_ms > 0) usleep((useconds_t)g_ack_delay_ms * 1000);
            if (m.ep == 0x02) __atomic_add_fetch(&g_acks_sent, 1, __ATOMIC_SEQ_CST);   /* counted BEFORE the answer is put: the host may complete the moment it is there */
            hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, m.ep, NULL, 0);
        }
        free(m.data);
    }
    return NULL;
}

/* The wrapper lets a scenario make worker threads fail to start (linked with -Wl,--wrap=pthread_create). */
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    if (RD(g_fail_threads) && fn != guest) return 11;   /* EAGAIN */
    return __real_pthread_create(th, a, fn, arg);
}

static libusb_device_handle *g_h;
static libusb_device **g_list;
static int setup(void) {
    ST(g_stop, 0); ST(g_paused, 0); ST(g_drop_acks, 0); g_ack_delay_ms = 0; g_in_len = 64; g_out_bytes = g_out_pieces = g_acks_sent = 0;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    hv_vusb_cv_init(g_u);
    hv_vusb_set_online(g_u, 1);
    pthread_create(&g_th, NULL, guest, NULL);
    hv_libusb_shim_attach(g_u);
    libusb_device **l = NULL;
    ssize_t n = libusb_get_device_list(NULL, &l);
    if (n < 1 || libusb_open(l[0], &g_h) != 0) return -1;
    g_list = l;
    return 0;
}
static void teardown(void) {
    libusb_close(g_h);
    libusb_free_device_list(g_list, 1);
    ST(g_stop, 1); pthread_join(g_th, NULL);
}

typedef struct { volatile int calls, status, actual, took_ms, acks_at_done; int t0; uint8_t buf[70000]; struct libusb_transfer *t; } Job;
static void cb(struct libusb_transfer *t) { Job *j = t->user_data; j->status = t->status; j->actual = t->actual_length; j->took_ms = msnow() - j->t0; j->acks_at_done = RD(g_acks_sent); __atomic_add_fetch(&j->calls, 1, __ATOMIC_ACQ_REL); }
static Job *bulk(uint8_t ep, int len, unsigned timeout) {
    Job *j = calloc(1, sizeof *j);
    j->t = libusb_alloc_transfer(0);
    if (!(ep & 0x80)) memset(j->buf, 0xA5, (size_t)len);
    libusb_fill_bulk_transfer(j->t, g_h, ep, j->buf, len, cb, j, timeout);
    j->t0 = msnow();
    return j;
}
static void pump(Job *j, int ms) { int t0 = msnow(); while (j->calls < 1 && msnow() - t0 < ms) { struct timeval tv = {0, 20000}; libusb_handle_events_timeout(NULL, &tv); } }
static void fill_requests(void) {   /* the request ring: full (filler on an endpoint nothing else uses) */
    int n = 0;
    while (hv_vusb_send(g_u, HV_VUSB_REQ_DATA, 0x05, "x", 1) == 0) n++;
    printf("note: request ring filled with %d filler requests\n", n);
}

static void s_in_full(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    ST(g_paused, 1);
    fill_requests();
    Job *a = bulk(0x81, 64, 500);
    libusb_submit_transfer(a->t); pump(a, 3000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_ERROR, "in_full: a full request ring ends the IN transfer with ERROR, not NO_DEVICE (status %d, calls %d, after %d ms)", a->status, a->calls, a->took_ms);
    ST(g_paused, 0);
    usleep(300000);   /* the guest takes the fillers and answers them; the shim has nothing to do with those */
    hv_vusb_flush(g_u);
    Job *b = bulk(0x81, 64, 1500);
    libusb_submit_transfer(b->t); pump(b, 4000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_COMPLETED && b->actual == 64, "in_full: the next IN transfer asks the guest again and gets its 64 bytes (status %d, actual %d, %d ms)", b->status, b->actual, b->took_ms);
    libusb_free_transfer(a->t); libusb_free_transfer(b->t);
    teardown();
}

static void s_out_room(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    ST(g_paused, 1);
    fill_requests();
    Job *a = bulk(0x02, 40000, 4000);
    libusb_submit_transfer(a->t);
    usleep(250000);
    ST(g_paused, 0);   /* the guest posts its buffers: room appears */
    pump(a, 6000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_COMPLETED, "out_room: the OUT transfer waited for room and completed (status %d, calls %d, %d ms)", a->status, a->calls, a->took_ms);
    EXPECT(RD(g_out_bytes) == 40000 && RD(g_out_pieces) == 3, "out_room: the guest got the 40000 bytes once, in 3 pieces (bytes %d, pieces %d)", RD(g_out_bytes), RD(g_out_pieces));
    libusb_free_transfer(a->t);
    teardown();
}

static void s_lost(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
#ifdef HAS_LOST
    for (int i = 0; i < 256; i++) hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0x03, "y", 1);   /* the response ring: full, nobody takes them */
    EXPECT(hv_vusb_lost_total(g_u) == 0, "lost: nothing lost while there is room");
#else
    for (int i = 0; i < 256; i++) hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0x03, "y", 1);
#endif
    Job *a = bulk(0x81, 64, 0);   /* no timeout of its own, like usbmuxd's reads */
    libusb_submit_transfer(a->t); pump(a, 3000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_ERROR, "lost: the answer was dropped (ring full): the IN transfer ends with ERROR instead of waiting for ever (status %d, calls %d, %d ms)", a->status, a->calls, a->took_ms);
#ifdef HAS_LOST
    EXPECT(hv_vusb_lost_total(g_u) >= 1, "lost: the drop was counted (%u)", hv_vusb_lost_total(g_u));
#endif
    hv_vusb_flush(g_u);
    Job *b = bulk(0x81, 64, 1500);
    libusb_submit_transfer(b->t); pump(b, 4000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_COMPLETED && b->actual == 64, "lost: afterwards the next IN transfer works (status %d, actual %d)", b->status, b->actual);
    if (a->calls == 0) libusb_cancel_transfer(a->t);
    pump(a, 2000);
    libusb_free_transfer(a->t); libusb_free_transfer(b->t);
    teardown();
}

static void s_deadline(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    g_ack_delay_ms = 150;   /* 4 pieces, 150 ms each: about 600 ms in all */
    Job *a = bulk(0x02, 65536, 400);
    libusb_submit_transfer(a->t); pump(a, 4000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_TIMED_OUT && a->took_ms >= 380 && a->took_ms < 900, "deadline: a 400 ms limit holds between pieces: TIMED_OUT after %d ms (status %d)", a->took_ms, a->status);
    libusb_free_transfer(a->t);
    ST(g_stop, 1); pthread_join(g_th, NULL);   /* the stale answers of the ended transfer are the documented gap: no further check */
    libusb_close(g_h); libusb_free_device_list(g_list, 1);
}


/* The answers of an OUT transfer that ended early are still to come. The next OUT transfer must not complete on one of them. */
static void s_late_ack(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    g_ack_delay_ms = 150;   /* 4 pieces: answers at about 150, 300, 450, 600 ms */
    Job *a = bulk(0x02, 65536, 400);
    libusb_submit_transfer(a->t); pump(a, 4000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_TIMED_OUT, "late_ack: the first transfer ends by its time limit (status %d)", a->status);
    Job *b = bulk(0x02, 16384, 5000);
    libusb_submit_transfer(b->t); pump(b, 8000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_COMPLETED, "late_ack: the second transfer completes (status %d, %d ms)", b->status, b->took_ms);
    EXPECT(b->acks_at_done == 5, "late_ack: it completed on ITS OWN answer: the guest had answered all 5 pieces (4 + 1) when it did, not %d (a stranger's answer completed it)", b->acks_at_done);
    libusb_free_transfer(a->t); libusb_free_transfer(b->t);
    teardown();
}

static void s_out_partial(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    ST(g_paused, 1);
    fill_requests();
    HvVusbMsg m;
    if (hv_vusb_take_request(g_u, &m)) free(m.data);   /* one slot free: the first piece fits, the second does not */
    g_ack_delay_ms = 100;
    Job *a = bulk(0x02, 32768, 300);
    libusb_submit_transfer(a->t); pump(a, 3000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_ERROR, "out_partial: one piece queued, the next found no room: the transfer ends with ERROR (status %d, %d ms)", a->status, a->took_ms);
    ST(g_paused, 0);
    Job *b = bulk(0x02, 16384, 5000);
    libusb_submit_transfer(b->t); pump(b, 8000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_COMPLETED && b->acks_at_done == 2, "out_partial: the next transfer completes on its own answer (status %d, the guest had answered %d of 2 pieces)", b->status, b->acks_at_done);
    EXPECT(RD(g_out_bytes) == 32768 && RD(g_out_pieces) == 2, "out_partial: no piece was sent twice (bytes %d, pieces %d)", RD(g_out_bytes), RD(g_out_pieces));
    libusb_free_transfer(a->t); libusb_free_transfer(b->t);
    teardown();
}

static void s_cancel_next(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    g_ack_delay_ms = 150;
    Job *a = bulk(0x02, 65536, 0);
    libusb_submit_transfer(a->t);
    usleep(250000);
    libusb_cancel_transfer(a->t); pump(a, 3000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_CANCELLED, "cancel_next: the first transfer is cancelled (status %d)", a->status);
    Job *b = bulk(0x02, 16384, 8000);
    libusb_submit_transfer(b->t); pump(b, 10000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_COMPLETED && b->acks_at_done == 5, "cancel_next: the next transfer completes on its own answer (status %d, answered %d of 5)", b->status, b->acks_at_done);
    libusb_free_transfer(a->t); libusb_free_transfer(b->t);
    teardown();
}

/* A transfer with no time limit (usbmuxd's) waits for room, but not for ever. */
static void s_queue_wait0(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    hv_libusb_set_queue_wait_ms(400);
    ST(g_paused, 1);
    fill_requests();
    Job *a = bulk(0x02, 16384, 0);
    libusb_submit_transfer(a->t); pump(a, 4000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_ERROR && a->took_ms >= 350 && a->took_ms < 1500, "queue_wait0: no time limit, no room: ERROR after the queue wait (status %d, %d ms)", a->status, a->took_ms);
    ST(g_paused, 0);
    usleep(300000);
    Job *b = bulk(0x02, 16384, 4000);
    libusb_submit_transfer(b->t); pump(b, 6000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_COMPLETED && b->acks_at_done == 1, "queue_wait0: once there is room the next OUT transfer works (status %d, answered %d)", b->status, b->acks_at_done);
    libusb_free_transfer(a->t); libusb_free_transfer(b->t);
    teardown();
}

/* An IN request that waits for room while the connection ends and comes back. */
static void s_session(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    hv_libusb_set_queue_wait_ms(5000);
    ST(g_paused, 1);
    fill_requests();
    Job *a = bulk(0x81, 64, 0);
    libusb_submit_transfer(a->t);
    usleep(200000);
    hv_vusb_set_online(g_u, 0);   /* the guest leaves the bus: its rings are dropped */
    pump(a, 3000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_NO_DEVICE, "session: the connection ended while the request waited for room: NO_DEVICE (status %d)", a->status);
    libusb_close(g_h); libusb_free_device_list(g_list, 1);
    hv_vusb_set_online(g_u, 1);
    ST(g_paused, 0);
    libusb_device **l = NULL;
    ssize_t n = libusb_get_device_list(NULL, &l);
    if (n < 1 || libusb_open(l[0], &g_h) != 0) { printf("FAIL session: the device did not come back (n=%zd)\n", n); exit(2); }
    g_list = l;
    Job *b = bulk(0x81, 64, 2000);
    libusb_submit_transfer(b->t); pump(b, 4000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_COMPLETED && b->actual == 64, "session: on the new connection the next IN transfer asks the guest and gets its data (status %d, actual %d)", b->status, b->actual);
    libusb_free_transfer(a->t); libusb_free_transfer(b->t);
    teardown();
}

/* Answers that never come: the endpoint fails (fail-stop) instead of taking a later answer as its own, stays unusable, and works again on a new connection. */
static void s_debt_lost(void) {
    if (setup()) { printf("FAIL harness\n"); exit(2); }
    ST(g_drop_acks, 1);
    Job *a = bulk(0x02, 32768, 300);
    libusb_submit_transfer(a->t); pump(a, 3000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_TIMED_OUT, "debt_lost: the pieces were taken but never answered: TIMED_OUT (status %d)", a->status);
    int pieces_before = RD(g_out_pieces);
    ST(g_drop_acks, 0);   /* the guest is fine again, but the two answers of transfer 1 are gone for good */
    Job *b = bulk(0x02, 16384, 500);
    libusb_submit_transfer(b->t); pump(b, 4000);
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_ERROR, "debt_lost: the next transfer fails (fail-stop), it does not complete on a stranger's answer (status %d, %d ms)", b->status, b->took_ms);
    EXPECT(RD(g_out_pieces) == pieces_before, "debt_lost: it sent nothing while the debt stood (pieces %d -> %d)", pieces_before, RD(g_out_pieces));
    Job *c = bulk(0x02, 16384, 500);
    libusb_submit_transfer(c->t); pump(c, 4000);
    EXPECT(c->calls == 1 && c->status == LIBUSB_TRANSFER_ERROR, "debt_lost: and the endpoint stays unusable until the connection ends (status %d)", c->status);
    hv_vusb_set_online(g_u, 0);
    libusb_close(g_h); libusb_free_device_list(g_list, 1);
    hv_vusb_set_online(g_u, 1);
    libusb_device **l = NULL;
    ssize_t n = libusb_get_device_list(NULL, &l);
    if (n < 1 || libusb_open(l[0], &g_h) != 0) { printf("FAIL debt_lost: the device did not come back (n=%zd)\n", n); exit(2); }
    g_list = l;
    Job *d = bulk(0x02, 16384, 4000);
    libusb_submit_transfer(d->t); pump(d, 6000);
    EXPECT(d->calls == 1 && d->status == LIBUSB_TRANSFER_COMPLETED, "debt_lost: on the new connection the endpoint works again (status %d)", d->status);
    libusb_free_transfer(a->t); libusb_free_transfer(b->t); libusb_free_transfer(c->t); libusb_free_transfer(d->t);
    teardown();
}

/* Nothing is dropped: nothing is logged about a full ring (a wrong condition once printed a line for EVERY response). */
static void s_no_spam(void) {
    char path[] = "/tmp/uf_stderr_XXXXXX"; int fd = mkstemp(path);
    if (fd < 0) { printf("FAIL harness\n"); exit(2); }
    fflush(stderr); int save = dup(2); dup2(fd, 2);
    if (setup()) { dup2(save, 2); printf("FAIL harness\n"); exit(2); }
    for (int i = 0; i < 20; i++) { Job *a = bulk(0x81, 64, 2000); libusb_submit_transfer(a->t); pump(a, 3000); Job *b = bulk(0x02, 16384, 2000); libusb_submit_transfer(b->t); pump(b, 3000); }
    teardown();
    fflush(stderr); dup2(save, 2); close(save);
    char buf[65536]; lseek(fd, 0, SEEK_SET); ssize_t n = read(fd, buf, sizeof buf - 1); if (n < 0) n = 0; buf[n] = 0; close(fd); unlink(path);
    EXPECT(strstr(buf, "response ring full") == NULL, "no_spam: 40 normal transfers log nothing about a full response ring (%zd bytes of stderr)", n);
}

/* Real losses are logged on the 1st and on every 1024th drop only (not on each one). */
static void s_loss_log(void) {
    char path[] = "/tmp/uf_lossl_XXXXXX"; int fd = mkstemp(path);
    if (fd < 0) { printf("FAIL harness\n"); exit(2); }
    fflush(stderr); int save = dup(2); dup2(fd, 2);
    if (setup()) { dup2(save, 2); printf("FAIL harness\n"); exit(2); }
    for (int i = 0; i < 256 + 1100; i++) hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0x81, NULL, 0);   // nobody takes them: 256 fit, 1100 are dropped
    uint32_t total = hv_vusb_lost_total(g_u);
    teardown();
    fflush(stderr); dup2(save, 2); close(save);
    char buf[65536]; lseek(fd, 0, SEEK_SET); ssize_t n = read(fd, buf, sizeof buf - 1); if (n < 0) n = 0; buf[n] = 0; close(fd); unlink(path);
    int lines = 0; for (char *q = buf; (q = strstr(q, "response ring full")) != NULL; q++) lines++;
    EXPECT(total == 1100, "loss_log: 1100 responses were dropped and counted (%u)", total);
    EXPECT(lines == 2 && strstr(buf, "(1 so far)") && strstr(buf, "(1024 so far)"), "loss_log: only the 1st and the 1024th drop are logged (%d lines)", lines);
}

static void s_workers(void) {
    ST(g_fail_threads, 1);
    ST(g_stop, 0); ST(g_paused, 0);
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    hv_vusb_cv_init(g_u);
    hv_vusb_set_online(g_u, 1);
    pthread_create(&g_th, NULL, guest, NULL);
    hv_libusb_shim_attach(g_u);
    struct libusb_transfer *t = libusb_alloc_transfer(0);
    libusb_device **l = NULL;
    ssize_t n = libusb_get_device_list(NULL, &l);
    libusb_device_handle *hd = NULL;
    int ro = n >= 1 ? libusb_open(l[0], &hd) : -99;
    uint8_t buf[16];
    if (ro == 0) {
        libusb_fill_bulk_transfer(t, hd, 0x02, buf, 16, cb, NULL, 100);
        int r = libusb_submit_transfer(t);
        EXPECT(r == LIBUSB_ERROR_OTHER, "workers: no worker thread could start: a transfer is refused at submit (%d), not queued for ever", r);
    } else {
        EXPECT(1, "workers: no device could be listed without its worker threads (list %zd, open %d): nothing can be queued either", n, ro);
    }
    ST(g_stop, 1); pthread_join(g_th, NULL);
}

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "all";
    int all = !strcmp(sc, "all");
    if (all || !strcmp(sc, "in_full")) s_in_full();
    if (all || !strcmp(sc, "out_room")) s_out_room();
    if (all || !strcmp(sc, "lost")) s_lost();
    if (all || !strcmp(sc, "deadline")) s_deadline();
    if (!strcmp(sc, "late_ack")) s_late_ack();
    if (!strcmp(sc, "out_partial")) s_out_partial();
    if (!strcmp(sc, "cancel_next")) s_cancel_next();
    if (!strcmp(sc, "queue_wait0")) s_queue_wait0();
    if (!strcmp(sc, "session")) s_session();
    if (!strcmp(sc, "debt_lost")) s_debt_lost();
    if (!strcmp(sc, "no_spam")) s_no_spam();
    if (!strcmp(sc, "loss_log")) s_loss_log();
    if (!strcmp(sc, "workers")) s_workers();
    printf("%s: %d failure(s)\n", sc, bad);
    return bad ? 1 : 0;
}
