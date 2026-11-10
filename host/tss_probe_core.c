#define _GNU_SOURCE 1
#include "tss_probe_core.h"
#include <plist/plist.h>
#include <stdlib.h>
#include <string.h>
#include "libtatsu/tss.h"
#include "remote_zip.h"

#ifndef HV_TSS_HTTPS_ONLY
#error "tss_probe_core.c needs libtatsu built with HV_TSS_HTTPS_ONLY (one attempt, https only, scrubbed diagnosis)"
#endif
void tss_set_max_attempts(int n);
void tss_set_timeouts(long connect_s, long total_s);
const char *tss_last_diag(void);

#define P "tss probe: "
static unsigned long long num(plist_t n) {
    unsigned long long v = 0;
    if (!n) return ~0ULL;
    if (plist_get_node_type(n) == PLIST_INT) { uint64_t u = 0; plist_get_uint_val(n, &u); return u; }
    if (plist_get_node_type(n) == PLIST_STRING) { char *s = NULL; plist_get_string_val(n, &s); if (s) v = strtoull(s, 0, 0); free(s); return v; }
    return ~0ULL;
}
static int has(plist_t d, const char *k, plist_type t) { plist_t n = plist_dict_get_item(d, k); return n && plist_get_node_type(n) == t; }

int hv_tss_probe_core(const HvTssProbeCfg *c, FILE *out) {
    unsigned long long chip = c->chip ? c->chip : 0xFE00, board = c->board ? c->board : 0x20;
    const char *variant = c->variant ? c->variant : "Customer Erase Install (IPSW)";
    if (!c->ap_nonce || !c->ap_len || !c->sep_nonce || !c->sep_len || !c->ecid) { fprintf(out, P "no ECID/nonce to use: nothing sent\n"); return 2; }

    /* 1. the manifest, by Range requests: the approved limits (24 requests, 4 MiB received), strong ETag, in memory only */
    RzLimits L; rz_limits_default(&L);
    L.max_requests = 24; L.max_bytes = 4u << 20; L.max_entry_bytes = 48u << 20; L.require_strong_etag = 1; L.trace = out;
    char err[300] = "";
    RemoteZip *z = rz_open(c->ipsw_url, &L, err, sizeof err);
    if (!z) { fprintf(out, P "BuildManifest not read (%s): nothing sent\n", err); return 2; }
    uint8_t *mf = NULL; size_t mflen = 0;
    if (rz_read_entry(z, "BuildManifest.plist", &mf, &mflen, err, sizeof err)) { fprintf(out, P "BuildManifest not read (%s): nothing sent\n", err); rz_close(z); return 2; }
    const RzStats *s = rz_stats(z);
    fprintf(out, P "manifest read: %zu bytes inflated, %u requests, %llu bytes received\n", mflen, s->requests, (unsigned long long)s->bytes);
    rz_close(z);
    plist_t root = NULL;
    plist_from_memory((const char *)mf, (uint32_t)mflen, &root, NULL);
    free(mf);
    plist_t ids = root ? plist_dict_get_item(root, "BuildIdentities") : NULL;
    if (!ids || plist_get_node_type(ids) != PLIST_ARRAY) { fprintf(out, P "manifest has no BuildIdentities: nothing sent\n"); plist_free(root); return 2; }

    /* 2. the identity: chip, board and variant must all match; exactly which one is printed */
    plist_t ident = NULL; uint32_t at = 0, matches = 0;
    for (uint32_t i = 0; i < plist_array_get_size(ids); i++) {
        plist_t id = plist_array_get_item(ids, i);
        plist_t info = plist_dict_get_item(id, "Info");
        plist_t v = info ? plist_dict_get_item(info, "Variant") : NULL;
        char *vs = NULL;
        if (v && plist_get_node_type(v) == PLIST_STRING) plist_get_string_val(v, &vs);
        int ok = num(plist_dict_get_item(id, "ApChipID")) == chip && num(plist_dict_get_item(id, "ApBoardID")) == board && vs && !strcmp(vs, variant);
        free(vs);
        if (ok) { if (!ident) ident = id, at = i; matches++; }
    }
    if (!ident) { fprintf(out, P "no build identity with chip 0x%llx, board 0x%llx and variant \"%s\": nothing sent\n", chip, board, variant); plist_free(root); return 2; }
    fprintf(out, P "identity %u of %u (chip 0x%llx, board 0x%llx, \"%s\")%s\n", at, plist_array_get_size(ids), chip, board, variant, matches > 1 ? " [first of several matches]" : "");

    /* 3. the request, with libtatsu's builders; the parameters mirror get_tss_response() in idevicerestore.c */
    plist_t p = plist_new_dict();
    plist_dict_set_item(p, "ApECID", plist_new_uint(c->ecid));
    plist_dict_set_item(p, "ApNonce", plist_new_data((const char *)c->ap_nonce, (uint64_t)c->ap_len));
    plist_dict_set_item(p, "ApSepNonce", plist_new_data((const char *)c->sep_nonce, (uint64_t)c->sep_len));
    plist_dict_set_item(p, "ApProductionMode", plist_new_bool(1));
    plist_dict_set_item(p, "ApSecurityMode", plist_new_bool(1));
    plist_dict_set_item(p, "ApSupportsImg4", plist_new_bool(1));
    plist_t req = NULL;
    int bad = 0;
    if (tss_parameters_add_from_manifest(p, ident, true) < 0) bad++;
    req = tss_request_new(NULL);
    if (!req || tss_request_add_common_tags(req, p, NULL) < 0 || tss_request_add_ap_tags(req, p, NULL) < 0 || tss_request_add_ap_img4_tags(req, p) < 0) bad++;
    if (!bad) {
        static const struct { const char *k; plist_type t; } need[] = {
            {"@HostPlatformInfo", PLIST_STRING}, {"@VersionInfo", PLIST_STRING}, {"@UUID", PLIST_STRING}, {"@ApImg4Ticket", PLIST_BOOLEAN}, {"ApECID", PLIST_INT},
            {"ApChipID", PLIST_INT}, {"ApBoardID", PLIST_INT}, {"ApSecurityDomain", PLIST_INT}, {"UniqueBuildID", PLIST_DATA}, {"ApNonce", PLIST_DATA},
            {"SepNonce", PLIST_DATA}, {"ApProductionMode", PLIST_BOOLEAN}, {"ApSecurityMode", PLIST_BOOLEAN}};
        for (size_t i = 0; i < sizeof need / sizeof *need; i++) if (!has(req, need[i].k, need[i].t)) { fprintf(out, P "request lacks %s\n", need[i].k); bad++; }
    }
    if (bad) { fprintf(out, P "the request could not be completed from this identity: nothing sent\n"); if (req) plist_free(req); plist_free(p); plist_free(root); return 2; }
    fprintf(out, P "request built by libtatsu: %u top-level fields (values not printed)\n", plist_dict_get_size(req));
    if (!c->send) { fprintf(out, P "send disabled: nothing sent\n"); plist_free(req); plist_free(p); plist_free(root); return 1; }

    /* 4. ONE attempt */
    tss_set_max_attempts(1);
    tss_set_timeouts(c->tss_connect_s, c->tss_total_s);
    tss_set_debug_level(0);
    fprintf(out, P "SENDING the request to %s, one attempt, no retry\n", c->tss_url ? "the configured test server" : "Apple's TSS server (gs.apple.com, https, certificate checked)");
    fflush(out);
    plist_t resp = tss_request_send(req, c->tss_url);
    int rc = 1;
    if (resp) {
        unsigned char *t = NULL; unsigned int tl = 0;
        if (tss_response_get_ap_img4_ticket(resp, &t, &tl) == 0 && t && tl) fprintf(out, P "CLASS: ticket issued (ApImg4Ticket %u bytes; not stored, not printed). This is the answer to the main request only.\n", tl), rc = 0;
        else fprintf(out, P "CLASS: reply was a plist without an ApImg4Ticket\n");
        free(t);
        plist_free(resp);
    } else {
        const char *d = tss_last_diag();
        const char *cls = strstr(d, "transport error") ? "transport (DNS/TLS/connection/timeout): says nothing about signing"
                        : strstr(d, "STATUS=") ? "the server answered and refused or rejected this request (see STATUS/MESSAGE; not a verdict beyond this one request)"
                        : strstr(d, "without STATUS") ? "unexpected reply (not a TSS answer)" : "no usable reply";
        fprintf(out, P "CLASS: %s\n" P "detail: %s\n", cls, d);
    }
    fprintf(out, P "done. Nothing was stored. Local policy and recovery-OS requests, and everything after them, were not tried.\n");
    plist_free(req); plist_free(p); plist_free(root);
    return rc;
}
