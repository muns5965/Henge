/* g7-vbl-diag: the GIC's watch counters (devices/gicv3.c, hv_gicv3_watch / hv_gicv3_watch_get), driven through the real GICv3 code on any Linux.
   The machine is a bare HvMachine and the MMIO registration is captured; nothing else of the engine is linked.
   What this shows: the counters count pend / coalesced pend / pend while active / pend while disabled / IAR / EOIR / DIR of ONE SPI and nothing else, and that
   watching changes no delivery decision (the same register sequence gives the same IAR values and IRQ line with the watch on and off).
   What it does NOT show: anything about the guest or about which interrupts macOS takes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "../core/machine.h"
#include "../devices/gicv3.h"

static uint64_t (*dist_r)(void *, uint64_t, unsigned);
static void (*dist_w)(void *, uint64_t, uint64_t, unsigned);
static void *dist_o;
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque, uint64_t (*r)(void *, uint64_t, unsigned), void (*w)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    (void)pa, (void)size;
    if (!strcmp(name, "gicv3-dist")) dist_r = r, dist_w = w, dist_o = opaque;
    m->nmmio++;
    return 0;
}
void hv_cpu_wake(HvCpu *c) { (void)c; }

#define ICC(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))
#define ICC_IAR1 ICC(3, 0, 12, 12, 0)
#define ICC_EOIR1 ICC(3, 0, 12, 12, 1)
#define ICC_DIR ICC(3, 0, 12, 11, 1)
#define ICC_CTLR ICC(3, 0, 12, 12, 4)
#define ICC_PMR ICC(3, 0, 4, 6, 0)
#define ICC_IGRPEN1 ICC(3, 0, 12, 12, 7)

static int bad, n;
#define EXPECT(cond, ...) do { int ok_ = !!(cond); n++; if (!ok_) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static HvMachine M;
static HvCpu C;
static HvGic *G;
static void dw(uint64_t off, uint32_t v) { dist_w(dist_o, off, v, 4); }

static void setup(void) {
    memset(&M, 0, sizeof M); memset(&C, 0, sizeof C);
    M.ncpus = 1;
    G = hv_gicv3_create(&M, 0x1000000, 0x2000000);
    dw(0, 3);
    M.icc_write(&M, &C, ICC_PMR, 0xff);
    M.icc_write(&M, &C, ICC_IGRPEN1, 1);
    dw(0x84, 1u << 1);          /* group 1: INTID 33 */
    dw(0x104, 1u << 1);         /* enable 33 */
    dw(0x400 + 32, 0x80u << 8); /* priority of INTID 33 = 0x80 (byte 33) */
}

/* A fixed register sequence; returns a trace of what the CPU interface said, so the watch-on and watch-off runs can be compared. */
static void sequence(uint64_t *trace, int *nt) {
    int k = 0;
    hv_gicv3_pend(G, 33);                       trace[k++] = (uint64_t)M.percpu[0].irq_line;
    hv_gicv3_pend(G, 33);                       trace[k++] = (uint64_t)M.percpu[0].irq_line;
    trace[k++] = M.icc_read(&M, &C, ICC_IAR1);                     /* 33 */
    trace[k++] = (uint64_t)M.percpu[0].irq_line;                   /* active: line drops */
    hv_gicv3_pend(G, 33);                       trace[k++] = (uint64_t)M.percpu[0].irq_line;   /* pend while active: still no line */
    M.icc_write(&M, &C, ICC_EOIR1, 33);         trace[k++] = (uint64_t)M.percpu[0].irq_line;   /* the latched pend comes through */
    trace[k++] = M.icc_read(&M, &C, ICC_IAR1);                     /* 33 again */
    M.icc_write(&M, &C, ICC_EOIR1, 33);         trace[k++] = (uint64_t)M.percpu[0].irq_line;
    trace[k++] = M.icc_read(&M, &C, ICC_IAR1);                     /* spurious */
    hv_gicv3_pend(G, 34);                       trace[k++] = (uint64_t)M.percpu[0].irq_line;   /* not enabled: no line */
    *nt = k;
}

int main(void) {
    HvGicWatch w; HvGicWatchState s;
    uint64_t t_off[32], t_on[32]; int n_off, n_on;

    setup();
    EXPECT(hv_gicv3_watch_get(G, &w, &s) == 0, "nothing watched: get returns 0");
    EXPECT(hv_gicv3_watch_get(NULL, &w, &s) == 0, "NULL gic: get returns 0");
    hv_gicv3_watch(NULL, 33);   /* must not crash */
    hv_gicv3_pend(G, 33);
    EXPECT(hv_gicv3_watch_get(G, &w, &s) == 0, "pend with no watch: nothing counted");
    M.icc_read(&M, &C, ICC_IAR1); M.icc_write(&M, &C, ICC_EOIR1, 33);
    setup(); sequence(t_off, &n_off);

    setup();
    hv_gicv3_watch(G, 7); EXPECT(hv_gicv3_watch_get(G, &w, &s) == 0, "SGI/PPI number: watch stays off");
    hv_gicv3_watch(G, 9999); EXPECT(hv_gicv3_watch_get(G, &w, &s) == 0, "out of range: watch stays off");
    hv_gicv3_watch(G, 33);
    sequence(t_on, &n_on);
    EXPECT(n_on == n_off && !memcmp(t_on, t_off, (size_t)n_on * sizeof t_on[0]), "watching changes no delivery decision (same IAR / IRQ-line trace)");
    EXPECT(hv_gicv3_watch_get(G, &w, &s) == 1 && w.intid == 33, "watch on SPI 33");
    EXPECT(w.pend_calls == 3, "pend_calls = 3 (got %llu)", (unsigned long long)w.pend_calls);
    EXPECT(w.pend_coalesced == 1, "second pend while latched is coalesced (got %llu)", (unsigned long long)w.pend_coalesced);
    EXPECT(w.pend_while_active == 1, "pend between IAR and EOIR counted (got %llu)", (unsigned long long)w.pend_while_active);
    EXPECT(w.pend_disabled == 0, "SPI 34 pend is not counted for SPI 33");
    EXPECT(w.iar == 2 && w.eoir == 2 && w.dir == 0, "iar=2 eoir=2 dir=0 (got %llu %llu %llu)", (unsigned long long)w.iar, (unsigned long long)w.eoir, (unsigned long long)w.dir);
    EXPECT(w.active_ns_n == 2 && w.active_ns_max >= w.active_ns_sum / 2, "two IAR->EOIR intervals measured");
    EXPECT(w.first_pend_ns && w.first_iar_ns >= w.first_pend_ns && w.last_eoir_ns >= w.last_iar_ns, "timestamps ordered");
    EXPECT(s.active == 0 && s.latch == 0 && s.enabled == 1 && s.group == 1 && s.priority == 0x80 && s.target_cpu == 0, "state snapshot after the sequence");

    /* EOImode 1: EOIR drops priority only, DIR deactivates; both are counted and the interrupt stays active in between. */
    setup(); hv_gicv3_watch(G, 33);
    M.icc_write(&M, &C, ICC_CTLR, 2);
    hv_gicv3_pend(G, 33);
    EXPECT(M.icc_read(&M, &C, ICC_IAR1) == 33, "EOImode 1: IAR");
    M.icc_write(&M, &C, ICC_EOIR1, 33);
    hv_gicv3_watch_get(G, &w, &s);
    EXPECT(s.active == 1 && w.eoir == 1 && w.dir == 0, "EOImode 1: after EOIR still active, eoir=1");
    hv_gicv3_pend(G, 33);
    hv_gicv3_watch_get(G, &w, &s);
    EXPECT(w.pend_while_active == 1 && M.percpu[0].irq_line == 0, "EOImode 1: pend while active-after-EOIR is held back");
    M.icc_write(&M, &C, ICC_DIR, 33);
    hv_gicv3_watch_get(G, &w, &s);
    EXPECT(s.active == 0 && w.dir == 1 && w.active_ns_n == 1 && M.percpu[0].irq_line == 1, "EOImode 1: DIR deactivates, counted, the held pend comes through");

    /* pend while the SPI is disabled */
    setup(); hv_gicv3_watch(G, 33);
    dw(0x184, 1u << 1);   /* ICENABLER1: disable 33 */
    hv_gicv3_pend(G, 33);
    hv_gicv3_watch_get(G, &w, &s);
    EXPECT(w.pend_disabled == 1 && s.enabled == 0 && s.latch == 1 && M.percpu[0].irq_line == 0, "pend while disabled: counted, latched, no line");
    hv_gicv3_watch(G, 33);
    hv_gicv3_watch_get(G, &w, &s);
    EXPECT(w.pend_calls == 0 && s.latch == 1, "re-arming the watch resets the counters, not the GIC");

    printf("%s test_gicwatch (%d checks, %d failed)\n", bad ? "FAIL" : "PASS", n, bad);
    return bad ? 1 : 0;
}
