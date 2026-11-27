#ifndef HENGEVM_PVG_VBLDIAG_H
#define HENGEVM_PVG_VBLDIAG_H

/* DIAGNOSTICS ONLY (research/g7-vbl-diag). Nothing in this header changes what the device does: it keeps counters and formats bounded log lines.
   Header-only and free of the machine / Metal types so tests/test_pvg.c (any Linux) can drive it directly.

   What it records, and what each number can and cannot tell apart (see docs/research/vbl-diag-2026-11-27.md):
     - the FIRST VBL_READ_SAMPLES guest reads of each of the three ISR registers (0x1014 display cause, 0x1018 stamps, 0x102c fault) with the value returned and
       the shared page's +0x0 / +0x100 / +0x104 words at that instant, the time since the first tick and since the previous read;
     - the first VBL_TICK_SAMPLES ticks: the page's +0x100 before and after our OR, the cause word, the enable word;
     - totals: per register reads / nonzero, reads before the first tick, whether the guest read 0x1014 between two ticks, and whether +0x100 was already zero
       by the time the same ISR read 0x102c (a heuristic on the order 0x1018, 0x1014, 0x102c seen in the g7 logs; it is NOT proof of which code cleared the word).
   A register READ proves only that the guest's interrupt handler ran. It does not prove the guest acted on the value. Page bit, MMIO cause and GIC pending/active
   are three different things; the GIC side is counted separately (hv_gicv3_watch). */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VBL_READ_SAMPLES 24u   /* logged reads per register */
#define VBL_TICK_SAMPLES 24u   /* logged ticks */
#define VBL_REGS 3u

typedef struct HvVblDiag {
    uint64_t epoch_ns;                       /* first call of any note function: t=+0 */
    uint64_t first_tick_ns;
    uint64_t first_read_ns[VBL_REGS];        /* first guest read of each register, ever */
    uint64_t first_read_after_tick_ns;       /* first 0x1014 read at or after the first tick */
    uint64_t first_nonzero_cause_ns;         /* first 0x1014 read that returned nonzero */
    uint64_t last_read_ns[VBL_REGS];
    uint64_t reads[VBL_REGS], nonzero[VBL_REGS], reads_before_tick[VBL_REGS];
    uint64_t ticks, ticks_unread_prev;       /* ticks sent / ticks sent while +0x100 still had the bit */
    uint64_t ticks_guest_read_since_prev, ticks_no_guest_read_since_prev;
    uint64_t reads1014_at_prev_tick;
    uint64_t p100_zero_at_102c, p100_set_at_102c;   /* order heuristic, see above */
    uint32_t p100_seen_at_1014;              /* +0x100 as the last 0x1014 read saw it (ISR entry) */
    uint32_t read_logged[VBL_REGS], tick_logged;
    uint32_t p100_union_at_1014;             /* OR of every +0x100 value seen at an ISR entry */
    uint64_t sum_last_ns; uint32_t sum_lines;   /* the caller's summary pacing (pvg_lite.c) */
    uint32_t cause_union;                    /* OR of every 0x1014 value returned */
} HvVblDiag;

#define VBL_LD(p) __atomic_load_n(&(p), __ATOMIC_RELAXED)
#define VBL_ADD(p, n) __atomic_fetch_add(&(p), (n), __ATOMIC_RELAXED)
#define VBL_OR(p, n) __atomic_fetch_or(&(p), (n), __ATOMIC_RELAXED)

/* Sets the epoch and, once, the "first" timestamps. Returns now - epoch. */
static inline uint64_t hv_vbldiag_epoch(HvVblDiag *d, uint64_t now) {
    uint64_t z = 0;
    __atomic_compare_exchange_n(&d->epoch_ns, &z, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    uint64_t e = VBL_LD(d->epoch_ns);
    return now >= e ? now - e : 0;
}
static inline void hv_vbldiag_once(uint64_t *slot, uint64_t now) {
    uint64_t z = 0;
    __atomic_compare_exchange_n(slot, &z, now ? now : 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}
static inline int hv_vbldiag_reg_index(uint64_t off) {
    return off == 0x1014 ? 0 : off == 0x1018 ? 1 : off == 0x102c ? 2 : -1;
}
/* Milliseconds with one decimal as a pair of integers, for the line formatter below. */
#define VBL_MS_ARGS(ns) (unsigned long long)((ns) / 1000000ull), (unsigned long long)(((ns) / 100000ull) % 10ull)

/* A guest read of register `off` (0x1014, 0x1018 or 0x102c) returned `value`; p0/p100/p104 are the shared page's words read just before the register was read (the
   caller passes 0 and has_page = 0 when no display page exists yet). Returns 1 when `line` holds a line to print (the first VBL_READ_SAMPLES of that register). */
static inline int hv_vbldiag_note_read(HvVblDiag *d, uint64_t off, uint32_t value, int has_page, uint32_t p0, uint32_t p100, uint32_t p104, uint64_t now, char *line, size_t cap) {
    int r = hv_vbldiag_reg_index(off);
    if (r < 0) return 0;
    uint64_t t = hv_vbldiag_epoch(d, now);
    uint64_t prev = VBL_LD(d->last_read_ns[r]);
    uint64_t n = VBL_ADD(d->reads[r], 1) + 1;
    __atomic_store_n(&d->last_read_ns[r], now ? now : 1, __ATOMIC_RELAXED);
    hv_vbldiag_once(&d->first_read_ns[r], now);
    uint64_t ft = VBL_LD(d->first_tick_ns);
    if (!ft) VBL_ADD(d->reads_before_tick[r], 1);
    if (value) VBL_ADD(d->nonzero[r], 1);
    if (r == 0) {
        if (ft) hv_vbldiag_once(&d->first_read_after_tick_ns, now);
        if (value) hv_vbldiag_once(&d->first_nonzero_cause_ns, now);
        VBL_OR(d->cause_union, value);
        if (has_page) { __atomic_store_n(&d->p100_seen_at_1014, p100 | 0x80000000u, __ATOMIC_RELAXED); VBL_OR(d->p100_union_at_1014, p100); }
    } else if (r == 2 && has_page) {
        uint32_t seen = __atomic_exchange_n(&d->p100_seen_at_1014, 0, __ATOMIC_RELAXED);
        if ((seen & 0x80000000u) && (seen & 0x7fffffffu)) { if (p100 == 0) VBL_ADD(d->p100_zero_at_102c, 1); else VBL_ADD(d->p100_set_at_102c, 1); }
    }
    uint32_t k = VBL_ADD(d->read_logged[r], 1);
    if (k >= VBL_READ_SAMPLES || !line || !cap) return 0;
    uint64_t since_tick = ft && now >= ft ? now - ft : 0, since_prev = prev && now >= prev ? now - prev : 0;
    int w = snprintf(line, cap, "hengevm: pvg-lite: vbl-diag read #%llu +%04x = 0x%x t=+%llu.%llums", (unsigned long long)n, (unsigned)off, (unsigned)value, VBL_MS_ARGS(t));
    if (w > 0 && (size_t)w < cap) w += snprintf(line + w, cap - (size_t)w, " since_first_tick=%s%llu.%llums since_prev_read=%s%llu.%llums", ft ? "+" : "(no tick yet) ", VBL_MS_ARGS(since_tick), prev ? "+" : "(first) ", VBL_MS_ARGS(since_prev));
    if (w > 0 && (size_t)w < cap) {
        if (has_page) snprintf(line + w, cap - (size_t)w, " page+0=%x page+100=%x page+104=%x", (unsigned)p0, (unsigned)p100, (unsigned)p104);
        else snprintf(line + w, cap - (size_t)w, " (no display page yet)");
    }
    return 1;
}

/* One VBL tick sent by display_events_tick(): `en` = the guest's enable word, `bits` = what we ORed into +0x100, p100_before / p100_after = +0x100 around the OR,
   cause = the value handed to the 0x1014 path. Returns 1 when `line` holds a line to print (the first VBL_TICK_SAMPLES). */
static inline int hv_vbldiag_note_tick(HvVblDiag *d, uint32_t en, uint32_t bits, uint32_t p100_before, uint32_t p100_after, uint32_t cause, uint64_t now, char *line, size_t cap) {
    uint64_t t = hv_vbldiag_epoch(d, now);
    uint64_t n = VBL_ADD(d->ticks, 1) + 1;
    hv_vbldiag_once(&d->first_tick_ns, now);
    if (p100_before & bits) VBL_ADD(d->ticks_unread_prev, 1);
    uint64_t r1014 = VBL_LD(d->reads[0]), prev = VBL_LD(d->reads1014_at_prev_tick);
    if (n > 1) { if (r1014 > prev) VBL_ADD(d->ticks_guest_read_since_prev, 1); else VBL_ADD(d->ticks_no_guest_read_since_prev, 1); }
    __atomic_store_n(&d->reads1014_at_prev_tick, r1014, __ATOMIC_RELAXED);
    uint32_t k = VBL_ADD(d->tick_logged, 1);
    if (k >= VBL_TICK_SAMPLES || !line || !cap) return 0;
    snprintf(line, cap, "hengevm: pvg-lite: vbl-diag tick #%llu t=+%llu.%llums enable=%x bits=%x cause=%x page+100 before=%x after=%x reads_of_1014_since_prev_tick=%llu",
             (unsigned long long)n, VBL_MS_ARGS(t), (unsigned)en, (unsigned)bits, (unsigned)cause, (unsigned)p100_before, (unsigned)p100_after, (unsigned long long)(n > 1 ? r1014 - prev : 0));
    return 1;
}

/* The totals as one line. `t_now` is the caller's clock. */
static inline size_t hv_vbldiag_summary(HvVblDiag *d, uint64_t now, char *out, size_t cap) {
    uint64_t e = VBL_LD(d->epoch_ns), t = e && now >= e ? now - e : 0;
    uint64_t ft = VBL_LD(d->first_tick_ns), fr = VBL_LD(d->first_read_after_tick_ns), fn = VBL_LD(d->first_nonzero_cause_ns);
    uint64_t f0 = VBL_LD(d->first_read_ns[0]);
    int w = snprintf(out, cap, "hengevm: pvg-lite: vbl-diag summary t=+%llu.%llums reads[1014]=%llu(nonzero %llu, before_first_tick %llu) reads[1018]=%llu(nonzero %llu) reads[102c]=%llu(nonzero %llu) cause_union=%x ",
                     VBL_MS_ARGS(t), (unsigned long long)VBL_LD(d->reads[0]), (unsigned long long)VBL_LD(d->nonzero[0]), (unsigned long long)VBL_LD(d->reads_before_tick[0]),
                     (unsigned long long)VBL_LD(d->reads[1]), (unsigned long long)VBL_LD(d->nonzero[1]), (unsigned long long)VBL_LD(d->reads[2]), (unsigned long long)VBL_LD(d->nonzero[2]), (unsigned)VBL_LD(d->cause_union));
    if (w < 0 || (size_t)w >= cap) return cap ? cap - 1 : 0;
    w += snprintf(out + w, cap - (size_t)w, "first_read[1014]=%s%llu.%llums first_tick=%s%llu.%llums first_1014_read_after_tick=%s%llu.%llums first_nonzero_cause=%s%llu.%llums ",
                  f0 ? "+" : "none:", VBL_MS_ARGS(f0 > e ? f0 - e : 0), ft ? "+" : "none:", VBL_MS_ARGS(ft > e ? ft - e : 0), fr ? "+" : "none:", VBL_MS_ARGS(fr > e ? fr - e : 0), fn ? "+" : "none:", VBL_MS_ARGS(fn > e ? fn - e : 0));
    if (w < 0 || (size_t)w >= cap) return cap ? cap - 1 : 0;
    w += snprintf(out + w, cap - (size_t)w, "ticks=%llu ticks_with_1014_read_since_prev=%llu ticks_without=%llu ticks_event_bit_still_set=%llu isr_page100_at_entry_union=%x page100_zero_by_102c=%llu page100_still_set_at_102c=%llu",
                  (unsigned long long)VBL_LD(d->ticks), (unsigned long long)VBL_LD(d->ticks_guest_read_since_prev), (unsigned long long)VBL_LD(d->ticks_no_guest_read_since_prev),
                  (unsigned long long)VBL_LD(d->ticks_unread_prev), (unsigned)VBL_LD(d->p100_union_at_1014), (unsigned long long)VBL_LD(d->p100_zero_at_102c), (unsigned long long)VBL_LD(d->p100_set_at_102c));
    return w < 0 ? 0 : (size_t)w < cap ? (size_t)w : cap - 1;
}

#endif
