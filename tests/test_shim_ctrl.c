/* host/libusb_shim.c's transfer machinery (workers, cancel, completion, the synchronous call) over the STRICT control path, against a FAKE ROM thread. No libirecovery, no real USB, no device.
   What it shows: the cancel flag of a libusb transfer reaches the strict wait; a queued transfer that is cancelled sends nothing; each transfer is completed exactly once;
   the user's buffer is not written by a cancelled transfer; a transfer object freed right after its callback (and a synchronous caller's wait object) are not touched by the worker
   afterwards (ASan is the check for that, and says nothing about accesses it cannot see). Verdict tags as in tests/test_usb_ctrl.c: SAFE_BLOCKED is not a recovery.
   "KNOWN_RISK" lines reproduce a hazard that is NOT fixed (libusb_close with a transfer still queued); they never fail the run, only a hang or a harness error does.
   Scenario in argv[1], or "all". One process per scenario in tools/usb-ctrl-test.sh. */
#define _GNU_SOURCE 1
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../host/usb_host.h"

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

/* ---- the fake ROM: answers the shim's enumeration, and the vendor IN request 0xA0 as the scenario says ---- */
static HvVirtioUsb *g_u;
static volatile int g_stop;
static volatile int g_probe_seen;     /* vendor 0xA0 requests the ROM took */
static volatile int g_rom_busy;       /* the ROM is in the middle of a delayed answer */
static int g_delay_ms = -1;           /* answer to 0xA0 after this long; -1: never */
static uint8_t g_ansbyte = 0xA1;
static pthread_t g_th;
static void *rom(void *arg) {
    (void)arg;
    while (!g_stop) {
        HvVusbMsg m;
        if (!hv_vusb_take_request(g_u, &m)) { usleep(300); continue; }
        if (m.type == HV_VUSB_REQ_RESET) { hv_vusb_put_response(g_u, HV_VUSB_RSP_RESET, 0, NULL, 0); free(m.data); continue; }
        const uint8_t *s = m.data;
        uint16_t val = (uint16_t)(s[2] | s[3] << 8);
        if (s[1] == 6 && val == 0x0100) {
            uint8_t d[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x6b, 0x10, 0x01, 0x12, 0, 1, 0, 0, 0, 1};
            hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0, d, 18);
        } else if (s[1] == 6 && (val >> 8) == 2) {
            uint8_t c[9] = {9, 2, 9, 0, 0, 1, 0, 0x80, 50};
            hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0, c, 9);
        } else if (s[1] == 0xA0) {
            __atomic_add_fetch(&g_probe_seen, 1, __ATOMIC_ACQ_REL);
            int dl = g_delay_ms;
            if (dl >= 0) {
                g_rom_busy = 1;
                int t0 = msnow();
                while (!g_stop && msnow() - t0 < dl) usleep(500);
                hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0, &g_ansbyte, 1);
                g_rom_busy = 0;
            }
        }
        free(m.data);
    }
    return NULL;
}

static libusb_device_handle *g_h;
static libusb_device **g_list;
static void setup(int strict, int delay_ms) {
    g_delay_ms = delay_ms; g_stop = 0; g_probe_seen = 0; g_rom_busy = 0;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    hv_vusb_cv_init(g_u);
    hv_vusb_set_online(g_u, 1);
    if (hv_usb_set_strict(g_u, strict) != 0) { printf("FAIL set_strict at set-up\n"); bad++; }
    pthread_create(&g_th, NULL, rom, NULL);
    hv_libusb_shim_attach(g_u);
    libusb_device **l = NULL;
    ssize_t n = libusb_get_device_list(NULL, &l);
    if (n < 1 || libusb_open(l[0], &g_h) != 0) { printf("FAIL harness: no device through the shim (n=%zd)\n", n); exit(2); }
    g_list = l;
}
static void teardown(void) {
    libusb_close(g_h);
    libusb_free_device_list(g_list, 1);
    g_stop = 1; pthread_join(g_th, NULL);
}

/* ---- transfers with a callback that counts ---- */
typedef struct { volatile int calls; volatile int status; volatile int actual; uint8_t buf[8 + 4]; struct libusb_transfer *t; } Job;
static void cb(struct libusb_transfer *t) { Job *j = t->user_data; j->status = t->status; j->actual = t->actual_length; __atomic_add_fetch(&j->calls, 1, __ATOMIC_ACQ_REL); }
static Job *job_new(unsigned timeout) {
    Job *j = calloc(1, sizeof *j);
    j->t = libusb_alloc_transfer(0);
    memset(j->buf + 8, 0xEE, 4);
    libusb_fill_control_setup(j->buf, 0xC0, 0xA0, 0, 0, 1);
    libusb_fill_control_transfer(j->t, g_h, j->buf, cb, j, timeout);
    return j;
}
static void pump(Job *j, int want_calls, int ms) {
    int t0 = msnow();
    while (j->calls < want_calls && msnow() - t0 < ms) { struct timeval tv = {0, 20000}; libusb_handle_events_timeout(NULL, &tv); }
}
static void pump_for(int ms) { int t0 = msnow(); while (msnow() - t0 < ms) { struct timeval tv = {0, 20000}; libusb_handle_events_timeout(NULL, &tv); } }
static void job_free(Job *j) { libusb_free_transfer(j->t); free(j); }

static void s_sync(void) {
    setup(1, 20);
    uint8_t b = 0;
    int r = libusb_control_transfer(g_h, 0xC0, 0xA0, 0, 0, &b, 1, 1000);
    EXPECT(r == 1 && b == 0xA1 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "sync: strict control transfer returns its answer (%d, %02x), CLEAR", r, b);
    teardown();
}

static void s_cancel_run(void) {
    setup(1, -1);
    Job *j = job_new(10000);
    libusb_submit_transfer(j->t);
    usleep(150000);
    int t0 = msnow();
    libusb_cancel_transfer(j->t);
    pump(j, 1, 3000);
    int took = msnow() - t0;
    EXPECT(j->calls == 1 && j->status == LIBUSB_TRANSFER_CANCELLED && took < 700, "cancel(running): libusb_cancel_transfer reached the strict wait: callback once, CANCELLED, %d ms (the transfer timeout was 10000)", took);
    EXPECT(hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN && g_probe_seen == 1, "SAFE_BLOCKED cancel(running): the sent request is unsettled: UNCERTAIN (not a recovery); the ROM saw 1 request");
    EXPECT(j->buf[8] == 0xEE, "cancel(running): the user's buffer was not written (%02x)", j->buf[8]);
    pump_for(300);
    EXPECT(j->calls == 1, "cancel(running): still exactly one completion notice after 300 ms more of event handling (%d)", j->calls);
    job_free(j);
    teardown();
}

static void s_cancel_queued(void) {
    setup(1, -1);
    Job *a = job_new(10000), *b = job_new(10000);
    libusb_submit_transfer(a->t);
    usleep(100000);
    libusb_submit_transfer(b->t);   /* queued behind A on the control worker */
    usleep(50000);
    libusb_cancel_transfer(b->t);
    libusb_cancel_transfer(a->t);
    pump(a, 1, 3000); pump(b, 1, 3000);
    EXPECT(a->calls == 1 && a->status == LIBUSB_TRANSFER_CANCELLED, "cancel(queued): A (running) CANCELLED once");
    EXPECT(b->calls == 1 && b->status == LIBUSB_TRANSFER_CANCELLED, "cancel(queued): B (waiting in the queue) CANCELLED once");
    EXPECT(g_probe_seen == 1, "cancel(queued): B never reached the guest: the ROM saw %d request(s), not 2", g_probe_seen);
    pump_for(200);
    EXPECT(a->calls == 1 && b->calls == 1, "cancel(queued): no second notice later");
    job_free(a); job_free(b);
    teardown();
}

static void s_race(void) {
    setup(1, 100);
    int done = 0, canc = 0, incons = 0, twice = 0;
    for (int i = 0; i < 30; i++) {
        Job *j = job_new(5000);
        libusb_submit_transfer(j->t);
        usleep((useconds_t)(40 + i * 4) * 1000);   /* the answer comes at 100 ms; a cancel is noticed at the next 50 ms slice end: they straddle each other */
        libusb_cancel_transfer(j->t);
        pump(j, 1, 3000);
        usleep(30000);
        int st = hv_usb_ctrl_state(g_u);
        if (j->calls != 1) twice++;
        else if (j->status == LIBUSB_TRANSFER_COMPLETED && j->actual == 1 && j->buf[8] == 0xA1 && st == HV_CTRL_CLEAR) done++;
        else if (j->status == LIBUSB_TRANSFER_CANCELLED && j->buf[8] == 0xEE && st == HV_CTRL_UNCERTAIN) canc++;
        else incons++;
        job_free(j);
        while (g_rom_busy) usleep(1000);
        usleep(5000);
        hv_vusb_flush(g_u);                        /* drop an answer the cancelled request left, as a CALLER vouching for isolation must */
        if (st == HV_CTRL_UNCERTAIN) hv_usb_ctrl_reacquire(g_u);
    }
    EXPECT(twice == 0 && incons == 0 && done + canc == 30, "race: 30 cancels around the answer: %d completed (data written, CLEAR), %d cancelled (buffer untouched, UNCERTAIN), %d inconsistent, %d with a notice count other than 1", done, canc, incons, twice);
    teardown();
}

static void s_sync_timeout(void) {
    setup(1, -1);
    uint8_t b = 0;
    int t0 = msnow(), r = libusb_control_transfer(g_h, 0xC0, 0xA0, 0, 0, &b, 1, 150), took = msnow() - t0;
    EXPECT(r == LIBUSB_ERROR_TIMEOUT && took >= 100 && took < 900, "sync: no answer: the caller returns after %d ms with TIMEOUT (%d), after the worker is done with its wait object", took, r);
    int r2 = libusb_control_transfer(g_h, 0xC0, 0xA0, 0, 0, &b, 1, 500);
    EXPECT(r2 == LIBUSB_ERROR_IO && g_probe_seen == 1 && hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN, "SAFE_BLOCKED sync: the next transfer fails at once (IO) and is not sent: the ROM saw %d request", g_probe_seen);
    teardown();
}

static void s_legacy(void) {
    setup(0, 300);
    uint8_t b = 0;
    int r = libusb_control_transfer(g_h, 0xC0, 0xA0, 0, 0, &b, 1, 100);
    int r2 = libusb_control_transfer(g_h, 0xC0, 0xA0, 0, 0, &b, 1, 1500);
    EXPECT(r == LIBUSB_ERROR_TIMEOUT && g_probe_seen == 2, "legacy (strict off, the default): the shim behaves as before: the second request IS sent after a timeout (%d, ROM saw %d)", r, g_probe_seen);
    EXPECT(r2 == 1 && g_u->cstrict == 0 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "legacy: no state is tracked (%d)", r2);
    teardown();
}

static void s_lifetime(void) {
    setup(1, 0);
    int bad_sync = 0, bad_async = 0;
    uint8_t b;
    for (int i = 0; i < 150; i++) { b = 0; if (libusb_control_transfer(g_h, 0xC0, 0xA0, 0, 0, &b, 1, 2000) != 1 || b != 0xA1) bad_sync++; }
    for (int i = 0; i < 60; i++) {
        Job *j = job_new(2000);
        libusb_submit_transfer(j->t);
        pump(j, 1, 3000);
        if (j->calls != 1 || j->status != LIBUSB_TRANSFER_COMPLETED) bad_async++;
        job_free(j);   /* freed straight after the callback: the worker must not touch it again (ASan) */
    }
    EXPECT(bad_sync == 0 && bad_async == 0, "lifetime: 150 synchronous and 60 asynchronous transfers, objects freed right after completion: %d + %d wrong; no use-after-free reported by the sanitizer in this run", bad_sync, bad_async);
    EXPECT(g_u->cbusy == 0, "lifetime: nothing left counted inside the control path (%d)", g_u->cbusy);
    teardown();
}

static void s_close(void) {   /* KNOWN_RISK: the shim does not wait for transfers before libusb_close */
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        alarm(20);
        setup(1, -1);
        Job *a = job_new(10000), *b = job_new(10000);
        libusb_submit_transfer(a->t);
        usleep(100000);
        libusb_submit_transfer(b->t);        /* B waits behind A */
        usleep(50000);
        libusb_close(g_h);                   /* the handle goes while B still points at it */
        libusb_cancel_transfer(a->t);        /* A ends, the worker takes B and reads B's handle */
        pump(a, 1, 2000);
        usleep(300000);
        _exit(0);
    }
    int st = 0, waited = 0;
    while (waitpid(p, &st, WNOHANG) == 0 && waited < 30000) { usleep(10000); waited += 10; }
    if (waited >= 30000) { kill(p, SIGKILL); waitpid(p, &st, 0); EXPECT(0, "close: HUNG: the child did not end in 30 s"); return; }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) printf("KNOWN_RISK close: libusb_close with a transfer still queued: the child ended normally (a plain build does not show the access; not a safety result)\n");
    else printf("KNOWN_RISK close: libusb_close with a transfer still queued: the child ended abnormally (status %d): the hazard is REPRODUCED and NOT fixed; the contract is that the caller closes a handle only with none of its transfers outstanding\n", st);
    EXPECT(1, "close: the risk run ended within its limit (a reproduction, not a pass of the hazard)");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(120);
    const char *sc = argc > 1 ? argv[1] : "sync";
    if (!strcmp(sc, "sync")) s_sync();
    else if (!strcmp(sc, "cancel_run")) s_cancel_run();
    else if (!strcmp(sc, "cancel_queued")) s_cancel_queued();
    else if (!strcmp(sc, "race")) s_race();
    else if (!strcmp(sc, "sync_timeout")) s_sync_timeout();
    else if (!strcmp(sc, "legacy")) s_legacy();
    else if (!strcmp(sc, "lifetime")) s_lifetime();
    else if (!strcmp(sc, "close")) s_close();
    else { printf("FAIL unknown scenario %s\n", sc); return 2; }
    printf("%s test_shim_ctrl %s: %d failed\n", bad ? "FAIL" : "PASS", sc, bad);
    return bad ? 1 : 0;
}
