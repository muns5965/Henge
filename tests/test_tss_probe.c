/* Driver for host/tss_probe_core.c against local mock servers. usage: test_tss_probe IPSW_URL TSS_URL [--variant V] [--nosend] [--nononce] [--timeout N]
   TSS_URL is required: this driver never uses Apple's default servers. Fixed fake ECID/nonces; the Python test checks that none of them is ever printed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tss_probe_core.h"
int main(int argc, char **argv) {
    if (argc < 3) return 64;
    static uint8_t ap[32], sep[20];
    memset(ap, 0x5c, sizeof ap); memset(sep, 0x6d, sizeof sep);
    HvTssProbeCfg c; memset(&c, 0, sizeof c);
    c.ipsw_url = argv[1]; c.tss_url = argv[2]; c.ecid = 0x2d2a8d94f32ba4d2ULL;
    c.ap_nonce = ap; c.ap_len = sizeof ap; c.sep_nonce = sep; c.sep_len = sizeof sep; c.send = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--variant") && i + 1 < argc) c.variant = argv[++i];
        else if (!strcmp(argv[i], "--nosend")) c.send = 0;
        else if (!strcmp(argv[i], "--nononce")) c.ap_nonce = NULL;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) { c.tss_connect_s = 2; c.tss_total_s = atol(argv[++i]); }
    }
    int rc = hv_tss_probe_core(&c, stdout);
    printf("EXIT %d\n", rc);
    return rc;
}
