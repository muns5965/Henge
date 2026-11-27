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

#endif
