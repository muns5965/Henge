#ifndef HENGEVM_G7_FBPROBE_H
#define HENGEVM_G7_FBPROBE_H
/* Read-only boot-framebuffer probe (header-only so tests/test_g7_fbprobe.c compiles the same code). It only READS the pixels it is given and formats log lines; it never writes guest memory.
   Lines (all on stderr, so they land in the same g7 log as the hengevm: lines):
     g7-boot-fb: boot_args at ram+<off> rev=<u> ver=<u> virtBase=<hex> physBase=<hex> memSize=<hex> topOfKernelData=<hex> v_baseAddr=<hex> v_display=<hex> v_rowBytes=<n> v_width=<n> v_height=<n> v_depth=<hex> valid=<0|1> why=<word> ram_base=<hex> ram_size=<hex>
         once per boot_args found (valid=0 when the Video values would not be used as a framebuffer; why= says which check failed). Raw 8-byte slots 4..10 of boot_args; the field order is the one host/vm.c already assumes (INFER: XNU arm64 boot_args).
     g7-boot-fb: scan none t=<s>s    (at most 5 times, ~2 s apart: no boot_args-shaped page found yet in the first 1 GiB of guest RAM)
     g7-boot-fb: t=<s>s fb=<W>x<H> stride=<n> nonempty=<ppm>ppm(<k>/<n> samples) hash=<hex16> changed=<0|1|first> nchanges=<n>
         every ~2 s, at most 30 lines. nonempty = share of SAMPLED pixels whose colour bytes (alpha ignored) are not zero, in parts per million. The hash is FNV-1a over the same sample set, so a change
         confined to unsampled pixels is NOT seen (INFER: a logo or a progress bar is large enough to hit the grid). t = seconds since the first probe call (the display pump starting, INFER: close to VM start).
     g7-boot-fb: src t=<s>s <word> [<W>x<H>]   the first time each display source is seen (pvg_frame, pvg_surface_read, pvg_surface_nonblack, pvg_surface_black, pvg_surface_none, boot_args_used), at most 8 lines.
   Nothing here allocates, locks or sleeps. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HV_FBP_MAX_LINES 30
#define HV_FBP_MAX_SCAN_LINES 5
#define HV_FBP_MAX_SRC_LINES 8
#define HV_FBP_PERIOD_S 2.0
#define HV_FBP_SAMPLE_GRID 256   /* about 256 x 256 = 65536 sampled pixels at most */

typedef struct {
    uint64_t samples, nonempty, hash;
} HvFbpSummary;

/* Samples a grid of at most HV_FBP_SAMPLE_GRID x HV_FBP_SAMPLE_GRID pixels (BGRA, stride in bytes). Returns 0, or -1 for an unusable region. */
static inline int hv_fbp_summarize(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, HvFbpSummary *o) {
    if (!o) return -1;
    o->samples = o->nonempty = 0; o->hash = 1469598103934665603ull;
    if (!px || !w || !h || stride < (uint64_t)w * 4) return -1;
    uint32_t sx = (w + HV_FBP_SAMPLE_GRID - 1) / HV_FBP_SAMPLE_GRID, sy = (h + HV_FBP_SAMPLE_GRID - 1) / HV_FBP_SAMPLE_GRID;
    for (uint32_t y = 0; y < h; y += sy) {
        const uint8_t *row = px + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x += sx) {
            uint32_t v = (uint32_t)row[(size_t)x * 4] | (uint32_t)row[(size_t)x * 4 + 1] << 8 | (uint32_t)row[(size_t)x * 4 + 2] << 16 | (uint32_t)row[(size_t)x * 4 + 3] << 24;
            o->samples++;
            if (v & 0x00ffffffu) o->nonempty++;
            o->hash = (o->hash ^ v) * 1099511628211ull;
        }
    }
    return 0;
}

static inline uint64_t hv_fbp_ppm(const HvFbpSummary *s) { return s->samples ? s->nonempty * 1000000ull / s->samples : 0; }

typedef struct {
    int have_prev, lines, scan_lines, src_lines, nchanges;
    uint64_t prev_hash;
    double t0, last_tick;     /* t0: first call; last_tick: last emitted probe line */
    unsigned src_seen;        /* bit per source word */
} HvFbpState;

/* One probe step. now = monotonic seconds. Returns 0 nothing to log, else 1 and *line holds one text line (no newline); *changed_first = 1 the first time the hash differs from the previous one
   (the caller may then save one PNG); *at_10s = 1 the first emitted line at or after t=10 s. Rate and count caps live here. */
static inline int hv_fbp_tick(HvFbpState *st, double now, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, char *line, size_t cap, int *changed_first, int *at_10s) {
    if (changed_first) *changed_first = 0;
    if (at_10s) *at_10s = 0;
    if (st->t0 == 0) st->t0 = now;
    if (st->lines >= HV_FBP_MAX_LINES) return 0;
    if (st->last_tick != 0 && now - st->last_tick < HV_FBP_PERIOD_S) return 0;
    HvFbpSummary s;
    if (hv_fbp_summarize(px, w, h, stride, &s) != 0) return 0;
    const char *chg = "first";
    if (st->have_prev) {
        int c = st->prev_hash != s.hash;
        chg = c ? "1" : "0";
        if (c) { if (st->nchanges == 0 && changed_first) *changed_first = 1; st->nchanges++; }
    }
    if (at_10s && now - st->t0 >= 10.0 && !(st->last_tick != 0 && st->last_tick - st->t0 >= 10.0)) *at_10s = 1;
    st->have_prev = 1; st->prev_hash = s.hash; st->last_tick = now; st->lines++;
    snprintf(line, cap, "g7-boot-fb: t=%.1fs fb=%ux%u stride=%u nonempty=%lluppm(%llu/%llu samples) hash=%016llx changed=%s nchanges=%d",
             now - st->t0, (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned long long)hv_fbp_ppm(&s), (unsigned long long)s.nonempty, (unsigned long long)s.samples,
             (unsigned long long)s.hash, chg, st->nchanges);
    return 1;
}

/* First time a source word is seen (id 0..7 = bit): formats the line and returns 1, else 0. Capped at HV_FBP_MAX_SRC_LINES. */
static inline int hv_fbp_src(HvFbpState *st, double now, unsigned id, const char *word, uint32_t w, uint32_t h, char *line, size_t cap) {
    if (id >= 32 || (st->src_seen >> id & 1) || st->src_lines >= HV_FBP_MAX_SRC_LINES) return 0;
    if (st->t0 == 0) st->t0 = now;
    st->src_seen |= 1u << id; st->src_lines++;
    snprintf(line, cap, "g7-boot-fb: src t=%.1fs %s [%ux%u]", now - st->t0, word, (unsigned)w, (unsigned)h);
    return 1;
}

/* The raw Video words of a boot_args-shaped page (q = the page as 8-byte words, rev/ver its two u16 header fields). why = "ok" or the first failed check. */
static inline const char *hv_fbp_video_check(const uint64_t *q, uint64_t ram_base, uint64_t ram_size) {
    uint64_t base = q[5], row = q[7], w = q[8], hh = q[9], depth = q[10];
    if (w < 320 || w > 8192 || hh < 200 || hh > 8192) return "size";
    if (row < w * 4) return "rowbytes";
    if ((depth & 0xff) != 32) return "depth";
    if (base < ram_base || base - ram_base + row * hh > ram_size) return "baseaddr";
    return "ok";
}

static inline int hv_fbp_boot_args_line(char *line, size_t cap, uint64_t off, const uint16_t *hdr, const uint64_t *q, uint64_t ram_base, uint64_t ram_size) {
    const char *why = hv_fbp_video_check(q, ram_base, ram_size);
    snprintf(line, cap, "g7-boot-fb: boot_args at ram+%#llx rev=%u ver=%u virtBase=%#llx physBase=%#llx memSize=%#llx topOfKernelData=%#llx v_baseAddr=%#llx v_display=%#llx v_rowBytes=%llu v_width=%llu v_height=%llu v_depth=%#llx valid=%d why=%s ram_base=%#llx ram_size=%#llx",
             (unsigned long long)off, (unsigned)hdr[0], (unsigned)hdr[1], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
             (unsigned long long)q[5], (unsigned long long)q[6], (unsigned long long)q[7], (unsigned long long)q[8], (unsigned long long)q[9], (unsigned long long)q[10],
             why[0] == 'o' && why[1] == 'k', why, (unsigned long long)ram_base, (unsigned long long)ram_size);
    return 1;
}
#endif
