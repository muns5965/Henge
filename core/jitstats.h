#ifndef HV_JITSTATS_H
#define HV_JITSTATS_H
/* Diagnostics for the translation cache (g7-jit2). Header-only and free of HvMachine, so the host
   tests (tests/test_jitstats.c, any CPU) build the very same code the dispatcher runs.

   "Was this block translated before?" is answered two ways:
   - the hashed bitmaps (2^24 bits each, 2 MiB): cheap, always on with --stats, approximate: a block
     never seen before is reported as seen when its hash collides with an earlier one
     (probability about 1 - exp(-distinct/2^24), no false negatives);
   - the exact set (HV_STATS_EXACT or G7_STATS_EXACT = <max entries in thousands>, default 2048): every (VA, PA, flags)
     with the page generation and flush epoch of its last translation. Open addressing, grows by
     doubling, 24 bytes a slot, at most 2^22 slots (96 MiB); beyond 70% load of the largest table it
     stops inserting and counts the overflow. Phone memory is tight: leave it off unless asked.
   Both are touched only under the translation lock. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HV_JSTAT_BITMAP_BITS 24
#define HV_JSTAT_BITMAP_WORDS (1u << (HV_JSTAT_BITMAP_BITS - 6))

/* The hashes dispatch.c has always used for retrans_va / retrans_vapa (kept bit for bit). */
static inline uint32_t hv_jstat_hash_va(uint64_t va, uint32_t flags) {
    return (uint32_t)(((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull >> 40);
}
static inline uint32_t hv_jstat_hash_vapa(uint64_t va, uint64_t pa, uint32_t flags) {
    return (uint32_t)((((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull ^ pa * 0xc2b2ae3d27d4eb4full) >> 40);
}
/* 1 if the bit was already set; sets it. */
static inline int hv_jstat_bitmap_test_set(uint64_t *bm, uint32_t h) {
    int was = (int)(bm[h >> 6] >> (h & 63) & 1);
    bm[h >> 6] |= 1ull << (h & 63);
    return was;
}

typedef struct {
    uint64_t va, pa_flags; /* pa_flags: the PA with the flags in bits 56..63 */
    uint32_t gen;          /* page generation at the last translation */
    uint32_t epoch1;       /* flush epoch of the last translation, plus one (0: empty slot) */
} HvJstatSlot;

typedef struct {
    HvJstatSlot *slot;
    uint64_t nslots, count, overflow;
    uint64_t max_slots; /* power of two */
} HvJstatExact;

static inline uint64_t hv_jstat_mix(uint64_t va, uint64_t pa_flags) {
    uint64_t x = va * 0x9e3779b97f4a7c15ull ^ (pa_flags + 0x632be59bd9b4e019ull) * 0xc2b2ae3d27d4eb4full;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    return x ^ (x >> 32);
}

/* max_entries: the most distinct keys to hold (rounded up to a power of two of slots at 70% load). */
static inline void hv_jstat_exact_init(HvJstatExact *s, uint64_t max_entries) {
    memset(s, 0, sizeof *s);
    uint64_t want = max_entries * 10 / 7 + 1, n = 1u << 12;
    while (n < want && n < (1ull << 22)) n <<= 1;
    s->max_slots = n;
}

static inline int hv_jstat_exact_grow(HvJstatExact *s) {
    uint64_t nn = s->nslots ? s->nslots * 2 : (1u << 12);
    if (nn > s->max_slots) return -1;
    HvJstatSlot *ns = (HvJstatSlot *)calloc(nn, sizeof *ns);
    if (!ns) return -1;
    for (uint64_t i = 0; i < s->nslots; i++) {
        HvJstatSlot *o = &s->slot[i];
        if (!o->epoch1) continue;
        uint64_t j = hv_jstat_mix(o->va, o->pa_flags) & (nn - 1);
        while (ns[j].epoch1) j = (j + 1) & (nn - 1);
        ns[j] = *o;
    }
    free(s->slot);
    s->slot = ns;
    s->nslots = nn;
    return 0;
}

/* Record a translation of (va, pa, flags) at page generation gen in flush epoch epoch.
   Returns: 0 new, 1 seen before (*old_gen and *old_epoch say when), -1 the table is full (not recorded). */
static inline int hv_jstat_exact_note(HvJstatExact *s, uint64_t va, uint64_t pa, uint32_t flags, uint32_t gen, uint32_t epoch,
                                      uint32_t *old_gen, uint32_t *old_epoch) {
    uint64_t pf = pa | (uint64_t)flags << 56, j;
    if (!s->nslots || (s->count + 1) * 10 > s->nslots * 7) {
        if (hv_jstat_exact_grow(s) != 0) {
            if (!s->nslots) {
                s->overflow++;
                return -1;
            }
            /* Cannot grow: still find an old key, but add no new one. */
            j = hv_jstat_mix(va, pf) & (s->nslots - 1);
            for (uint64_t probes = 0; s->slot[j].epoch1 && probes < s->nslots; probes++, j = (j + 1) & (s->nslots - 1))
                if (s->slot[j].va == va && s->slot[j].pa_flags == pf) goto found;
            s->overflow++;
            return -1;
        }
    }
    j = hv_jstat_mix(va, pf) & (s->nslots - 1);
    while (s->slot[j].epoch1) {
        if (s->slot[j].va == va && s->slot[j].pa_flags == pf) goto found;
        j = (j + 1) & (s->nslots - 1);
    }
    s->slot[j] = (HvJstatSlot){va, pf, gen, epoch + 1};
    s->count++;
    return 0;
found:
    *old_gen = s->slot[j].gen;
    *old_epoch = s->slot[j].epoch1 - 1;
    s->slot[j].gen = gen;
    s->slot[j].epoch1 = epoch + 1;
    return 1;
}

/* Which gap bucket a retranslation falls in: flush epochs since the last translation of the block
   (0: same epoch, which a lost race or a changed page can give; 1; 2; 3-4; 5 or more). */
static inline int hv_jstat_gap_bucket(uint32_t gap) {
    return gap == 0 ? 0 : gap == 1 ? 1 : gap == 2 ? 2 : gap <= 4 ? 3 : 4;
}

#endif
