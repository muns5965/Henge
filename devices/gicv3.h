#ifndef HENGEVM_GICV3_H
#define HENGEVM_GICV3_H

#include <pthread.h>
#include <stdint.h>

#include "machine.h"

/* GICv3 with one security state (DS=1), no LPIs or ITS: the distributor, one
   redistributor per CPU, and each CPU's ICC_* system registers. INTIDs 0-15
   are SGIs, 16-31 PPIs (both banked per CPU), 32 and up SPIs. */
#define HV_GIC_NUM_IRQS (256 + 32) /* vmapple: 256 SPIs */

typedef struct HvGicActive {
    uint16_t intid;
    uint8_t prio, group;
} HvGicActive;

/* One CPU's redistributor (its SGIs and PPIs) and CPU interface. */
typedef struct HvGicCpu {
    uint8_t group[32], grpmod[32], enabled[32], latch[32], level[32], active[32], priority[32], edge[32];
    uint32_t waker;
    uint64_t pmr, bpr0, bpr1, icc_ctlr, igrpen0, igrpen1, sre;
    uint64_t ap0r, ap1r;         /* active priority bits, one per priority level >> 3 */
    HvGicActive stack[32];       /* acknowledged, not yet priority-dropped */
    int depth;
} HvGicCpu;

/* DIAGNOSTICS ONLY (g7-vbl-diag): counters for ONE watched SPI (the gfx interrupt). Nothing here changes what the GIC does; all fields are written under the GIC's own lock.
   pend_coalesced: a pend arrived while the interrupt was already latched pending (two pends, one IAR). pend_while_active: a pend arrived between its IAR and its EOIR/DIR.
   pend_disabled: a pend arrived while the SPI's enable bit was clear. active_ns: IAR to the EOIR/DIR that cleared it (EOImode 0: EOIR; EOImode 1: DIR). */
typedef struct HvGicWatch {
    int on, intid;
    uint64_t pend_calls, pend_coalesced, pend_while_active, pend_disabled;
    uint64_t iar, eoir, dir, eoir_without_active;
    uint64_t first_pend_ns, first_iar_ns, last_pend_ns, last_iar_ns, last_eoir_ns;
    uint64_t active_ns_max, active_ns_sum, active_ns_n, iar_ns_cur;
} HvGicWatch;
/* The watched SPI's state at the moment of the snapshot (what the GIC would use to decide delivery). */
typedef struct HvGicWatchState {
    uint8_t latch, level, active, enabled, edge, group, priority;
    int target_cpu, irq_line_cpu0, fiq_line_cpu0;
    uint64_t pmr_cpu0, icc_ctlr_cpu0, igrpen1_cpu0;
    int depth_cpu0;
} HvGicWatchState;

typedef struct HvGic {
    HvMachine *m;
    pthread_mutex_t mu;            /* the GIC's own lock (see gicv3.c) */
    uint32_t ctlr;                 /* GICD_CTLR */
    /* SPIs (INTIDs 32 and up; entries below 32 are unused: see HvGicCpu) */
    uint8_t group[HV_GIC_NUM_IRQS];
    uint8_t grpmod[HV_GIC_NUM_IRQS];
    uint8_t enabled[HV_GIC_NUM_IRQS];
    uint8_t latch[HV_GIC_NUM_IRQS];  /* edge or software pending */
    uint8_t level[HV_GIC_NUM_IRQS];  /* input line */
    uint8_t active[HV_GIC_NUM_IRQS];
    uint8_t priority[HV_GIC_NUM_IRQS];
    uint8_t edge[HV_GIC_NUM_IRQS];   /* ICFGR: 1 = edge-triggered */
    uint64_t route[HV_GIC_NUM_IRQS];
    uint64_t spi_on[(HV_GIC_NUM_IRQS + 63) / 64]; /* SPIs that may be enabled (a superset: hppi checks each) */
    HvGicCpu cpu[HV_MAX_CPUS];
    HvGicWatch watch;              /* diagnostics only, see HvGicWatch */
} HvGic;

HvGic *hv_gicv3_create(HvMachine *m, uint64_t dist_pa, uint64_t redist_pa);
/* Drive an SPI's input line (32+). */
void hv_gicv3_set_irq(HvGic *g, int intid, int level);
/* Drive one CPU's PPI (16-31). */
void hv_gicv3_set_ppi(HvGic *g, int cpu, int intid, int level);
/* Make an SPI pending, as an MSI write does. */
void hv_gicv3_pend(HvGic *g, int intid);
/* Diagnostics only: count pends / IAR / EOIR / DIR for SPI `intid` (32+; anything else turns it off). Resets the counters. Does not change delivery. */
void hv_gicv3_watch(HvGic *g, int intid);
/* Copies the counters and the SPI's current state under the GIC's lock. Returns 0 when nothing is watched. Either pointer may be NULL. */
int hv_gicv3_watch_get(HvGic *g, HvGicWatch *w, HvGicWatchState *st);
/* Recompute every CPU's IRQ and FIQ inputs (waking any that just got one). */
void hv_gicv3_update(HvGic *g);

#endif
