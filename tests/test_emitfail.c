/* Host test (any CPU) of the emit-failure policy in core/emitfail.h (g7-jit3).
   The real lookup() and run_loop() need the arm64 JIT and cannot run here, so this file models their control flow
   around the very hv_emitfail_note / hv_emitfail_clear the dispatcher calls: lookup() asks emit, then place; an
   emit failure asks note(); FLUSH sets flush_pending and returns NULL, GIVE_UP returns NULL with nothing pending;
   place failure (region full) sets flush_pending; run_loop flushes when pending and retries, and reports
   "cannot translate" (park, HV_EXIT_UNSUPPORTED) when lookup returned NULL with nothing pending and no flush
   happened. The seams (emit / place function pointers) play the part of hv_test_emit_hook / hv_test_place_hook.
   Also shows the pre-g7-jit3 behaviour (limit -1) running into the bound of the harness: a flush loop. */
#include "../core/emitfail.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint64_t va, pa; uint32_t flags, gen; } Key;

typedef struct {
    int (*emit)(const Key *k, void *ctx);  /* nonzero: emit failed */
    int (*place)(const Key *k, void *ctx); /* nonzero: region full */
    void *ctx;
    int flush_pending;
    unsigned used, capacity;               /* region, in blocks */
    uint64_t block_flushes, translations, emit_calls;
    uint64_t emit_fail_region, emit_fail_deterministic, emit_fail_retry_exhausted, flush_emit_fail;
    int max_flushes;
    HvEmitFailState efs;
} Model;

enum { M_OK, M_FATAL, M_LOOP };

static int lookup(Model *m, const Key *k) { /* 1: block made, 0: NULL */
    m->emit_calls++;
    if (m->emit(k, m->ctx)) {
        m->emit_fail_deterministic++;
        if (hv_emitfail_note(&m->efs, k->va, k->pa, k->flags, k->gen, m->max_flushes) == HV_EMITFAIL_GIVE_UP) {
            m->emit_fail_retry_exhausted++;
            return 0;
        }
        m->flush_emit_fail++;
        m->flush_pending = 1;
        return 0;
    }
    hv_emitfail_clear(&m->efs);
    if (m->place(k, m->ctx) || m->used >= m->capacity) {
        m->emit_fail_region++;
        m->flush_pending = 1;
        return 0;
    }
    m->used++;
    m->translations++;
    return 1;
}

/* run_loop around one lookup; `bound` iterations are the harness's way of seeing a loop. */
static int run_block(Model *m, const Key *k, int bound) {
    for (int it = 0; it < bound; it++) {
        if (m->flush_pending) {
            m->flush_pending = 0;
            m->used = 0;
            m->block_flushes++;
            continue;
        }
        uint64_t fl = m->block_flushes;
        if (lookup(m, k)) return M_OK;
        if (m->flush_pending || m->block_flushes != fl) continue;
        return M_FATAL; /* "cannot translate block", park, HV_EXIT_UNSUPPORTED */
    }
    return M_LOOP;
}

static int never(const Key *k, void *c) { (void)k; (void)c; return 0; }
static int bad_va(const Key *k, void *c) { return k->va == *(uint64_t *)c; }
static int fail_n_times(const Key *k, void *c) { (void)k; int *n = c; return (*n)-- > 0; }

static void model_init(Model *m, unsigned cap, int max) {
    memset(m, 0, sizeof *m);
    m->capacity = cap, m->max_flushes = max;
    m->emit = never, m->place = never;
}

static void t_region_full(void) {
    Model m; model_init(&m, 10, HV_EMIT_FAIL_MAX_FLUSHES);
    int bad = 0;
    for (uint64_t i = 0; i < 100; i++) {
        Key k = {0x1000 + i * 4, 0x80000000 + i * 4, 0, 0};
        if (run_block(&m, &k, 100) != M_OK) bad++;
    }
    CHECK(!bad, "region-full blocks did not all succeed");
    CHECK(m.block_flushes == 9, "flushes %llu, want 9 (100 blocks, 10 per region)", (unsigned long long)m.block_flushes);
    CHECK(m.emit_fail_region == 9 && m.emit_fail_deterministic == 0 && m.emit_fail_retry_exhausted == 0, "counters region %llu det %llu exh %llu",
          (unsigned long long)m.emit_fail_region, (unsigned long long)m.emit_fail_deterministic, (unsigned long long)m.emit_fail_retry_exhausted);
    /* identical to the old policy: same flushes and translations with the limit off */
    Model o; model_init(&o, 10, -1);
    for (uint64_t i = 0; i < 100; i++) { Key k = {0x1000 + i * 4, 0x80000000 + i * 4, 0, 0}; run_block(&o, &k, 100); }
    CHECK(o.block_flushes == m.block_flushes && o.translations == m.translations && o.emit_calls == m.emit_calls, "region-full behaviour differs from the old policy");
}

static void t_deterministic(int limit) {
    Model m; model_init(&m, 10, limit);
    uint64_t bad = 0x2000;
    m.emit = bad_va, m.ctx = &bad;
    Key k = {0x2000, 0x80002000, 1, 7};
    int r = run_block(&m, &k, 100000);
    if (limit < 0) {
        /* before g7-jit3: never ends; only the harness's bound stops it */
        CHECK(r == M_LOOP, "old policy did not loop (r=%d)", r);
        CHECK(m.block_flushes >= 49000, "old policy flushed only %llu times", (unsigned long long)m.block_flushes);
        printf("  old policy (limit -1): outcome LOOP, %llu whole flushes in the bound, %llu emit attempts\n", (unsigned long long)m.block_flushes, (unsigned long long)m.emit_calls);
    } else {
        CHECK(r == M_FATAL, "limit %d: outcome %d, want FATAL", limit, r);
        CHECK(m.block_flushes == (uint64_t)limit, "limit %d: %llu flushes", limit, (unsigned long long)m.block_flushes);
        CHECK(m.emit_calls == (uint64_t)limit + 1, "limit %d: %llu emit attempts", limit, (unsigned long long)m.emit_calls);
        CHECK(m.emit_fail_retry_exhausted == 1 && m.emit_fail_deterministic == (uint64_t)limit + 1 && m.flush_emit_fail == (uint64_t)limit, "limit %d counters det %llu exh %llu", limit,
              (unsigned long long)m.emit_fail_deterministic, (unsigned long long)m.emit_fail_retry_exhausted);
        printf("  new policy (limit %d): outcome FATAL after %llu flush(es), %llu emit attempts\n", limit, (unsigned long long)m.block_flushes, (unsigned long long)m.emit_calls);
    }
}

static void t_transient(void) {
    Model m; model_init(&m, 10, 1);
    int n = 1;
    m.emit = fail_n_times, m.ctx = &n;
    Key k = {0x3000, 0x80003000, 0, 0};
    CHECK(run_block(&m, &k, 100) == M_OK, "a failure that goes away on retry did not succeed");
    CHECK(m.block_flushes == 1 && m.emit_fail_retry_exhausted == 0 && !m.efs.valid, "transient: flushes %llu, state not cleared", (unsigned long long)m.block_flushes);
    /* and it gets a fresh allowance later */
    n = 1;
    Key k2 = {0x3000, 0x80003000, 0, 1}; /* same block, page changed */
    CHECK(run_block(&m, &k2, 100) == M_OK && m.block_flushes == 2, "fresh allowance for a new generation");
}

static void t_key_change(void) {
    HvEmitFailState s; memset(&s, 0, sizeof s);
    CHECK(hv_emitfail_note(&s, 1, 2, 3, 4, 1) == HV_EMITFAIL_FLUSH, "first failure flushes");
    CHECK(hv_emitfail_note(&s, 1, 2, 3, 5, 1) == HV_EMITFAIL_FLUSH, "other gen: new key");
    CHECK(hv_emitfail_note(&s, 1, 2, 3, 5, 1) == HV_EMITFAIL_GIVE_UP, "same key again: give up");
    CHECK(hv_emitfail_note(&s, 1, 2, 3, 5, 1) == HV_EMITFAIL_GIVE_UP, "and stays given up");
    CHECK(hv_emitfail_note(&s, 9, 2, 3, 5, 1) == HV_EMITFAIL_FLUSH, "other va: new key");
    hv_emitfail_clear(&s);
    CHECK(hv_emitfail_note(&s, 9, 2, 3, 5, 1) == HV_EMITFAIL_FLUSH, "after a success: allowance again");
}

static void t_mixed(void) { /* a bad block among good ones in a small region: good ones keep working until the bad one is reached */
    Model m; model_init(&m, 4, 1);
    uint64_t bad = 0x5000;
    m.emit = bad_va, m.ctx = &bad;
    int ok = 0;
    for (uint64_t i = 0; i < 20; i++) { Key k = {0x4000 + i * 4, 0x80004000 + i * 4, 0, 0}; ok += run_block(&m, &k, 100) == M_OK; }
    CHECK(ok == 20, "good blocks failed");
    Key k = {0x5000, 0x80005000, 0, 0};
    CHECK(run_block(&m, &k, 100) == M_FATAL, "bad block: not fatal");
    CHECK(m.emit_fail_region >= 4 && m.emit_fail_retry_exhausted == 1, "mixed counters");
}

int main(void) {
    t_region_full();
    t_deterministic(-1);
    t_deterministic(0);
    t_deterministic(1);
    t_deterministic(3);
    t_transient();
    t_key_change();
    t_mixed();
    printf("test_emitfail: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
