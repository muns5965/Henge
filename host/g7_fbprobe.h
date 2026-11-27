#ifndef HENGEVM_G7_FBPROBE_H
#define HENGEVM_G7_FBPROBE_H
/* Read-only boot-framebuffer probe (header-only so tests/test_g7_fbprobe.c compiles the same code). It only READS the pixels it is given and formats log lines; it never writes guest memory.
   Lines (all on stderr, so they land in the same g7 log as the hengevm: lines):
     g7-boot-fb: boot_args at ram+<off> rev=<u> ver=<u> virtBase=<hex> physBase=<hex> memSize=<hex> topOfKernelData=<hex> v_baseAddr=<hex> v_display=<hex> v_rowBytes=<n> v_width=<n> v_height=<n> v_depth=<hex> valid=<0|1> why=<word> ram_base=<hex> ram_size=<hex>
         once per boot_args found (valid=0 when the Video values would not be used as a framebuffer; why= says which check failed). Raw 8-byte slots 4..10 of boot_args; the field order is the one host/vm.c already assumes (INFER: XNU arm64 boot_args).
     g7-boot-fb: scan none t=<s>s    (at most 5 times, ~2 s apart: no boot_args-shaped page found yet in the first 1 GiB of guest RAM)
     g7-boot-fb: kind=first|periodic|change|slow|final t=<s>s fb=<W>x<H> rowBytes=<n> width*4=<n> rgb_nonzero=<ppm>ppm alpha_nonmax=<ppm>ppm luma8=<8 counts> bbox_rgb=<x0,y0,x1,y1|none> samples=<n> hash=<hex16> changed=<0|1|first> nchanges=<n> (...)
         Observed every ~2 s for the whole boot (never stops). Lines: first = immediately, periodic <= 30, then change <= 60 (hash differs) and slow <= 30 (every 60 s), plus one final at VM stop. All numbers come from a SAMPLED grid (<= 256x256),
         so a change confined to unsampled pixels is NOT seen. rgb_nonzero = any of bytes 0..2 != 0; alpha_nonmax = byte 3 != 255 (INFER: BGRA or RGBA, not told apart). t = seconds since the first probe call (INFER: close to VM start).
     g7-boot-fb: src t=<s>s <word> [<W>x<H>]   the first time each display source is seen (pvg_frame, pvg_surface_read, pvg_surface_nonblack, pvg_surface_black, pvg_surface_none, boot_args_used), at most 8 lines.
   Nothing here allocates, locks or sleeps. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HV_FBP_MAX_LINES 30          /* periodic lines (kind=first/periodic); observation itself never stops */
#define HV_FBP_MAX_CHANGE_LINES 60   /* extra lines after the periodic cap, one per hash change */
#define HV_FBP_MAX_SLOW_LINES 30     /* extra lines after the periodic cap, one per 60 s */
#define HV_FBP_SLOW_PERIOD_S 60.0
#define HV_FBP_MAX_SCAN_LINES 5
#define HV_FBP_MAX_SRC_LINES 8
#define HV_FBP_PERIOD_S 2.0
#define HV_FBP_SAMPLE_GRID 256   /* about 256 x 256 = 65536 sampled pixels at most */

typedef struct {
    uint64_t samples, rgb_nonzero, alpha_nonmax, hash;
    uint64_t luma[8];                 /* (R+G+B)/3 >> 5 per sampled pixel; symmetric in R/B so BGRA and RGBA give the same histogram */
    int bbox_valid; uint32_t x0, y0, x1, y1;   /* bounding box of sampled pixels with RGB != 0 (pixel coordinates of the sample points) */
} HvFbpSummary;

/* Samples a grid of at most HV_FBP_SAMPLE_GRID x HV_FBP_SAMPLE_GRID pixels (4 bytes each, stride in bytes). INFER: byte 3 is alpha, bytes 0..2 are colour (BGRA or RGBA; not told apart). Returns 0, or -1 for an unusable region. */
static inline int hv_fbp_summarize(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, HvFbpSummary *o) {
    if (!o) return -1;
    memset(o, 0, sizeof *o); o->hash = 1469598103934665603ull;
    if (!px || !w || !h || stride < (uint64_t)w * 4) return -1;
    uint32_t sx = (w + HV_FBP_SAMPLE_GRID - 1) / HV_FBP_SAMPLE_GRID, sy = (h + HV_FBP_SAMPLE_GRID - 1) / HV_FBP_SAMPLE_GRID;
    for (uint32_t y = 0; y < h; y += sy) {
        const uint8_t *row = px + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x += sx) {
            const uint8_t *q = row + (size_t)x * 4;
            uint32_t v = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
            o->samples++;
            if (v & 0x00ffffffu) {
                o->rgb_nonzero++;
                if (!o->bbox_valid) { o->bbox_valid = 1; o->x0 = o->x1 = x; o->y0 = o->y1 = y; }
                else { if (x < o->x0) o->x0 = x; if (x > o->x1) o->x1 = x; if (y < o->y0) o->y0 = y; if (y > o->y1) o->y1 = y; }
            }
            if (q[3] != 0xff) o->alpha_nonmax++;
            o->luma[((uint32_t)q[0] + q[1] + q[2]) / 3u >> 5]++;
            o->hash = (o->hash ^ v) * 1099511628211ull;
        }
    }
    return 0;
}

static inline uint64_t hv_fbp_ppm_of(uint64_t k, uint64_t n) { return n ? k * 1000000ull / n : 0; }

typedef struct {
    int have_prev, lines, scan_lines, src_lines, nchanges, change_lines, slow_lines;
    uint64_t prev_hash;
    double t0, last_tick, last_emit;   /* t0: first call; last_tick: last observation; last_emit: last emitted line */
    unsigned src_seen;                 /* bit per source word */
    int first_done;
} HvFbpState;

static inline int hv_fbp_format(HvFbpState *st, const char *kind, double now, const HvFbpSummary *s, uint32_t w, uint32_t h, uint32_t stride, const char *chg, char *line, size_t cap) {
    char bb[64];
    if (s->bbox_valid) snprintf(bb, sizeof bb, "%u,%u,%u,%u", s->x0, s->y0, s->x1, s->y1); else snprintf(bb, sizeof bb, "none");
    return snprintf(line, cap, "g7-boot-fb: kind=%s t=%.1fs fb=%ux%u rowBytes=%u width*4=%u rgb_nonzero=%lluppm alpha_nonmax=%lluppm luma8=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu bbox_rgb=%s samples=%llu hash=%016llx changed=%s nchanges=%d "
                    "(sampled grid <=256x256, not every pixel; INFER: 4 bytes/pixel, byte3=alpha, bytes0-2=colour, BGRA or RGBA unknown; luma=(b0+b1+b2)/3>>5)",
                    kind, now - st->t0, (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned)w * 4u, (unsigned long long)hv_fbp_ppm_of(s->rgb_nonzero, s->samples), (unsigned long long)hv_fbp_ppm_of(s->alpha_nonmax, s->samples),
                    (unsigned long long)s->luma[0], (unsigned long long)s->luma[1], (unsigned long long)s->luma[2], (unsigned long long)s->luma[3], (unsigned long long)s->luma[4], (unsigned long long)s->luma[5], (unsigned long long)s->luma[6], (unsigned long long)s->luma[7],
                    bb, (unsigned long long)s->samples, (unsigned long long)s->hash, chg, st->nchanges);
}

/* One observation step (call as often as you like; it observes every HV_FBP_PERIOD_S). now = monotonic seconds. Returns 1 when *line holds a line to log (no newline), else 0.
   Lines: kind=first (the first valid framebuffer, immediately), kind=periodic (until HV_FBP_MAX_LINES), then kind=change (hash differs from the previous observation, up to HV_FBP_MAX_CHANGE_LINES)
   and kind=slow (every 60 s, up to HV_FBP_MAX_SLOW_LINES). The caps limit LINES only: the hash and the change count keep being updated every observation.
   *changed_first = 1 the first time the hash differs (caller may save one PNG); *at_10s = 1 at the first observation at or after t=10 s. *is_first = 1 on the very first line. */
static inline int hv_fbp_tick(HvFbpState *st, double now, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, char *line, size_t cap, int *changed_first, int *at_10s) {
    if (changed_first) *changed_first = 0;
    if (at_10s) *at_10s = 0;
    if (st->t0 == 0) st->t0 = now;
    if (st->last_tick != 0 && now - st->last_tick < HV_FBP_PERIOD_S) return 0;
    HvFbpSummary s;
    if (hv_fbp_summarize(px, w, h, stride, &s) != 0) return 0;
    int changed = 0; const char *chg = "first";
    if (st->have_prev) {
        changed = st->prev_hash != s.hash; chg = changed ? "1" : "0";
        if (changed) { if (st->nchanges == 0 && changed_first) *changed_first = 1; st->nchanges++; }
    }
    if (at_10s && now - st->t0 >= 10.0 && !(st->last_tick != 0 && st->last_tick - st->t0 >= 10.0)) *at_10s = 1;
    st->have_prev = 1; st->prev_hash = s.hash; st->last_tick = now;
    const char *kind = NULL;
    if (!st->first_done) { kind = "first"; st->first_done = 1; st->lines++; }
    else if (st->lines < HV_FBP_MAX_LINES) { kind = "periodic"; st->lines++; }
    else if (changed && st->change_lines < HV_FBP_MAX_CHANGE_LINES) { kind = "change"; st->change_lines++; }
    else if (now - st->last_emit >= HV_FBP_SLOW_PERIOD_S && st->slow_lines < HV_FBP_MAX_SLOW_LINES) { kind = "slow"; st->slow_lines++; }
    if (!kind) return 0;
    st->last_emit = now;
    hv_fbp_format(st, kind, now, &s, w, h, stride, chg, line, cap);
    return 1;
}

/* The final-state line (VM stop). Always formatted (once is the caller's business); returns 0 when the region is unusable. Does not change the change statistics. */
static inline int hv_fbp_final(HvFbpState *st, double now, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, char *line, size_t cap) {
    HvFbpSummary s;
    if (st->t0 == 0 || hv_fbp_summarize(px, w, h, stride, &s) != 0) return 0;
    hv_fbp_format(st, "final", now, &s, w, h, stride, st->have_prev ? (st->prev_hash != s.hash ? "1" : "0") : "first", line, cap);
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

/* The kernel command line and the device-tree words of a boot_args page, as one log line (read-only; page = the boot_args page, page_len >= 0x1000). Offsets are the arm64 struct of XNU's
   pexpert/pexpert/arm64/boot.h (Apple open source, APSL 2.0), revision 2: Revision u16 @0, Version u16 @2, virtBase @8, physBase @16, memSize @24, topOfKernelData @32, Video (6 x u64) @40,
   machineType u32 @88, deviceTreeP u64 @96, deviceTreeLength u32 @104, CommandLine[] @108 (BOOT_LINE_LENGTH, 1024 for the simulator build). The Video words at @40 are the ones this file already
   uses; the guest here reports revision 3, whose tail layout is NOT established, so the line says INFER and shows how much of the text is printable (a wrong offset shows as 0 or as noise).
   Printable ASCII only (anything else is shown as '?'); at most HV_FBP_CMDLINE_SHOW characters. Returns 1. */
#define HV_FBP_CMDLINE_OFF 108u
#define HV_FBP_CMDLINE_MAX 1024u
#define HV_FBP_CMDLINE_SHOW 240u
static inline int hv_fbp_cmdline_line(char *line, size_t cap, const uint8_t *page, size_t page_len) {
    uint32_t mt = 0, dtl = 0; uint64_t dtp = 0; uint16_t rev = 0, ver = 0;
    if (page_len < HV_FBP_CMDLINE_OFF + 1) { snprintf(line, cap, "g7-boot-fb: boot_args cmdline: page too short (%zu bytes)", page_len); return 1; }
    memcpy(&rev, page, 2); memcpy(&ver, page + 2, 2); memcpy(&mt, page + 88, 4); memcpy(&dtp, page + 96, 8); memcpy(&dtl, page + 104, 4);
    size_t max = page_len - HV_FBP_CMDLINE_OFF; if (max > HV_FBP_CMDLINE_MAX) max = HV_FBP_CMDLINE_MAX;
    size_t n = 0; while (n < max && page[HV_FBP_CMDLINE_OFF + n]) n++;
    int terminated = n < max;
    size_t nonprint = 0; char txt[HV_FBP_CMDLINE_SHOW + 1]; size_t shown = n < HV_FBP_CMDLINE_SHOW ? n : HV_FBP_CMDLINE_SHOW;
    for (size_t i = 0; i < n; i++) { uint8_t c = page[HV_FBP_CMDLINE_OFF + i]; if (c < 0x20 || c >= 0x7f) nonprint++; }
    for (size_t i = 0; i < shown; i++) { uint8_t c = page[HV_FBP_CMDLINE_OFF + i]; txt[i] = (c < 0x20 || c >= 0x7f) ? '?' : (char)c; }
    txt[shown] = 0;
    snprintf(line, cap, "g7-boot-fb: boot_args tail (INFER: offsets 88/96/104/108 are XNU boot.h revision 2; this page says rev=%u ver=%u) machineType=%u deviceTreeP=%#llx deviceTreeLength=%u "
             "cmdline_len=%zu terminated=%d nonprintable=%zu shown=%zu cmdline=\"%s\"",
             (unsigned)rev, (unsigned)ver, (unsigned)mt, (unsigned long long)dtp, (unsigned)dtl, n, terminated, nonprint, shown, txt);
    return 1;
}
#endif
