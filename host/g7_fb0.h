#ifndef HENGEVM_G7_FB0_H
#define HENGEVM_G7_FB0_H
/* F1: early boot-framebuffer observation (research/g7-hjit2). Header-only so tests/test_g7_fb0.c compiles the same code. It only READS the pixels it is given and formats log lines; it never writes guest memory.
   The sampler thread in host/vm.c (g7_fb0_thread) calls these from t=0 of the VM start: every HV_FB0_PERIOD_MS for HV_FB0_SECONDS, then it stops for good. Only when env G7_FB0=1 (the app's start dialog
   sets it; default checked); without that variable no thread exists, so every other runner is unchanged.
   Lines (stderr, same g7 log as the hengevm: lines; a line is written ONLY when the hash differs from the previous sample, plus the first sample and one end line):
     g7-fb0: start period_ms=<n> seconds=<n> default_base=<hex> default_window=<bytes> (nz = 32-bit words with any of bytes 0..2 != 0 = "drawn pixels"; hash = 4-lane multiply/xor over all words of the window; changed lines only, max <n> lines)
     g7-fb0: t=<s>s nz=<n> of=<words> hash=<hex16> src=<raw|boot_args> base=<hex> win=<WxH|bytes> change=<first|nz_up|nz_down|same_nz> dump=<none|first_nz|big_change> (n=<scan index>)
         t = seconds since the sampler started (VM start, monotonic). src=raw: boot_args not (yet) found or its Video words unusable -> the fixed carveout window at default_base; src=boot_args: base/size from boot_args Video words.
         When src changes (boot_args appear) the previous sample is forgotten, so the next line has change=first. nz counts words, so nz and "of" are comparable only inside one src.
     g7-fb0: boot_args t=<s>s base=<hex> w=<n> h=<n> row=<n>     once, when the Video words become usable
     g7-fb0: dump fb0_<n>_<why>_t<ms>ms png started|NOT saved ... / raw <bytes> bytes saved|NOT saved ... / NOT saved (no dump folder ...)   one per dump
     g7-fb0: end t=<s>s scans=<n> total_ms=<x> max_ms=<x> lines=<n> suppressed=<n> dumps=<n> nz_first_t=<s|none> (cost measured in this process: total/max time spent in the scan itself)
   Dumps (at most HV_FB0_MAX_DUMPS = 3 per run, into LAGFX_ARM64_DUMP_DIR so the export carries them): #1 the first sample with nz > 0 (dump=first_nz), then a sample whose nz differs from the last dumped nz by a factor >= 4 up or down
   (dump=big_change). File g7_frame_fb0_<n>_<why>_t<ms>ms_<WxH>.png when the geometry is known (src=boot_args), else g7_fb0_<n>_<why>_t<ms>ms_raw<bytes>B.bin (the first HV_FB0_RAW_DUMP_MAX bytes of the window). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HV_FB0_PERIOD_MS 250
#define HV_FB0_SECONDS 60
#define HV_FB0_MAX_LINES 200
#define HV_FB0_MAX_DUMPS 3
#define HV_FB0_BIG_FACTOR 4
#define HV_FB0_DEFAULT_BASE 0xef540000ull     /* the boot_args framebuffer carveout seen in the G7 boots (docs/g7-analysis-notes.md); only used until boot_args give the real one */
#define HV_FB0_DEFAULT_WINDOW (9u << 20)      /* 9 MiB: a 1920x1080x4 frame with room to spare; clipped to guest RAM by the caller */
#define HV_FB0_RAW_DUMP_MAX (4u << 20)

typedef struct { uint64_t nz, words, hash; } HvFb0Sample;

/* Scans w x h 32-bit pixels (stride in bytes). nz = pixels with any of bytes 0..2 != 0 (byte 3 = alpha is ignored). hash: 4 independent multiply/xor lanes over the 64-bit pairs (two pixels), so it is cheap and does not stall on one chain. */
static inline void hv_fb0_scan(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, HvFb0Sample *o) {
    uint64_t nz = 0, l0 = 0x9e3779b97f4a7c15ull, l1 = 0xc2b2ae3d27d4eb4full, l2 = 0x165667b19e3779f9ull, l3 = 0x27d4eb2f165667c5ull;
    const uint64_t P = 0x100000001b3ull;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = px + (size_t)y * stride; uint32_t x = 0;
        for (; x + 8 <= w; x += 8) {
            uint32_t v[8]; uint64_t q[4]; memcpy(v, r + (size_t)x * 4, 32); memcpy(q, v, 32);
            for (int i = 0; i < 8; i++) nz += (v[i] & 0x00ffffffu) != 0;
            l0 = (l0 ^ q[0]) * P; l1 = (l1 ^ q[1]) * P; l2 = (l2 ^ q[2]) * P; l3 = (l3 ^ q[3]) * P;
        }
        for (; x < w; x++) { uint32_t v; memcpy(&v, r + (size_t)x * 4, 4); nz += (v & 0x00ffffffu) != 0; l0 = (l0 ^ v) * P; }
    }
    o->nz = nz; o->words = (uint64_t)w * h; o->hash = l0 ^ (l1 * 3) ^ (l2 * 5) ^ (l3 * 7);
}

typedef struct {
    int have_prev, lines, suppressed, dumps, scans, first_nz_done, src_prev;   /* src_prev: 0 none yet, 1 raw, 2 boot_args */
    uint64_t prev_hash, prev_nz, dump_nz;
    double t_first_nz;                                                       /* < 0 until seen */
} HvFb0State;

static inline void hv_fb0_init(HvFb0State *st) { memset(st, 0, sizeof *st); st->t_first_nz = -1; }

/* Decides what one sample means. src: 1 raw, 2 boot_args. Returns 1 when *line holds a log line (no newline). *dump = 0 none, 1 first_nz, 2 big_change (a dump is due; the dump counter is already advanced). */
static inline int hv_fb0_step(HvFb0State *st, double t, const HvFb0Sample *s, int src, uint64_t base, uint32_t w, uint32_t h, char *line, size_t cap, int *dump) {
    *dump = 0; st->scans++;
    if (src != st->src_prev) { st->have_prev = 0; st->src_prev = src; }   /* the basis changed: nz is not comparable with the earlier ones */
    const char *chg = "first";
    int changed = 1;
    if (st->have_prev) {
        changed = s->hash != st->prev_hash;
        chg = !changed ? "same_nz" : s->nz > st->prev_nz ? "nz_up" : s->nz < st->prev_nz ? "nz_down" : "same_nz";   /* a changed hash with an equal count is "same_nz": the pixels moved, not their number */
    }
    st->have_prev = 1; st->prev_hash = s->hash; st->prev_nz = s->nz;
    if (!changed) return 0;
    if (s->nz > 0 && !st->first_nz_done) { st->first_nz_done = 1; st->t_first_nz = t; if (st->dumps < HV_FB0_MAX_DUMPS) { st->dumps++; st->dump_nz = s->nz; *dump = 1; } }
    else if (s->nz > 0 && st->first_nz_done && st->dumps < HV_FB0_MAX_DUMPS && st->dump_nz > 0 &&
             (s->nz >= st->dump_nz * HV_FB0_BIG_FACTOR || s->nz * HV_FB0_BIG_FACTOR <= st->dump_nz)) { st->dumps++; st->dump_nz = s->nz; *dump = 2; }
    if (st->lines >= HV_FB0_MAX_LINES) { st->suppressed++; return 0; }
    st->lines++;
    char win[40]; if (src == 2) snprintf(win, sizeof win, "%ux%u", (unsigned)w, (unsigned)h); else snprintf(win, sizeof win, "%llu", (unsigned long long)(s->words * 4));
    snprintf(line, cap, "g7-fb0: t=%.2fs nz=%llu of=%llu hash=%016llx src=%s base=%#llx win=%s change=%s dump=%s (n=%d)", t, (unsigned long long)s->nz, (unsigned long long)s->words, (unsigned long long)s->hash,
             src == 2 ? "boot_args" : "raw", (unsigned long long)base, win, chg, *dump == 1 ? "first_nz" : *dump == 2 ? "big_change" : "none", st->scans);
    return 1;
}

static inline void hv_fb0_start_line(char *line, size_t cap) {
    snprintf(line, cap, "g7-fb0: start period_ms=%d seconds=%d default_base=%#llx default_window=%u (nz = 32-bit words with any of bytes 0..2 != 0; hash = 4-lane multiply/xor over all words; lines only when the hash changed, max %d lines, max %d dumps)",
             HV_FB0_PERIOD_MS, HV_FB0_SECONDS, (unsigned long long)HV_FB0_DEFAULT_BASE, (unsigned)HV_FB0_DEFAULT_WINDOW, HV_FB0_MAX_LINES, HV_FB0_MAX_DUMPS);
}
static inline void hv_fb0_end_line(const HvFb0State *st, double t, double total_ms, double max_ms, char *line, size_t cap) {
    char f[32]; if (st->t_first_nz < 0) snprintf(f, sizeof f, "none"); else snprintf(f, sizeof f, "%.2fs", st->t_first_nz);
    snprintf(line, cap, "g7-fb0: end t=%.1fs scans=%d total_ms=%.1f max_ms=%.2f lines=%d suppressed=%d dumps=%d nz_first_t=%s", t, st->scans, total_ms, max_ms, st->lines, st->suppressed, st->dumps, f);
}
#endif
