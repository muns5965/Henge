#ifndef HV_EMITFAIL_H
#define HV_EMITFAIL_H
/* What the dispatcher does when hv_translate_emit fails (g7-jit3). Header-only and free of HvMachine, so the host
   test (tests/test_emitfail.c, any CPU) builds the very decision code lookup() runs.

   Before: a failed emit set flush_pending and lookup() returned NULL; run_loop saw the flush and tried the same
   block again. An emit failure is a property of the block (its guest words, page generation, flags), not of the
   region, so the retry failed again and the machine flushed every translation forever (no progress, no message).

   Now: every failed emit of one key (va, pa, flags, gen) on a CPU is counted. The first HV_EMIT_FAIL_MAX_FLUSHES
   failures still ask for a whole flush (a failure that was not a property of the block may go away; it costs what
   it always cost). The next one gives up: no flush, lookup() returns NULL with nothing pending, and run_loop's
   existing "cannot translate block" path parks the CPU and returns HV_EXIT_UNSUPPORTED. A different key, or a
   block that emits, starts the count again. Region full (hv_translate_place) is a different event and is not
   touched by this. */
#include <stdint.h>
#include <string.h>

#define HV_EMIT_FAIL_MAX_FLUSHES 1 /* flushes asked for one failing key before giving up */

enum { HV_EMITFAIL_FLUSH = 0, HV_EMITFAIL_GIVE_UP = 1 };

typedef struct {
    uint64_t va, pa;
    uint32_t flags, gen;
    uint32_t flushes; /* flushes asked for this key so far */
    uint32_t valid;
    uint32_t logged;  /* the give-up was reported */
} HvEmitFailState;

/* A failed emit of this key. max_flushes < 0 is the behaviour before g7-jit3 (never give up; tests only). */
static inline int hv_emitfail_note(HvEmitFailState *s, uint64_t va, uint64_t pa, uint32_t flags, uint32_t gen, int max_flushes) {
    if (!s->valid || s->va != va || s->pa != pa || s->flags != flags || s->gen != gen) {
        s->va = va, s->pa = pa, s->flags = flags, s->gen = gen;
        s->flushes = 0, s->logged = 0, s->valid = 1;
    }
    if (max_flushes >= 0 && s->flushes >= (uint32_t)max_flushes) return HV_EMITFAIL_GIVE_UP;
    s->flushes++;
    return HV_EMITFAIL_FLUSH;
}

/* The block emitted: forget the failing key. */
static inline void hv_emitfail_clear(HvEmitFailState *s) {
    if (s->valid) s->valid = 0;
}

/* integ7b (T1): what a lookup() that returns NULL means, said explicitly instead of inferred from m->flush_pending afterwards.
   Before: run_loop treated "NULL and a flush pending (or stop-the-world active, or the flush count moved)" as "retry after the flush", whatever the
   cause. A deterministic emit failure that gave up (nothing pending when lookup returned) could meet a flush another vCPU had just requested
   (its region was full) and take the ordinary flush-and-retry path: it still ended up fatal later, but only when no other vCPU happened to request
   a flush at that moment. And a CPU that did give up ended only its own thread (host/vm.c secondary_thread) while the VM went on.
   Outcomes:
     HV_LK_OK          a block was returned (not a NULL case).
     HV_LK_FLUSH_RETRY this CPU asked for a whole flush (region full, block table full, first emit failure of a key): run_loop retries after it.
     HV_LK_WAIT_FLUSH  a flush was already pending (another CPU's request) when this lookup began or was about to publish: wait for it, retry.
     HV_LK_FATAL_EMIT  deterministic emit failure, the key failed again after HV_EMIT_FAIL_MAX_FLUSHES flush(es): no retry helps, whatever is pending. */
enum { HV_LK_OK = 0, HV_LK_FLUSH_RETRY = 1, HV_LK_WAIT_FLUSH = 2, HV_LK_FATAL_EMIT = 3 };
enum { HV_LKA_RETRY = 0, HV_LKA_FATAL = 1 };

typedef struct {
    int outcome;       /* HV_LK_* */
    int rc;            /* the emit failure (HV_TR_E_*), HV_LK_FATAL_EMIT only */
    uint32_t gen;      /* page generation of the failing key */
    uint32_t flushes;  /* flushes asked for that key before giving up */
} HvLookupInfo;

/* What run_loop does with a NULL from lookup(). pending: flush_pending or stw_active is set; flushed: block_flushes moved during the lookup. */
static inline int hv_lookup_null_action(int outcome, int pending, int flushed) {
    if (outcome == HV_LK_FATAL_EMIT) return HV_LKA_FATAL;            /* a concurrent flush request does not change a property of the block */
    if (outcome == HV_LK_FLUSH_RETRY || outcome == HV_LK_WAIT_FLUSH) /* the flush is pending, or another CPU already did it */
        return (pending || flushed) ? HV_LKA_RETRY : HV_LKA_FATAL;  /* nothing pending, nothing flushed: unexplained, fatal as before */
    return HV_LKA_FATAL;                                              /* no block and no reason, or an unknown value: never retry blindly */
}

/* The first engine-fatal stop of the machine, recorded once. state: 0 none, 1 being written, 2 published. Any CPU may call hv_engfatal_record:
   the first caller wins and gets 1 (it logs and stops the machine); later callers get 0 and the record is not overwritten. */
typedef struct {
    uint32_t state;
    uint32_t cpu, flags, gen, flushes;
    int rc;
    uint64_t va, pa;
} HvEngineFatal;

static inline int hv_engfatal_record(HvEngineFatal *f, uint32_t cpu, uint64_t va, uint64_t pa, uint32_t flags, uint32_t gen, int rc, uint32_t flushes) {
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&f->state, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 0;
    f->cpu = cpu, f->va = va, f->pa = pa, f->flags = flags, f->gen = gen, f->rc = rc, f->flushes = flushes;
    __atomic_store_n(&f->state, 2, __ATOMIC_RELEASE);
    return 1;
}

/* A consistent copy of the published record, or 0 when there is none (yet). */
static inline int hv_engfatal_get(const HvEngineFatal *f, HvEngineFatal *out) {
    if (__atomic_load_n(&f->state, __ATOMIC_ACQUIRE) != 2) return 0;
    *out = *f;
    return 1;
}

#endif
