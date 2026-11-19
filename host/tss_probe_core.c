#define _GNU_SOURCE 1
#include "tss_probe_core.h"
#include <plist/plist.h>
#include <stdlib.h>
#include <string.h>
#include "libtatsu/tss.h"
#include "remote_zip.h"
#include <libimobiledevice-glue/sha.h>

#ifndef HV_TSS_HTTPS_ONLY
#error "tss_probe_core.c needs libtatsu built with HV_TSS_HTTPS_ONLY (one attempt, https only, scrubbed diagnosis)"
#endif
void tss_set_max_attempts(int n);
void tss_set_timeouts(long connect_s, long total_s);
const char *tss_last_diag(void);

#define P "tss probe: "

/* Apple's well-known "empty local policy" file (22 bytes), the same constant idevicerestore.c calls lpol_file/lpol_file_length. That file is vendored
   third_party code and is not linked into this binary (see tools/vmrun-tss-build.sh), so this is a local copy of the same public, fixed bytes, not an
   extern reference into the vendored tree. */
static const uint8_t HvLpolFile[22] = {
    0x30, 0x14, 0x16, 0x04, 0x49, 0x4d, 0x34, 0x50,
    0x16, 0x04, 0x6c, 0x70, 0x6f, 0x6c, 0x16, 0x03,
    0x31, 0x2e, 0x30, 0x04, 0x01, 0x00
};
static const uint32_t HvLpolFileLen = 22;

static unsigned long long num(plist_t n) {
    unsigned long long v = 0;
    if (!n) return ~0ULL;
    if (plist_get_node_type(n) == PLIST_INT) { uint64_t u = 0; plist_get_uint_val(n, &u); return u; }
    if (plist_get_node_type(n) == PLIST_STRING) { char *s = NULL; plist_get_string_val(n, &s); if (s) v = strtoull(s, 0, 0); free(s); return v; }
    return ~0ULL;
}
static int has(plist_t d, const char *k, plist_type t) { plist_t n = plist_dict_get_item(d, k); return n && plist_get_node_type(n) == t; }

/* Steps 1-2 shared by every stage of every prober in this file: read BuildManifest.plist out of the IPSW by Range requests (approved limits, strong ETag),
   then find the one build identity whose chip/board/variant all match. Behavior and printed text are unchanged from before this function was split out. */
static int fetch_identity(const HvTssProbeCfg *c, FILE *out, unsigned long long chip, unsigned long long board, const char *variant, plist_t *out_root, plist_t *out_ident) {
    *out_root = NULL; *out_ident = NULL;
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
    *out_root = root; *out_ident = ident;
    return 0;
}

int hv_tss_probe_core(const HvTssProbeCfg *c, FILE *out) {
    unsigned long long chip = c->chip ? c->chip : 0xFE00, board = c->board ? c->board : 0x20;
    const char *variant = c->variant ? c->variant : "Customer Erase Install (IPSW)";
    if (!c->ap_nonce || !c->ap_len || !c->sep_nonce || !c->sep_len || !c->ecid) { fprintf(out, P "no ECID/nonce to use: nothing sent\n"); return 2; }

    /* 1-2. the manifest and the matching build identity */
    plist_t root = NULL, ident = NULL;
    int frc = fetch_identity(c, out, chip, board, variant, &root, &ident);
    if (frc) return frc;

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
        /* The diagnosis is a fixed vocabulary (attempts, HTTP code, STATUS number, a MESSAGE_CLASS chosen by this tool); no server text is in it. */
        const char *d = tss_last_diag();
        const char *cls = strstr(d, "TRANSPORT:") ? "transport (DNS/TLS/connection/timeout): says nothing about signing"
                        : strstr(d, "TSS_STATUS:") ? "the server answered and refused or rejected this request (not a verdict beyond this one request; MESSAGE_CLASS is this tool's guess)"
                        : strstr(d, "NOT_A_TSS_REPLY") ? "unexpected reply (not a TSS answer)"
                        : strstr(d, "SUCCESS_BUT_NOT_A_PLIST") ? "server said SUCCESS but the reply was not a plist" : "no usable reply";
        fprintf(out, P "CLASS: %s\n" P "%s\n", cls, d);
    }
    fprintf(out, P "done. Nothing was stored. Local policy and recovery-OS requests, and everything after them, were not tried.\n");
    plist_free(req); plist_free(p); plist_free(root);
    return rc;
}

/* Builds and sends one stage's request (p/req already built by the caller; build_bad is non-zero if the caller's own builder calls already failed).
   Frees p and req (and resp, if any) before returning. Prints a "stage <name>: ..." line for every step, same scrubbed-diagnosis vocabulary as the
   AP-only prober above. use_ap_ticket_check selects how a non-NULL response is judged:
     1 - the AP (main) stage: libtatsu exposes tss_response_get_ap_img4_ticket() for exactly this response shape, so a ticket must actually be present.
     0 - the LocalPolicy / RecoveryOS-root-ticket stages: libtatsu has no equivalent extractor for these response shapes (confirmed: tss.h declares
         ticket extractors only for the AP, baseband and generic-blob cases). idevicerestore.c's own get_local_policy_tss_response() and
         get_recoveryos_root_ticket_tss_response() do not validate ticket presence either - they treat tss_request_send() returning a non-NULL plist
         as the pass signal (tss_request_send() already classifies a server-side rejection as a NULL return with tss_last_diag() explaining why, which
         is the same assumption the AP-stage code above already relies on). This function mirrors that reference behavior for these two stages rather
         than inventing a field-presence check this codebase cannot confirm.
   On a ticket-bearing success (use_ap_ticket_check=1 and a ticket is found), ticket_out and ticket_len_out receive the ticket's raw bytes (caller must
   free() it); otherwise they are left at NULL/0. */
static int send_stage(const HvTssProbeCfg *c, FILE *out, const char *stage, plist_t p, plist_t req, int build_bad, int use_ap_ticket_check,
                       unsigned char **ticket_out, unsigned int *ticket_len_out) {
    *ticket_out = NULL; *ticket_len_out = 0;
    if (build_bad || !req) {
        fprintf(out, P "stage %s: the request could not be completed from this identity: nothing sent\n", stage);
        if (req) plist_free(req);
        plist_free(p);
        return 2;
    }
    fprintf(out, P "stage %s: request built by libtatsu: %u top-level fields (values not printed)\n", stage, plist_dict_get_size(req));
    if (!c->send) {
        fprintf(out, P "stage %s: send disabled: nothing sent\n", stage);
        plist_free(req); plist_free(p);
        return 1;
    }
    tss_set_max_attempts(1);
    tss_set_timeouts(c->tss_connect_s, c->tss_total_s);
    tss_set_debug_level(0);
    fprintf(out, P "stage %s: SENDING the request to %s, one attempt, no retry\n", stage,
            c->tss_url ? "the configured test server" : "Apple's TSS server (gs.apple.com, https, certificate checked)");
    fflush(out);
    plist_t resp = tss_request_send(req, c->tss_url);
    plist_free(req);
    plist_free(p);
    int rc = 1;
    if (resp) {
        if (use_ap_ticket_check) {
            unsigned char *t = NULL; unsigned int tl = 0;
            if (tss_response_get_ap_img4_ticket(resp, &t, &tl) == 0 && t && tl) {
                fprintf(out, P "stage %s: CLASS: ticket issued (ApImg4Ticket %u bytes; not stored, not printed)\n", stage, tl);
                *ticket_out = t; *ticket_len_out = tl;
                rc = 0;
            } else {
                fprintf(out, P "stage %s: CLASS: reply was a plist without an ApImg4Ticket\n", stage);
                free(t);
            }
        } else {
            /* No ticket-field extractor exists in libtatsu for this stage's response shape (see function doc comment); a non-NULL plist here is the
               same "the server accepted and answered" signal idevicerestore.c itself relies on for these two stages. */
            fprintf(out, P "stage %s: CLASS: TSS accepted the request (plist response received, %u top-level field(s); not stored, not printed; no field-level ticket check is available for this stage in this build)\n",
                    stage, plist_dict_get_size(resp));
            rc = 0;
        }
        plist_free(resp);
    } else {
        const char *d = tss_last_diag();
        const char *cls = strstr(d, "TRANSPORT:") ? "transport (DNS/TLS/connection/timeout): says nothing about signing"
                        : strstr(d, "TSS_STATUS:") ? "the server answered and refused or rejected this request (not a verdict beyond this one request; MESSAGE_CLASS is this tool's guess)"
                        : strstr(d, "NOT_A_TSS_REPLY") ? "unexpected reply (not a TSS answer)"
                        : strstr(d, "SUCCESS_BUT_NOT_A_PLIST") ? "server said SUCCESS but the reply was not a plist" : "no usable reply";
        fprintf(out, P "stage %s: CLASS: %s\n" P "%s\n", stage, cls, d);
    }
    return rc;
}

/* AP -> LocalPolicy -> RecoveryOS-root-ticket, stopping at the first stage that is not OK. No stage is retried. Mirrors, field by field, idevicerestore.c's
   get_tss_response() (AP), get_local_policy_tss_response() and get_recoveryos_root_ticket_tss_response() - using libtatsu's own request builders directly,
   the same way hv_tss_probe_core() already does for the AP-only case, rather than linking idevicerestore.c (which is not part of this binary; see
   tools/vmrun-tss-build.sh). The LocalPolicy stage needs the AP stage's own raw ticket bytes (Ap,NextStageIM4MHash = sha384(ap ticket)) and Apple's fixed,
   publicly-documented 22-byte "empty local policy" file (Ap,LocalPolicy.Digest = sha384(that file); a local copy, HvLpolFile above, since the vendored
   idevicerestore.c that defines the same constant is not linked into this binary).
   Returns 0 if all three stages were sent and answered, 1 if sending stopped because a stage was answered but not accepted (or disabled), 2 if nothing was
   sent (manifest/identity/request-build failure before any network attempt). Prints a "stage <name>: ..." account for each stage attempted; later stages
   are never reached once an earlier one does not return 0. */
int hv_tss_probe_core_chain(const HvTssProbeCfg *c, FILE *out) {
    unsigned long long chip = c->chip ? c->chip : 0xFE00, board = c->board ? c->board : 0x20;
    const char *variant = c->variant ? c->variant : "Customer Erase Install (IPSW)";
    if (!c->ap_nonce || !c->ap_len || !c->sep_nonce || !c->sep_len || !c->ecid) { fprintf(out, P "no ECID/nonce to use: nothing sent\n"); return 2; }

    plist_t root = NULL, ident = NULL;
    int frc = fetch_identity(c, out, chip, board, variant, &root, &ident);
    if (frc) return frc;

    /* Stage 1: AP (main) ticket - identical construction to hv_tss_probe_core() above */
    plist_t p1 = plist_new_dict();
    plist_dict_set_item(p1, "ApECID", plist_new_uint(c->ecid));
    plist_dict_set_item(p1, "ApNonce", plist_new_data((const char *)c->ap_nonce, (uint64_t)c->ap_len));
    plist_dict_set_item(p1, "ApSepNonce", plist_new_data((const char *)c->sep_nonce, (uint64_t)c->sep_len));
    plist_dict_set_item(p1, "ApProductionMode", plist_new_bool(1));
    plist_dict_set_item(p1, "ApSecurityMode", plist_new_bool(1));
    plist_dict_set_item(p1, "ApSupportsImg4", plist_new_bool(1));
    int bad1 = tss_parameters_add_from_manifest(p1, ident, true) < 0;
    plist_t req1 = tss_request_new(NULL);
    if (!req1 || tss_request_add_common_tags(req1, p1, NULL) < 0 || tss_request_add_ap_tags(req1, p1, NULL) < 0 || tss_request_add_ap_img4_tags(req1, p1) < 0) bad1 = 1;
    unsigned char *ap_ticket = NULL; unsigned int ap_ticket_len = 0;
    int rc1 = send_stage(c, out, "AP", p1, req1, bad1, 1, &ap_ticket, &ap_ticket_len);
    if (rc1 != 0) {
        fprintf(out, P "stopped after stage AP: local policy and recovery-OS root ticket requests were not tried\n");
        fprintf(out, P "done. Nothing was stored.\n");
        plist_free(root);
        return rc1;
    }

    /* Stage 2: LocalPolicy - mirrors get_local_policy_tss_response() in idevicerestore.c */
    plist_t p2 = plist_new_dict();
    plist_dict_set_item(p2, "ApECID", plist_new_uint(c->ecid));
    plist_dict_set_item(p2, "Ap,LocalBoot", plist_new_bool(0));
    plist_dict_set_item(p2, "ApNonce", plist_new_data((const char *)c->ap_nonce, (uint64_t)c->ap_len));
    plist_dict_set_item(p2, "ApSepNonce", plist_new_data((const char *)c->sep_nonce, (uint64_t)c->sep_len));
    plist_dict_set_item(p2, "ApProductionMode", plist_new_bool(1));
    plist_dict_set_item(p2, "ApSecurityMode", plist_new_bool(1));
    plist_dict_set_item(p2, "ApSupportsImg4", plist_new_bool(1));
    int bad2 = tss_parameters_add_from_manifest(p2, ident, true) < 0;
    uint8_t lpol_digest[SHA384_DIGEST_LENGTH];
    sha384(HvLpolFile, HvLpolFileLen, lpol_digest);
    plist_t lpol = plist_new_dict();
    plist_dict_set_item(lpol, "Digest", plist_new_data((const char *)lpol_digest, SHA384_DIGEST_LENGTH));
    plist_dict_set_item(lpol, "Trusted", plist_new_bool(1));
    plist_dict_set_item(p2, "Ap,LocalPolicy", lpol);
    uint8_t ns_hash[SHA384_DIGEST_LENGTH];
    sha384(ap_ticket, ap_ticket_len, ns_hash);
    plist_dict_set_item(p2, "Ap,NextStageIM4MHash", plist_new_data((const char *)ns_hash, SHA384_DIGEST_LENGTH));
    free(ap_ticket); ap_ticket = NULL;
    plist_t req2 = tss_request_new(NULL);
    if (!req2 || tss_request_add_local_policy_tags(req2, p2) < 0) bad2 = 1;
    unsigned char *lpol_ticket = NULL; unsigned int lpol_ticket_len = 0;
    int rc2 = send_stage(c, out, "LocalPolicy", p2, req2, bad2, 0, &lpol_ticket, &lpol_ticket_len);
    free(lpol_ticket);
    if (rc2 != 0) {
        fprintf(out, P "stopped after stage LocalPolicy: the recovery-OS root ticket request was not tried\n");
        fprintf(out, P "done. Nothing was stored.\n");
        plist_free(root);
        return rc2;
    }

    /* Stage 3: RecoveryOS root ticket - mirrors get_recoveryos_root_ticket_tss_response() in idevicerestore.c */
    plist_t p3 = plist_new_dict();
    plist_dict_set_item(p3, "ApECID", plist_new_uint(c->ecid));
    plist_dict_set_item(p3, "Ap,LocalBoot", plist_new_bool(0));
    plist_dict_set_item(p3, "ApNonce", plist_new_data((const char *)c->ap_nonce, (uint64_t)c->ap_len));
    plist_dict_set_item(p3, "ApSepNonce", plist_new_data((const char *)c->sep_nonce, (uint64_t)c->sep_len));
    plist_dict_set_item(p3, "ApProductionMode", plist_new_bool(1));
    plist_dict_set_item(p3, "ApSecurityMode", plist_new_bool(1));
    plist_dict_set_item(p3, "ApSupportsImg4", plist_new_bool(1));
    int bad3 = tss_parameters_add_from_manifest(p3, ident, true) < 0;
    plist_t req3 = tss_request_new(NULL);
    if (!req3 || tss_request_add_ap_img4_tags(req3, p3) < 0 || tss_request_add_common_tags(req3, p3, NULL) < 0 || tss_request_add_ap_recovery_tags(req3, p3, NULL) < 0) bad3 = 1;
    unsigned char *root_ticket = NULL; unsigned int root_ticket_len = 0;
    int rc3 = send_stage(c, out, "RecoveryOS", p3, req3, bad3, 0, &root_ticket, &root_ticket_len);
    free(root_ticket);
    plist_free(root);
    if (rc3 != 0) {
        fprintf(out, P "stopped after stage RecoveryOS: nothing after this was tried\n");
        fprintf(out, P "done. Nothing was stored.\n");
        return rc3;
    }

    fprintf(out, P "all three stages (AP, LocalPolicy, RecoveryOS root ticket) were sent and answered. Nothing was stored. Firmware transfer and everything after it was not tried.\n");
    return 0;
}
