/* Host test of core/jitstats.h (any CPU): the hashed "translated before" bitmaps and the exact set the dispatcher uses
   for its retranslation figures. Says nothing about the JIT itself (that cannot run off arm64). Prints the numbers the
   report (docs/research/jit-reverification.md) quotes. */
#include "../core/jitstats.h"

#include <math.h>
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 0x123456789abcdefull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* The formulas as dispatch.c had them before g7-jit2 (copied, not shared): the move into jitstats.h must not change a bit. */
static uint32_t old_hv(uint64_t va, uint32_t flags) { return (uint32_t)(((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull >> 40); }
static uint32_t old_hp(uint64_t va, uint64_t pa, uint32_t flags) { return (uint32_t)((((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull ^ pa * 0xc2b2ae3d27d4eb4full) >> 40); }

static void t_hash_same(void) {
    for (int i = 0; i < 100000; i++) {
        uint64_t va = rnd(), pa = rnd() & 0xffffffffffull;
        uint32_t f = (uint32_t)rnd() & 15;
        CHECK(hv_jstat_hash_va(va, f) == old_hv(va, f) && hv_jstat_hash_vapa(va, pa, f) == old_hp(va, pa, f), "hash changed");
    }
    CHECK(hv_jstat_hash_va(~0ull, 15) < (1u << HV_JSTAT_BITMAP_BITS), "hash out of the bitmap");
}

/* Expected fraction of the N inserted distinct keys that find their bit set already: the mean of 1-exp(-i/M) over i<N. */
static double expect_fp(double n) { double m = 1 << HV_JSTAT_BITMAP_BITS; return 1 - (m / n) * (1 - exp(-n / m)); }

static void fp_run(const char *shape, int which, int which_map, uint64_t n) {
    uint64_t *bm = (uint64_t *)calloc(HV_JSTAT_BITMAP_WORDS, 8);
    uint64_t fp = 0;
    uint64_t va = 0xfffffe0010000000ull, pa = 0x800000000ull;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t flags = (uint32_t)(i & 1) * 1; /* EL0/EL1 alternating where the shape says so */
        switch (which) {
        case 0: va = (rnd() & 0x0000ffffffffffffull) & ~3ull; pa = rnd() & 0xffffffffc000ull; flags = (uint32_t)rnd() & 3; break; /* random */
        case 1: va += 4 * (1 + (rnd() & 15)); pa = (va & 0x3fffffff) + 0x800000000ull; flags = 0; break;                              /* kernel text walked in order: ~5 insns a block */
        case 2: va = 0x100000000ull + (i % 4096) * 0x4000 + 4 * (i / 4096 % 4096) + 0x4000ull * 4096 * (i / (4096 * 4096)); pa = 0x800000000ull + ((i / 4096) << 14); flags = 0; break; /* user pages, distinct offsets, PA per group */
        }
        int b = which_map ? hv_jstat_bitmap_test_set(bm, hv_jstat_hash_vapa(va, pa, flags)) : hv_jstat_bitmap_test_set(bm, hv_jstat_hash_va(va, flags));
        fp += (uint64_t)b; /* keys of a shape can repeat by chance: counted as seen, which is right unless the shape repeats; random 48-bit ones do not */
    }
    free(bm);
    double got = (double)fp / (double)n, want = expect_fp((double)n);
    printf("  %s bitmap false positives, %-18s N=%8llu: %.3f%% (analytic %.3f%%)\n", which_map ? "va+pa" : "va   ", shape, (unsigned long long)n, 100 * got, 100 * want);
    double sd = sqrt(want * (1 - want) / (double)n);
    if (which == 0) /* random keys: the analytic figure */
        CHECK(fabs(got - want) < 6 * sd + 0.004 * want + 0.0005, "%s N=%llu: %.4f against %.4f", shape, (unsigned long long)n, got, want);
    else /* structured keys (a multiplicative hash of an arithmetic run collides less than random, of a two-level one a bit more): never far above analytic */
        CHECK(got < 1.5 * want + 0.001, "%s N=%llu: %.4f against %.4f", shape, (unsigned long long)n, got, want);
}

static void t_bitmap_fp(void) {
    printf("hashed bitmap, 2^%d bits:\n", HV_JSTAT_BITMAP_BITS);
    static const uint64_t ns[] = {500000, 1000000, 2000000, 4000000};
    for (int k = 0; k < 4; k++) {
        for (int mp = 0; mp < 2; mp++) {
            fp_run("random VA+PA", 0, mp, ns[k]);
            fp_run("ordered kernel text", 1, mp, ns[k]);
            fp_run("user pages", 2, mp, ns[k]);
        }
    }
    /* no false negatives: a key set twice is all seen the second time */
    uint64_t *bm = (uint64_t *)calloc(HV_JSTAT_BITMAP_WORDS, 8);
    uint64_t save = rng;
    for (int i = 0; i < 100000; i++) hv_jstat_bitmap_test_set(bm, hv_jstat_hash_vapa(rnd(), rnd(), 1));
    rng = save;
    int miss = 0;
    for (int i = 0; i < 100000; i++) miss += !hv_jstat_bitmap_test_set(bm, hv_jstat_hash_vapa(rnd(), rnd(), 1));
    CHECK(miss == 0, "false negatives %d", miss);
    free(bm);
}

/* What the log's "again %" turns into once the collisions are taken out: observed = true again + new * q, q = 1-exp(-distinct/2^24). */
static void t_qualify_table(void) {
    printf("over-count of 'again' by the bitmap (true again -> reported again), by distinct blocks so far:\n");
    static const double distinct[] = {0.8e6, 1.3e6, 1.75e6, 2.0e6, 2.7e6};
    static const double again[] = {0.60, 0.85, 0.91, 0.98};
    for (int d = 0; d < 5; d++) {
        printf("  distinct %.2fM (q=%.1f%%):", distinct[d] / 1e6, 100 * (1 - exp(-distinct[d] / (1 << 24))));
        for (int a = 0; a < 4; a++) {
            double q = 1 - exp(-distinct[d] / (1 << 24)), rep = again[a] + (1 - again[a]) * q;
            printf("  %.0f%%->%.1f%%", 100 * again[a], 100 * rep);
            CHECK(rep >= again[a] && rep - again[a] <= q, "model");
        }
        printf("\n");
    }
}

typedef struct { uint64_t va, pa; uint32_t flags, gen, epoch; int seen; } Ref;

static void t_exact(void) {
    enum { POOL = 60000, OPS = 400000 };
    Ref *pool = (Ref *)calloc(POOL, sizeof *pool);
    for (int i = 0; i < POOL; i++) {
        pool[i].va = (rnd() & 0xffffffffffffull) & ~3ull;
        pool[i].pa = rnd() & 0xffffffffc000ull;
        pool[i].flags = (uint32_t)rnd() & 3;
    }
    HvJstatExact s;
    hv_jstat_exact_init(&s, 100000);
    int bad = 0, nnew = 0, again_same = 0, again_new = 0;
    uint32_t epoch = 0;
    for (int op = 0; op < OPS; op++) {
        if (op % 7919 == 0) epoch++;
        int i = (int)(rnd() % POOL);
        uint32_t gen = pool[i].seen ? pool[i].gen + (rnd() % 5 == 0) : 0;
        uint32_t og = 99, oe = 99;
        int r = hv_jstat_exact_note(&s, pool[i].va, pool[i].pa, pool[i].flags, gen, epoch, &og, &oe);
        if (r != pool[i].seen) bad++;
        else if (r == 1) {
            if (og != pool[i].gen || oe != pool[i].epoch) bad++;
            if (og == gen) again_same++; else again_new++;
        } else nnew++;
        pool[i].seen = 1, pool[i].gen = gen, pool[i].epoch = epoch;
    }
    CHECK(bad == 0, "exact set disagrees with the reference %d times", bad);
    CHECK(s.count == (uint64_t)nnew && s.overflow == 0, "count %llu, new %d, overflow %llu", (unsigned long long)s.count, nnew, (unsigned long long)s.overflow);
    printf("exact set: %d ops over %d keys: new %d, again same page %d, again page changed %d; %llu slots\n", OPS, POOL, nnew, again_same, again_new, (unsigned long long)s.nslots);
    free(s.slot);

    /* overflow: a table limited to ~2.8k keys keeps finding its own and counts the rest */
    hv_jstat_exact_init(&s, 1000);
    int stored = 0, over = 0;
    for (int i = 0; i < 10000; i++) {
        uint32_t og, oe;
        int r = hv_jstat_exact_note(&s, 4ull * (uint64_t)i + 0x1000, 0x80000000ull, 0, 1, 0, &og, &oe);
        if (r == 0) stored++; else if (r < 0) over++; else CHECK(0, "a new key reported seen");
    }
    CHECK(stored > 2000 && stored <= 4096 * 7 / 10 + 1 && over == 10000 - stored && s.overflow == (uint64_t)over, "stored %d over %d overflow %llu", stored, over, (unsigned long long)s.overflow);
    int found = 0;
    for (int i = 0; i < 10000 && found < stored; i++) {
        uint32_t og, oe;
        found += hv_jstat_exact_note(&s, 4ull * (uint64_t)i + 0x1000, 0x80000000ull, 0, 2, 3, &og, &oe) == 1;
    }
    CHECK(found == stored, "after overflow only %d of %d stored keys found", found, stored);
    printf("exact set capped: %d stored, %d not recorded, all stored keys still found\n", stored, over);
    free(s.slot);
    free(pool);
}

/* A cyclic working set W with a cache of C blocks flushed whole when full: the retranslation figure and the epoch gaps. */
static void t_cycle(void) {
    const int W = 300000, C = 100000;
    HvJstatExact s;
    hv_jstat_exact_init(&s, 1000000);
    uint64_t *bm = (uint64_t *)calloc(HV_JSTAT_BITMAP_WORDS, 8);
    char *in = (char *)calloc(W, 1);
    int held = 0;
    uint32_t epoch = 0;
    uint64_t tr = 0, ex_again = 0, bm_again = 0, gap3 = 0, gap_other = 0, bm_false = 0;
    for (int pass = 0; pass < 6; pass++)
        for (int i = 0; i < W; i++) {
            if (in[i]) continue;
            if (held == C) { memset(in, 0, W); held = 0; epoch++; }
            in[i] = 1, held++, tr++;
            uint64_t va = 0xfffffe0010000000ull + 4ull * (uint64_t)i * 5, pa = 0x800000000ull + 4ull * (uint64_t)i * 5;
            uint32_t og = 0, oe = 0;
            int r = hv_jstat_exact_note(&s, va, pa, 0, 1, epoch, &og, &oe);
            int b = hv_jstat_bitmap_test_set(bm, hv_jstat_hash_vapa(va, pa, 0));
            if (r == 1) { ex_again++; if (epoch - oe == 3) gap3++; else gap_other++; }
            bm_again += (uint64_t)b;
            if (r == 0 && b) bm_false++;
        }
    printf("cyclic working set %d blocks, cache %d, flush-all: %llu translations, %llu again (exact), bitmap said %llu; gap 3 epochs %llu, other %llu; bitmap false %llu\n",
           W, C, (unsigned long long)tr, (unsigned long long)ex_again, (unsigned long long)bm_again, (unsigned long long)gap3, (unsigned long long)gap_other, (unsigned long long)bm_false);
    CHECK(gap3 == ex_again && gap_other == 0, "every block comes back 3 flush epochs later (W/C), got %llu / %llu", (unsigned long long)gap3, (unsigned long long)gap_other);
    CHECK(s.count == (uint64_t)W, "distinct %llu", (unsigned long long)s.count);
    CHECK(ex_again == tr - (uint64_t)W, "again %llu", (unsigned long long)ex_again);
    CHECK(bm_again >= ex_again && bm_again - ex_again == bm_false, "bitmap again %llu, exact %llu, false %llu", (unsigned long long)bm_again, (unsigned long long)ex_again, (unsigned long long)bm_false);
    CHECK(bm_false < W / 100, "300k distinct keys: collisions should be ~1.7%%, got %llu of %d", (unsigned long long)bm_false, W);
    for (int g = 0; g < 8; g++) CHECK(hv_jstat_gap_bucket((uint32_t)g) == (g == 0 ? 0 : g == 1 ? 1 : g == 2 ? 2 : g <= 4 ? 3 : 4), "bucket %d", g);
    free(s.slot), free(bm), free(in);
}

int main(void) {
    t_hash_same();
    t_bitmap_fp();
    t_qualify_table();
    t_exact();
    t_cycle();
    printf("%s test_jitstats: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
