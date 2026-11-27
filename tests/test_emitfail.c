/* Host test (any CPU) of the emit-failure policy in core/emitfail.h (g7-jit3).
   The real lookup() and run_loop() need the arm64 JIT and cannot run here, so this file models their control flow
   around the very hv_emitfail_note / hv_emitfail_clear the dispatcher calls: lookup() asks emit, then place; an
   emit failure asks note(); FLUSH sets flush_pending and returns NULL, GIVE_UP returns NULL with nothing pending;
   place failure (region full) sets flush_pending; run_loop flushes when pending and retries, and reports
   "cannot translate" (park, HV_EXIT_UNSUPPORTED) when lookup returned NULL with nothing pending and no flush
   happened. The seams (emit / place function pointers) play the part of hv_test_emit_hook / hv_test_place_hook.
   Also shows the pre-g7-jit3 behaviour (limit -1) running into the bound of the harness: a flush loop.
   integ7b: lookup() now reports WHY it returned NULL (HV_LK_*, core/emitfail.h) and run_loop decides with hv_lookup_null_action(); the model does the same, and
   t_concurrent_flush shows the case the old inference got wrong: another CPU requests a flush at the moment this CPU gives up on a deterministic failure. The old
   rule ("a flush is pending: retry") then retried (and with a busy neighbour, every time); the explicit outcome stops at once. The engine-fatal record (first writer
   wins, readers never see a half-written one) is tested with threads. */
#include "../core/emitfail.h"

#include <pthread.h>
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
    int outcome;                 /* HV_LK_* of the last lookup (what lookup() reports in HvLookupInfo) */
    int legacy;                  /* 1: decide as run_loop did before integ7b (infer from flush_pending / flush count) */
    int other_flush_on_fatal;    /* another CPU requests a flush right after this lookup returns (adversarial: every time) */
    int other_pending_entry;     /* another CPU's request is pending when lookup starts, this many times */
    uint64_t waited;             /* lookups that returned WAIT_FLUSH */
} Model;

enum { M_OK, M_FATAL, M_LOOP };

static int lookup(Model *m, const Key *k) { /* 1: block made, 0: NULL (m->outcome says why) */
    m->outcome = HV_LK_OK;
    if (m->other_pending_entry > 0) { /* the "flush already pending" exit at the top of lookup(): no emit */
        m->other_pending_entry--;
        m->flush_pending = 1;
        m->outcome = HV_LK_WAIT_FLUSH, m->waited++;
        return 0;
    }
    m->emit_calls++;
    if (m->emit(k, m->ctx)) {
        m->emit_fail_deterministic++;
        if (hv_emitfail_note(&m->efs, k->va, k->pa, k->flags, k->gen, m->max_flushes) == HV_EMITFAIL_GIVE_UP) {
            m->emit_fail_retry_exhausted++;
            m->outcome = HV_LK_FATAL_EMIT;
            if (m->other_flush_on_fatal) m->flush_pending = 1; /* not ours: another CPU's region filled up just now */
            return 0;
        }
        m->flush_emit_fail++;
        m->flush_pending = 1;
        m->outcome = HV_LK_FLUSH_RETRY;
        return 0;
    }
    hv_emitfail_clear(&m->efs);
    if (m->place(k, m->ctx) || m->used >= m->capacity) {
        m->emit_fail_region++;
        m->flush_pending = 1;
        m->outcome = HV_LK_FLUSH_RETRY;
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
        int pending = m->flush_pending, flushed = m->block_flushes != fl;
        if (m->legacy ? (pending || flushed) : hv_lookup_null_action(m->outcome, pending, flushed) == HV_LKA_RETRY) continue;
        return M_FATAL; /* engine fatal: the whole VM is stopped (core/dispatch.c engine_fatal_stop) */
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

static void t_concurrent_flush(void) {
    uint64_t bad = 0x6000;
    Key k = {0x6000, 0x80006000, 0, 0};
    /* adversarial neighbour: asks for a flush each time this CPU gives up. Old inference: retries forever (bounded here). New: fatal after limit+1 emits. */
    Model o; model_init(&o, 10, 1); o.emit = bad_va, o.ctx = &bad, o.legacy = 1, o.other_flush_on_fatal = 1;
    CHECK(run_block(&o, &k, 1000) == M_LOOP, "legacy inference with a busy neighbour should loop (it flushed for the neighbour and tried again)");
    Model n; model_init(&n, 10, 1); n.emit = bad_va, n.ctx = &bad, n.other_flush_on_fatal = 1;
    CHECK(run_block(&n, &k, 1000) == M_FATAL, "explicit outcome: fatal even with another CPU's flush pending");
    CHECK(n.outcome == HV_LK_FATAL_EMIT && n.emit_calls == 2 && n.block_flushes == 1 && n.emit_fail_retry_exhausted == 1, "fatal after exactly 1 flush and 2 emits (calls %llu flushes %llu)", (unsigned long long)n.emit_calls, (unsigned long long)n.block_flushes);
    printf("  concurrent flush: legacy inference LOOP (%llu flushes in the bound), explicit outcome FATAL after %llu emits\n", (unsigned long long)o.block_flushes, (unsigned long long)n.emit_calls);
    /* the same neighbour with a block that is fine: nothing fatal, the flush is just taken */
    Model g; model_init(&g, 10, 1); g.other_pending_entry = 3;
    Key ok = {0x7000, 0x80007000, 0, 0};
    CHECK(run_block(&g, &ok, 100) == M_OK && g.waited == 3 && g.block_flushes == 3 && g.emit_calls == 1, "WAIT_FLUSH outcomes retry and then succeed (waited %llu flushes %llu emits %llu)", (unsigned long long)g.waited, (unsigned long long)g.block_flushes, (unsigned long long)g.emit_calls);
    /* the decision table itself */
    CHECK(hv_lookup_null_action(HV_LK_FATAL_EMIT, 1, 1) == HV_LKA_FATAL && hv_lookup_null_action(HV_LK_FATAL_EMIT, 0, 0) == HV_LKA_FATAL, "FATAL_EMIT is fatal whatever is pending");
    CHECK(hv_lookup_null_action(HV_LK_FLUSH_RETRY, 1, 0) == HV_LKA_RETRY && hv_lookup_null_action(HV_LK_FLUSH_RETRY, 0, 1) == HV_LKA_RETRY && hv_lookup_null_action(HV_LK_WAIT_FLUSH, 1, 0) == HV_LKA_RETRY, "flush outcomes retry while a flush is pending or happened");
    CHECK(hv_lookup_null_action(HV_LK_FLUSH_RETRY, 0, 0) == HV_LKA_FATAL && hv_lookup_null_action(HV_LK_WAIT_FLUSH, 0, 0) == HV_LKA_FATAL, "a flush outcome with nothing pending and nothing flushed is unexplained: fatal, as before");
    CHECK(hv_lookup_null_action(HV_LK_OK, 1, 1) == HV_LKA_FATAL && hv_lookup_null_action(99, 1, 1) == HV_LKA_FATAL, "NULL with outcome OK or unknown never retries blindly");
}

#define NT 8
static HvEngineFatal g_ef;
static volatile int g_go, g_wins[NT];
static void *rec_thread(void *a) {
    int i = (int)(intptr_t)a;
    while (!g_go) {}
    if (hv_engfatal_record(&g_ef, (uint32_t)i, 0x1000ull * (i + 1), 0x2000ull * (i + 1), (uint32_t)i + 10, (uint32_t)i + 20, -3, (uint32_t)i + 30)) g_wins[i] = 1;
    return NULL;
}
static volatile int g_stop_reader, g_reader_bad;
static void *read_thread(void *unused) {
    (void)unused;
    HvEngineFatal c;
    while (!g_stop_reader)
        if (hv_engfatal_get(&g_ef, &c)) { /* any published copy is one writer's, complete */
            int i = (int)c.cpu;
            if (i < 0 || i >= NT || c.va != 0x1000ull * (i + 1) || c.pa != 0x2000ull * (i + 1) || c.flags != (uint32_t)i + 10 || c.gen != (uint32_t)i + 20 || c.rc != -3 || c.flushes != (uint32_t)i + 30) g_reader_bad = 1;
        }
    return NULL;
}
static void t_engfatal_record(void) {
    HvEngineFatal z, out; memset(&z, 0, sizeof z);
    CHECK(!hv_engfatal_get(&z, &out), "no record: nothing to read");
    CHECK(hv_engfatal_record(&z, 2, 0xfe00, 0x8000, 1, 5, -2, 1) == 1 && hv_engfatal_get(&z, &out) && out.cpu == 2 && out.va == 0xfe00 && out.rc == -2, "first record wins and reads back");
    CHECK(hv_engfatal_record(&z, 3, 1, 1, 1, 1, -3, 1) == 0 && hv_engfatal_get(&z, &out) && out.cpu == 2, "second record is refused and does not overwrite");
    for (int round = 0; round < 200; round++) {
        memset(&g_ef, 0, sizeof g_ef); memset((void *)g_wins, 0, sizeof g_wins); g_go = 0; g_stop_reader = 0;
        pthread_t t[NT], r;
        for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, rec_thread, (void *)(intptr_t)i);
        pthread_create(&r, NULL, read_thread, NULL);
        g_go = 1;
        for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
        g_stop_reader = 1; pthread_join(r, NULL);
        int wins = 0, w = -1; for (int i = 0; i < NT; i++) if (g_wins[i]) wins++, w = i;
        HvEngineFatal f;
        if (wins != 1 || !hv_engfatal_get(&g_ef, &f) || (int)f.cpu != w || g_reader_bad) { CHECK(0, "round %d: wins %d, record cpu %u, reader saw a torn record %d", round, wins, g_ef.cpu, g_reader_bad); break; }
    }
    CHECK(1, "engine-fatal record: 200 rounds of %d racing writers and a reader", NT);
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
    t_concurrent_flush();
    t_engfatal_record();
    printf("test_emitfail: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
