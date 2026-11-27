#ifndef HENGEVM_TRANSLATE_H
#define HENGEVM_TRANSLATE_H

#include <stdint.h>

#include "machine.h"

/* What a block passes to hv_helper. Lives in the block's literal pool. */
typedef struct HvHelperInfo {
    uint64_t pc;
    uint32_t insn;
    uint32_t op;
} HvHelperInfo;

enum {
    HV_OP_MEM = 1,     /* a load or store the fast path did not handle: emulate it whole */
    HV_OP_MRS,
    HV_OP_MSR,         /* MSR (register) or MSR (immediate); always leaves the block */
    HV_OP_SYS,         /* DC, IC, TLBI, AT */
    HV_OP_EXCEPTION,   /* SVC, HVC, SMC, BRK, HLT */
    HV_OP_ERET,
    HV_OP_UNDEF,
    HV_OP_FPTRAP,      /* FP/SIMD while CPACR_EL1.FPEN traps it */
    HV_OP_CLREX,
    HV_OP_WFI,
    HV_OP_WFE,         /* waits for the exclusive monitor's word to change (or a request); the block carries on */
};

#define HV_MAX_BLOCK_INSNS 128

/* Translates guest code starting at b->va (b->pa, b->flags filled in) into the
   machine's JIT region. `code` is the host copy of the guest instructions and
   `avail` how many may be read before the end of the guest page.
   Returns -1 if the JIT region is full. */
int hv_translate_block(HvMachine *m, HvBlock *b, const uint32_t *code, uint32_t avail);
/* The same in two steps, so CPUs translate in parallel: emit into this
   thread's buffer (no lock needed), then place it in the JIT region (under the
   translation lock). place returns -1 if the region is full. */
int hv_translate_emit(HvMachine *m, HvBlock *b, const uint32_t *code, uint32_t avail);
/* hv_translate_emit returns 0 or a negative cause. All of the causes are properties of the block (its guest words and
   flags), not of the region: the same block fails the same way again, so a whole flush does not help (g7-jit3). */
enum {
    HV_TR_E_TOO_BIG = -2,  /* the hot or cold part is over the emit buffer (MAX_WORDS) */
    HV_TR_E_OVERFLOW = -3, /* the relocation table (MAX_RELS) overflowed */
    HV_TR_E_GROUP = -4,    /* the grouped-access copies did not fit (the wrapper then tries without groups) */
    HV_TR_E_INJECTED = -5, /* a test hook made it fail (HV_TEST_HOOKS builds only) */
};
const char *hv_translate_err_name(int rc);
#ifdef HV_TEST_HOOKS
/* Test seam, only in builds with -DHV_TEST_HOOKS: when set, called at the start of hv_translate_emit / hv_translate_place;
   a nonzero return makes that call fail (emit: with that cause, place: as a full region). */
extern int (*hv_test_emit_hook)(HvMachine *m, HvBlock *b);
extern int (*hv_test_place_hook)(HvMachine *m, HvBlock *b);
#endif
int hv_translate_place(HvMachine *m, HvBlock *b);
uint32_t hv_translate_last_place_words(void); /* region words the thread's last emitted block takes (diagnostics) */
uint32_t hv_translate_last_words(void);

int hv_branch_retarget(uint32_t *w, int64_t off);

#endif
