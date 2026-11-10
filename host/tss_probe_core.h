/* TSS probe core: reads the build identity for the virtual Mac out of the IPSW's BuildManifest.plist with Range requests (host/remote_zip.c), builds the main
   TSS request with libtatsu's own builders, and sends it ONCE. Android's https-only libtatsu (HV_TSS_HTTPS_ONLY) is required.
   Prints a scrubbed account to `out`: never the ECID, the nonces, the request, or any part of a ticket. Returns 0 = a ticket was issued, 1 = the request was
   sent (or the transport failed) and no ticket came back, 2 = nothing was sent (no manifest, no matching identity, request incomplete). */
#ifndef HV_TSS_PROBE_CORE_H
#define HV_TSS_PROBE_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {
    const char *ipsw_url;           /* https; http only for loopback in a test build */
    const char *tss_url;            /* NULL: Apple's https list (3 hosts, one attempt in total) */
    uint64_t ecid;
    const uint8_t *ap_nonce; size_t ap_len;
    const uint8_t *sep_nonce; size_t sep_len;
    long tss_connect_s, tss_total_s;/* 0: 20 / 60 */
    int send;                       /* 0: build and check the request, send nothing */
    uint64_t chip, board;           /* 0: 0xFE00 / 0x20 */
    const char *variant;            /* NULL: "Customer Erase Install (IPSW)" */
} HvTssProbeCfg;
int hv_tss_probe_core(const HvTssProbeCfg *cfg, FILE *out);
#endif
