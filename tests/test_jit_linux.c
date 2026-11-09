/* Linux and Android: the split mapping (memfd, RW and RX views) must run code,
   and a word rewritten through the RW view must change what the RX address
   executes. Only meaningful on an arm64 host, since it runs arm64 code. */
#include "jit_region.h"

#include <stdio.h>

typedef long (*jit_fn)(void);

static const uint32_t MOV_X0_42 = 0xd2800540; /* mov x0, #42 */
static const uint32_t MOV_X0_7 = 0xd28000e0;  /* mov x0, #7 */
static const uint32_t RET = 0xd65f03c0;       /* ret */

int main(void) {
    HvJitRegion region;
    if (hv_jit_region_open(&region, 1 << 20) != 0) {
        printf("linux-split: could not map the JIT region\n");
        return 1;
    }
    if (!region.rw_diff) {
        printf("linux-split: region has no RW alias, so this is not the split shape\n");
        hv_jit_region_close(&region);
        return 1;
    }

    int bad = 0;

    /* 1. Write a block through the alias, make it runnable, run it. */
    uint32_t *code = hv_jit_reserve(&region, 2);
    uint32_t body[2] = {MOV_X0_42, RET};
    hv_jit_write(&region, code, body, sizeof body);
    hv_jit_finish(&region, code, 2);
    long got = ((jit_fn)(void *)code)();
    printf("linux-split: first run returned %ld (expect 42)\n", got);
    bad |= got != 42;

    /* 2. Rewrite one word while the block exists, then run it again. The word is
          written through the RW alias with the 4-byte path, then invalidated. */
    hv_jit_write(&region, code, &MOV_X0_7, sizeof MOV_X0_7);
    hv_jit_finish(&region, code, 1);
    got = ((jit_fn)(void *)code)();
    printf("linux-split: after rewrite returned %ld (expect 7)\n", got);
    bad |= got != 7;

    hv_jit_region_close(&region);
    printf("linux-split: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}
