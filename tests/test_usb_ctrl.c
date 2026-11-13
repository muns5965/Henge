/* The strict control path (host/usb_host.c + devices/virtio_usb.c) against a FAKE guest: a thread that takes the virtio-usb requests and answers them as a script says
   (late, never, wrongly, as a RESET). No real USB, no libusb, no device. What each scenario shows is in its lines; a verdict tag says which kind of result it is:
     SAFE_BLOCKED   an unsettled request was not allowed to be followed by another, and nothing was misattributed. It is NOT a recovery: the endpoint stays unusable.
     SAFE_RECOVERED the endpoint was usable again by a path the design allows (a settled answer, an explicit hv_usb_ctrl_reacquire).
     MISATTRIBUTED  an answer reached a request it does not belong to. Any such line is a failure; the legacy-mode contrast prints it as the hazard being closed.
   A scenario that does not end (HUNG) is caught by the script's timeout. Shows nothing about the real ROM or kernel, and nothing about answers crossing a session
   in the real transport: the fake guest here is told what to do. Scenario in argv[1], or "all". */
#define _GNU_SOURCE 1
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
#define EXPECT(cond, ...) do { int ok_ = !!(cond); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

/* ---- the transport stubs virtio_usb.c needs (the same ones tests/test_irecv.c uses) ---- */
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
typedef struct { int delay_ms; int type; uint8_t byte; int bounce; } Act;   /* type 0: no answer; bounce: leave and come back before answering */
static HvVirtioUsb *g_u;
static int g_realtime_cv;   /* a creator that does its own pthread_cond_init(NULL): cmono stays 0 */
static Act g_prog[16]; static int g_nprog;
static int g_nreq, g_stop, g_taken_reset;
static pthread_t g_th;
static void *guest(void *arg) {
    (void)arg;
    while (!RD(g_stop)) {
        HvVusbMsg m;
        if (!hv_vusb_take_request(g_u, &m)) { usleep(500); continue; }
        int k = __atomic_fetch_add(&g_nreq, 1, __ATOMIC_ACQ_REL);
        if (m.type == HV_VUSB_REQ_RESET) __atomic_add_fetch(&g_taken_reset, 1, __ATOMIC_SEQ_CST);
        free(m.data);
        Act a = k < g_nprog ? g_prog[k] : (Act){0, m.type == HV_VUSB_REQ_RESET ? HV_VUSB_RSP_RESET : HV_VUSB_RSP_DATA, 0x77, 0};
        if (a.bounce) { hv_vusb_set_online(g_u, 0); hv_vusb_set_online(g_u, 1); }
        if (a.delay_ms) { int t0 = msnow(); while (!RD(g_stop) && msnow() - t0 < a.delay_ms) usleep(1000); }
        if (a.type) hv_vusb_put_response(g_u, (uint8_t)a.type, 0, &a.byte, a.type == HV_VUSB_RSP_DATA ? 1 : 0);
    }
    return NULL;
}
static void dev_new(int strict, const Act *prog, int n) {
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    if (g_realtime_cv) pthread_cond_init(&g_u->cv, NULL); else hv_vusb_cv_init(g_u);
    hv_vusb_set_online(g_u, 1);
    if (hv_usb_set_strict(g_u, strict) != 0) { printf("FAIL set_strict at set-up refused\n"); bad++; }
    g_nprog = n; for (int i = 0; i < n; i++) g_prog[i] = prog[i];
    ST(g_nreq, 0); ST(g_stop, 0); ST(g_taken_reset, 0);
    pthread_create(&g_th, NULL, guest, NULL);
}
static void dev_end(void) { ST(g_stop, 1); pthread_join(g_th, NULL); }
static int ctl(uint8_t *b, int timeout, volatile int *cancel) { return hv_usb_control_c(g_u, 0x80, 6, 0x100, 0, b, 1, timeout, cancel); }

static void s_basic(void) {
    Act p[] = {{0, HV_VUSB_RSP_DATA, 0x11, 0}, {0, HV_VUSB_RSP_STALL, 0, 0}};
    dev_new(1, p, 2);
    uint8_t b = 0;
    int r = ctl(&b, 1000, NULL);
    EXPECT(r == 1 && b == 0x11 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "basic: a settled answer: data 0x11, back to CLEAR (%d)", r);
    r = ctl(&b, 1000, NULL);
    EXPECT(r == HV_USB_ESTALL && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "basic: a STALL is a settled answer: CLEAR (%d)", r);
    EXPECT(hv_usb_reset(g_u, 1000) == 0 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "basic: RESET with the RESET answer: ok, CLEAR");
    dev_end();
}

static void s_timeout(void) {   /* A is never answered in time; the late answer comes; B must not be sent */
    Act p[] = {{400, HV_VUSB_RSP_DATA, 0xAA, 0}};
    dev_new(1, p, 1);
    uint8_t b = 0;
    int t0 = msnow(), r = ctl(&b, 120, NULL), took = msnow() - t0;
    EXPECT(r == HV_USB_ETIMEOUT && took >= 100 && took < 600 && hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN, "timeout: A times out after %d ms; the endpoint is UNCERTAIN", took);
    int sent = RD(g_nreq);
    r = ctl(&b, 500, NULL);
    EXPECT(r == HV_USB_EUNCERTAIN && RD(g_nreq) == sent, "SAFE_BLOCKED timeout: B is refused before anything is sent (%d), the guest saw no new request", r);
    usleep(600000);   /* A's late answer is in the ring by now */
    r = ctl(&b, 500, NULL);
    EXPECT(r == HV_USB_EUNCERTAIN && b == 0 && hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN, "SAFE_BLOCKED timeout: after A's late answer arrived, B is still refused and got no data (b=%02x); the late answer does not release UNCERTAIN", b);
    EXPECT(hv_usb_reset(g_u, 300) == HV_USB_EUNCERTAIN && RD(g_taken_reset) == 0, "SAFE_BLOCKED timeout: RESET is refused in UNCERTAIN too, none reached the guest");
    hv_vusb_set_online(g_u, 0); hv_vusb_set_online(g_u, 1);
    EXPECT(ctl(&b, 300, NULL) == HV_USB_EUNCERTAIN, "SAFE_BLOCKED timeout: a new session does not release it either");
    hv_usb_ctrl_reacquire(g_u);
    r = ctl(&b, 1000, NULL);
    EXPECT(r == 1 && b == 0x77 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "SAFE_RECOVERED timeout: after the explicit hv_usb_ctrl_reacquire the endpoint works (data %02x)", b);
    dev_end();
}

static void s_legacy(void) {   /* the same sequence without strict mode: the hazard this closes */
    Act p[] = {{300, HV_VUSB_RSP_DATA, 0xAA, 0}, {600, HV_VUSB_RSP_DATA, 0xBB, 0}};
    dev_new(0, p, 2);
    uint8_t b = 0;
    int r = ctl(&b, 100, NULL);
    EXPECT(r == HV_USB_ETIMEOUT, "legacy: A times out (%d)", r);
    r = ctl(&b, 2000, NULL);
    EXPECT(r == 1 && b == 0xAA, "legacy (the hazard): B is handed A's late answer, %02x instead of its own BB: MISATTRIBUTED, which strict mode refuses by not sending B", b);
    dev_end();
}

static int *g_race_c;
static int g_race_delay;
static void *race_setter(void *a) { (void)a; usleep((useconds_t)RD(g_race_delay) * 1000); __atomic_store_n(g_race_c, 1, __ATOMIC_SEQ_CST); return NULL; }
static void s_cancel(void) {
    int c = 1;
    Act none[] = {{0, 0, 0, 0}};
    dev_new(1, none, 1);
    uint8_t b;
    EXPECT(ctl(&b, 1000, &c) == HV_USB_ECANCELLED && RD(g_nreq) == 0 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "cancel: set before the request: nothing sent, CLEAR kept");
    dev_end();
    dev_new(1, none, 1);
    c = 0;
    pthread_t t;
    g_race_c = &c; ST(g_race_delay, 100);
    pthread_create(&t, NULL, race_setter, NULL);
    int t0 = msnow(), r = ctl(&b, 5000, &c), took = msnow() - t0;
    pthread_join(t, NULL);
    EXPECT(r == HV_USB_ECANCELLED && took < 600 && hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN, "cancel: set during the wait: returned after %d ms (not the 5000 ms deadline), UNCERTAIN, since the request was sent", took);
    EXPECT(ctl(&b, 300, NULL) == HV_USB_EUNCERTAIN, "SAFE_BLOCKED cancel: the next request is refused");
    dev_end();
}

static void s_race(void) {   /* the answer and the cancel at (nearly) the same time: whichever settles first, the state and the result agree */
    int done = 0, canc = 0, incons = 0;
    for (int i = 0; i < 40; i++) {
        int c = 0;
        Act p[] = {{60, HV_VUSB_RSP_DATA, 0x42, 0}};
        dev_new(1, p, 1);
        g_race_c = &c; ST(g_race_delay, 30 + i * 2);
        pthread_t t;
        pthread_create(&t, NULL, race_setter, NULL);
        uint8_t b = 0;
        int r = ctl(&b, 3000, &c);
        pthread_join(t, NULL);
        int st = hv_usb_ctrl_state(g_u);
        if (r == 1 && b == 0x42 && st == HV_CTRL_CLEAR) done++;
        else if (r == HV_USB_ECANCELLED && st == HV_CTRL_UNCERTAIN) canc++;
        else incons++;
        dev_end();
    }
    EXPECT(incons == 0 && done + canc == 40, "race: 40 runs, answer vs cancel: %d completed (CLEAR), %d cancelled (UNCERTAIN), %d inconsistent", done, canc, incons);
}

static int g_res[2]; static uint8_t g_resb[2];
static void *conc(void *a) { long i = (long)a; uint8_t b = 0; int rr_ = hv_usb_control_c(g_u, 0x80, 6, 0x100, 0, &b, 1, 2000, NULL); ST(g_resb[i], b); ST(g_res[i], rr_); return NULL; }
static void s_concurrent(void) {
    Act p[] = {{250, HV_VUSB_RSP_DATA, 0x31, 0}, {0, HV_VUSB_RSP_DATA, 0x32, 0}};
    dev_new(1, p, 2);
    pthread_t t[2];
    pthread_create(&t[0], NULL, conc, (void *)0);
    usleep(50000);
    pthread_create(&t[1], NULL, conc, (void *)1);
    pthread_join(t[0], NULL); pthread_join(t[1], NULL);
    EXPECT(RD(g_res[0]) == 1 && RD(g_resb[0]) == 0x31, "concurrent: the first caller got its own answer (31): %d %02x", RD(g_res[0]), RD(g_resb[0]));
    EXPECT(RD(g_res[1]) == HV_USB_EBUSY && RD(g_nreq) == 1, "concurrent: the second was refused while the first was in flight and was never sent (%d, guest saw %d request)", RD(g_res[1]), RD(g_nreq));
    dev_end();
}

static void s_reset(void) {
    Act p[] = {{300, HV_VUSB_RSP_DATA, 0x21, 0}, {0, HV_VUSB_RSP_RESET, 0, 0}, {0, HV_VUSB_RSP_DATA, 0x22, 0}};
    dev_new(1, p, 3);
    pthread_t t;
    pthread_create(&t, NULL, conc, (void *)0);   /* a control request in flight */
    usleep(60000);
    EXPECT(hv_usb_reset(g_u, 300) == HV_USB_EBUSY && RD(g_taken_reset) == 0, "reset: refused while a control request is in flight; none reached the guest");
    pthread_join(t, NULL);
    EXPECT(RD(g_res[0]) == 1 && RD(g_resb[0]) == 0x21, "reset: the control request still got its own answer");
    EXPECT(hv_usb_reset(g_u, 1000) == 0, "reset: now (CLEAR) a RESET goes through and takes the RESET answer");
    dev_end();
    /* a RESET answer that comes while a control request waits is not its answer */
    Act q[] = {{300, HV_VUSB_RSP_DATA, 0x23, 0}};
    dev_new(1, q, 1);
    hv_vusb_put_response(g_u, HV_VUSB_RSP_RESET, 0, NULL, 0);
    uint8_t b = 0;
    int r = ctl(&b, 1500, NULL);
    EXPECT(r == 1 && b == 0x23, "reset: a stray RESET answer in the ring is not taken for the control answer (data %02x, result %d)", b, r);
    dev_end();
    /* and a control answer is not taken for the RESET's */
    Act w[] = {{200, HV_VUSB_RSP_RESET, 0, 0}};
    dev_new(1, w, 1);
    hv_vusb_put_response(g_u, HV_VUSB_RSP_DATA, 0, "x", 1);
    EXPECT(hv_usb_reset(g_u, 1500) == 0, "reset: a stray control answer in the ring is not taken for the RESET answer");
    dev_end();
}

static void s_session(void) {
    Act none[] = {{0, 0, 0, 0}};
    dev_new(1, none, 1);
    uint8_t b;
    hv_vusb_set_online(g_u, 0);
    int r = ctl(&b, 300, NULL);
    EXPECT(r == HV_USB_EGONE && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR && RD(g_nreq) == 0, "session: offline: refused at the queue, nothing was queued, CLEAR (%d)", r);
    dev_end();
    /* the connection changes while A waits: reported as gone at once, not as a timeout; and the endpoint is UNCERTAIN across the new session */
    Act bnc[] = {{0, 0, 0, 1}};   /* the guest takes the request, leaves and comes back, answers nothing */
    dev_new(1, bnc, 1);
    int t0 = msnow();
    r = ctl(&b, 3000, NULL);
    int took = msnow() - t0;
    EXPECT(r == HV_USB_EGONE && took < 800, "session: the connection changed after the request was queued: EGONE after %d ms, not the 3000 ms timeout", took);
    EXPECT(hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN && ctl(&b, 300, NULL) == HV_USB_EUNCERTAIN, "SAFE_BLOCKED session: the endpoint stays UNCERTAIN in the new session (isolation of answers across sessions is not shown)");
    dev_end();
}

static HvVirtioUsb *g_hu;
static int g_hstop;
static void *hammer_ctl(void *a) { (void)a; uint8_t b; while (!RD(g_hstop)) { hv_usb_control_c(g_hu, 0x80, 6, 0x100, 0, &b, 1, 40, NULL); hv_usb_ctrl_reacquire(g_hu); } return NULL; }
static void *hammer_reset(void *a) { (void)a; while (!RD(g_hstop)) { hv_usb_reset(g_hu, 40); hv_usb_ctrl_reacquire(g_hu); } return NULL; }
static void *hammer_session(void *a) { (void)a; while (!RD(g_hstop)) { hv_vusb_set_online(g_hu, 0); usleep(2000); hv_vusb_set_online(g_hu, 1); usleep(3000); } return NULL; }
static void s_locks(void) {
    dev_new(1, NULL, 0);
    g_hu = g_u; ST(g_hstop, 0);
    pthread_t t[5];
    pthread_create(&t[0], NULL, hammer_ctl, NULL); pthread_create(&t[1], NULL, hammer_ctl, NULL);
    pthread_create(&t[2], NULL, hammer_reset, NULL); pthread_create(&t[3], NULL, hammer_session, NULL);
    sleep(2);
    ST(g_hstop, 1);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    EXPECT(1, "locks: 2 s of control requests, RESETs, reacquires and session changes from 4 threads ended without a deadlock (the script's timeout would call it HUNG)");
    dev_end();
}

static void s_cleanup(void) {   /* no answer ever: UNCERTAIN stays, and the teardown does not wait for it */
    Act none[] = {{0, 0, 0, 0}};
    dev_new(1, none, 1);
    uint8_t b;
    ctl(&b, 100, NULL);
    int t0 = msnow();
    hv_vusb_set_online(g_u, 0);
    dev_end();
    int took = msnow() - t0;
    EXPECT(hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN && took < 500, "cleanup: with no answer coming, UNCERTAIN is just a value: going offline and stopping the guest took %d ms (synthetic objects only; says nothing about the whole host)", took);
}


static void *rstcall(void *a) { (void)a; ST(g_res[1], hv_usb_reset(g_u, 2000)); return NULL; }
static void s_contract(void) {
    uint8_t b;
    /* CLEAR: nothing to reacquire */
    dev_new(1, NULL, 0);
    uint32_t g0 = g_u->cgen;
    EXPECT(hv_usb_ctrl_reacquire(g_u) == HV_USB_ESTATE && g_u->cgen == g0 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "contract: reacquire in CLEAR is refused (ESTATE), generation untouched");
    EXPECT(g_u->cbusy == 0, "contract: no call is counted inside when none runs");
    dev_end();
    /* PENDING: neither reacquire nor a mode change; the request still gets its own answer */
    Act p[] = {{300, HV_VUSB_RSP_DATA, 0x51, 0}};
    dev_new(1, p, 1);
    pthread_t t;
    pthread_create(&t, NULL, conc, (void *)0);
    usleep(80000);
    /* the generation is not read here (the worker writes it under usb_host.c's lock): a refusal that bumped it would show below, as the request in flight failing with EIO */
    EXPECT(hv_usb_ctrl_reacquire(g_u) == HV_USB_EBUSY, "contract: reacquire while PENDING is refused (EBUSY)");
    EXPECT(hv_usb_set_strict(g_u, 0) == HV_USB_EBUSY && g_u->cstrict == 1, "contract: set_strict while PENDING is refused (EBUSY), mode unchanged");
    pthread_join(t, NULL);
    EXPECT(RD(g_res[0]) == 1 && RD(g_resb[0]) == 0x51 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR && g_u->cbusy == 0, "contract: the request in flight got its own answer after the refused calls (%d %02x), so the generation was not touched; nothing left counted", RD(g_res[0]), RD(g_resb[0]));
    dev_end();
    /* RESETTING */
    Act q[] = {{300, HV_VUSB_RSP_RESET, 0, 0}};
    dev_new(1, q, 1);
    pthread_create(&t, NULL, rstcall, NULL);
    usleep(80000);
    EXPECT(hv_usb_ctrl_state(g_u) == HV_CTRL_RESETTING && hv_usb_ctrl_reacquire(g_u) == HV_USB_EBUSY && hv_usb_set_strict(g_u, 0) == HV_USB_EBUSY, "contract: during a RESET both reacquire and set_strict are refused (EBUSY)");
    pthread_join(t, NULL);
    EXPECT(RD(g_res[1]) == 0 && g_u->cbusy == 0, "contract: the RESET itself finished normally (%d)", RD(g_res[1]));
    dev_end();
    /* UNCERTAIN: the mode cannot be turned off from under it; the one allowed release works and is the only way */
    Act none[] = {{0, 0, 0, 0}};
    dev_new(1, none, 1);
    EXPECT(ctl(&b, 80, NULL) == HV_USB_ETIMEOUT && hv_usb_ctrl_state(g_u) == HV_CTRL_UNCERTAIN, "contract: set-up for UNCERTAIN");
    EXPECT(hv_usb_set_strict(g_u, 0) == HV_USB_EUNCERTAIN && g_u->cstrict == 1, "contract: set_strict in UNCERTAIN is refused (EUNCERTAIN): strict cannot be dropped to forget an unsettled request");
    EXPECT(hv_usb_ctrl_reacquire(g_u) == 0 && hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR, "contract: reacquire in UNCERTAIN with no call inside releases (SAFE_RECOVERED only in the test's sense: the caller vouches for isolation, nothing here shows it)");
    EXPECT(hv_usb_set_strict(g_u, 0) == 0 && g_u->cstrict == 0 && hv_usb_set_strict(g_u, 1) == 0 && g_u->cstrict == 1, "contract: from CLEAR with nothing inside, the mode can be changed");
    dev_end();
    /* a legacy call in flight: CLEAR alone does not mean idle */
    Act lp[] = {{300, HV_VUSB_RSP_DATA, 0x52, 0}};
    dev_new(0, lp, 1);
    pthread_create(&t, NULL, conc, (void *)0);
    usleep(80000);
    EXPECT(hv_usb_ctrl_state(g_u) == HV_CTRL_CLEAR && hv_usb_set_strict(g_u, 1) == HV_USB_EBUSY && g_u->cstrict == 0, "contract: a legacy call in flight (state still CLEAR): set_strict is refused (EBUSY): the mode is not switched under it");
    pthread_join(t, NULL);
    EXPECT(RD(g_res[0]) == 1 && RD(g_resb[0]) == 0x52 && hv_usb_set_strict(g_u, 1) == 0, "contract: the legacy call kept its own answer; after it left, the mode can change");
    dev_end();
}

static HvVirtioUsb *g_su; static int g_sstop, g_sbad;
static void *flip(void *a) { (void)a; int i = 0; while (!RD(g_sstop)) { hv_usb_set_strict(g_su, i++ & 1); if (hv_usb_ctrl_state(g_su) == HV_CTRL_UNCERTAIN && !g_su->cstrict) __atomic_add_fetch(&g_sbad, 1, __ATOMIC_SEQ_CST); usleep(300); } return NULL; }
static void *callr(void *a) { (void)a; uint8_t b; while (!RD(g_sstop)) { int r = hv_usb_control_c(g_su, 0x80, 6, 0x100, 0, &b, 1, 15, NULL); if (r > 1 || r < -8) __atomic_add_fetch(&g_sbad, 1, __ATOMIC_SEQ_CST); if (r == HV_USB_EUNCERTAIN) hv_usb_ctrl_reacquire(g_su); } return NULL; }
static void s_modeflip(void) {   /* set_strict and control calls racing: the mode is read once per call; ASan finds anything worse */
    Act none[] = {{0, 0, 0, 0}};
    dev_new(1, none, 1);
    g_su = g_u; ST(g_sstop, 0); ST(g_sbad, 0);
    pthread_t t[3];
    pthread_create(&t[0], NULL, flip, NULL); pthread_create(&t[1], NULL, callr, NULL); pthread_create(&t[2], NULL, callr, NULL);
    sleep(2);
    ST(g_sstop, 1);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    EXPECT(RD(g_sbad) == 0 && g_u->cbusy == 0, "modeflip: 2 s of mode changes against control calls: no result outside the defined codes, strict never dropped while UNCERTAIN, nothing left counted inside (%d odd, cbusy %d)", RD(g_sbad), g_u->cbusy);
    dev_end();
}

static void s_clock(void) {
    for (int rt = 0; rt < 2; rt++) {
        g_realtime_cv = rt;
        Act none[] = {{0, 0, 0, 0}};
        dev_new(0, none, 1);
        EXPECT(rt || g_u->cmono == 1, "clock(%s): the condvar clock flag is as the creator made it (cmono=%d; 1 = monotonic)", rt ? "realtime fallback" : "monotonic", g_u->cmono);
        EXPECT(!rt || g_u->cmono == 0, "clock(%s): a creator that does its own init keeps cmono=0", rt ? "realtime fallback" : "monotonic");
        uint8_t b;
        int t0 = msnow(), r = ctl(&b, 150, NULL), took = msnow() - t0;   /* the legacy hv_vusb_recv path */
        EXPECT(r == HV_USB_ETIMEOUT && took >= 120 && took < 700, "clock(%s): the legacy wait ends at its timeout: %d ms (%d)", rt ? "realtime fallback" : "monotonic", took, r);
        dev_end();
        g_realtime_cv = 0;
        dev_new(1, none, 1);
        t0 = msnow(); r = ctl(&b, 130, NULL); took = msnow() - t0;       /* hv_vusb_recv_ex */
        EXPECT(r == HV_USB_ETIMEOUT && took >= 100 && took < 700, "clock(%s): the strict wait ends at its timeout: %d ms (%d)", rt ? "realtime fallback" : "monotonic", took, r);
        dev_end();
    }
    EXPECT(1, "clock: a wall-clock jump cannot be made here: not shown that a jump leaves the waits alone (the strict deadline is monotonic; with cmono=1 so is each slice)");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(150);
    const char *sc = argc > 1 ? argv[1] : "all";
    int all = !strcmp(sc, "all");
    if (all || !strcmp(sc, "basic")) s_basic();
    if (all || !strcmp(sc, "timeout")) s_timeout();
    if (all || !strcmp(sc, "legacy")) s_legacy();
    if (all || !strcmp(sc, "cancel")) s_cancel();
    if (all || !strcmp(sc, "race")) s_race();
    if (all || !strcmp(sc, "concurrent")) s_concurrent();
    if (all || !strcmp(sc, "reset")) s_reset();
    if (all || !strcmp(sc, "session")) s_session();
    if (all || !strcmp(sc, "locks")) s_locks();
    if (all || !strcmp(sc, "cleanup")) s_cleanup();
    if (all || !strcmp(sc, "contract")) s_contract();
    if (all || !strcmp(sc, "modeflip")) s_modeflip();
    if (all || !strcmp(sc, "clock")) s_clock();
    printf("%s test_usb_ctrl %s: %d failed\n", bad ? "FAIL" : "PASS", sc, bad);
    return bad ? 1 : 0;
}
