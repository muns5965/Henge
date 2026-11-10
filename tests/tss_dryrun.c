/* TSS dry run: builds a TSS request with libtatsu's own request builders and describes its STRUCTURE. Nothing is sent: this program is linked with
   tss.c compiled -DHV_TSS_NO_SEND (no tss_request_send, no curl, no TLS), which tools/tss-dryrun-test.sh verifies on the binary.
   What it can and cannot say:
   - The builders (tss_parameters_add_from_manifest, tss_request_new, tss_request_add_common_tags/ap_tags/ap_img4_tags) are libtatsu's, the same code
     idevicerestore calls. The ~15 lines that fill `parameters` before them (ECID, nonces, production/security mode) mirror get_tss_response() in
     idevicerestore.c; they are repeated here, not shared (tools/tss-dryrun-test.sh checks that the same keys appear in that function).
   - With no argument the build identity is SYNTHETIC (invented digests, not from any IPSW): the result shows that the builders produce the expected
     fields and formats. It says nothing about whether Apple would sign any real build for this machine.
   - With `BuildManifest.plist INDEX` the identity is taken from that file (local file only) and the chip/board numbers in it are compared with the
     virtual machine's (ApChipID 0xFE00, ApBoardID 0x20). That is the first point at which compatibility can be judged, and still only
     chip/board and the request shape: not signing status.
   Output: one line per field: path, type, length. No value of any kind is printed (not the ECID, not the nonces, not strings).
   Exit 0 = the request has every required field with the right type; 1 = something missing; 2 = usage/file error. */
#include <plist/plist.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtatsu/tss.h"

#define VM_CHIP 0xFE00u
#define VM_BOARD 0x20u

static void describe(const char *path, plist_t n, int depth) {
    char buf[256];
    switch (plist_get_node_type(n)) {
    case PLIST_DICT: {
        printf("%-44s dict[%u]\n", path, (unsigned)plist_dict_get_size(n));
        if (depth >= 2) return;
        plist_dict_iter it = NULL; plist_dict_new_iter(n, &it);
        for (;;) {
            char *k = NULL; plist_t c = NULL;
            plist_dict_next_item(n, it, &k, &c);
            if (!k) break;
            snprintf(buf, sizeof buf, "%s%s%s", path, *path ? "." : "", k);
            describe(buf, c, depth + 1);
            free(k);
        }
        free(it);
        break;
    }
    case PLIST_DATA: { uint64_t l = 0; char *d = NULL; plist_get_data_val(n, &d, &l); free(d); printf("%-44s data[%llu]\n", path, (unsigned long long)l); break; }
    case PLIST_STRING: { char *s = NULL; plist_get_string_val(n, &s); printf("%-44s string[%zu]\n", path, s ? strlen(s) : 0); free(s); break; }
    case PLIST_BOOLEAN: printf("%-44s bool\n", path); break;
    case PLIST_INT: printf("%-44s int\n", path); break;
    case PLIST_ARRAY: printf("%-44s array[%u]\n", path, (unsigned)plist_array_get_size(n)); break;
    default: printf("%-44s other\n", path);
    }
}

static plist_t fake_data(uint8_t fill, size_t n) { char b[64]; memset(b, fill, n); return plist_new_data(b, n); }

static plist_t synthetic_identity(void) {
    plist_t id = plist_new_dict();
    plist_dict_set_item(id, "UniqueBuildID", fake_data(0x11, 20));
    plist_dict_set_item(id, "ApChipID", plist_new_uint(VM_CHIP));
    plist_dict_set_item(id, "ApBoardID", plist_new_uint(VM_BOARD));
    plist_dict_set_item(id, "ApSecurityDomain", plist_new_uint(1));
    plist_t info = plist_new_dict();
    plist_dict_set_item(info, "Variant", plist_new_string("synthetic"));
    plist_dict_set_item(id, "Info", info);
    plist_t man = plist_new_dict();
    const char *names[] = {"LLB", "iBoot", "KernelCache"};
    for (int i = 0; i < 3; i++) {
        plist_t e = plist_new_dict();
        plist_dict_set_item(e, "Digest", fake_data((uint8_t)(0x20 + i), 48));
        plist_dict_set_item(e, "Trusted", plist_new_bool(1));
        plist_t ei = plist_new_dict();
        plist_dict_set_item(ei, "Path", plist_new_string("synthetic/path"));
        plist_dict_set_item(e, "Info", ei);
        plist_dict_set_item(man, names[i], e);
    }
    plist_dict_set_item(id, "Manifest", man);
    return id;
}

static int need(plist_t req, const char *key, plist_type t, int *bad) {
    plist_t n = plist_dict_get_item(req, key);
    if (!n || plist_get_node_type(n) != t) { printf("MISSING-OR-WRONG-TYPE %s\n", key); (*bad)++; return 0; }
    return 1;
}

int main(int argc, char **argv) {
    plist_t id = NULL;
    const char *omit = NULL;
    int a = 1;
    if (a < argc && !strcmp(argv[a], "--omit") && a + 1 < argc) { omit = argv[a + 1]; a += 2; }   /* test hook: leave one parameter out */
    if (a < argc) {
        const char *path = argv[a++];
        int idx = a < argc ? atoi(argv[a]) : 0;
        char *buf = NULL; size_t cap = 0, len = 0; FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
        for (;;) { if (len + 65536 > cap) { cap = cap ? cap * 2 : 1 << 20; buf = realloc(buf, cap); } size_t r = fread(buf + len, 1, 65536, f); if (!r) break; len += r; }
        fclose(f);
        plist_t root = NULL; plist_from_memory(buf, (uint32_t)len, &root, NULL); free(buf);
        plist_t ids = root ? plist_dict_get_item(root, "BuildIdentities") : NULL;
        if (!ids || plist_get_node_type(ids) != PLIST_ARRAY || (uint32_t)idx >= plist_array_get_size(ids)) { fprintf(stderr, "no BuildIdentities[%d] in %s\n", idx, path); return 2; }
        id = plist_copy(plist_array_get_item(ids, (uint32_t)idx));
        plist_free(root);
        printf("source: BuildManifest file, identity %d (the file's values are used; none is printed)\n", idx);
        uint64_t chip = 0, board = 0;
        plist_t c = plist_dict_get_item(id, "ApChipID"), b = plist_dict_get_item(id, "ApBoardID");
        /* BuildManifest stores these as hex strings ("0xfe00") */
        char *cs = NULL, *bs = NULL;
        if (c && plist_get_node_type(c) == PLIST_STRING) { plist_get_string_val(c, &cs); chip = strtoull(cs, NULL, 0); }
        else if (c && plist_get_node_type(c) == PLIST_INT) plist_get_uint_val(c, &chip);
        if (b && plist_get_node_type(b) == PLIST_STRING) { plist_get_string_val(b, &bs); board = strtoull(bs, NULL, 0); }
        else if (b && plist_get_node_type(b) == PLIST_INT) plist_get_uint_val(b, &board);
        free(cs); free(bs);
        printf("chip/board vs the virtual machine (0x%X / 0x%X): chip %s, board %s\n", VM_CHIP, VM_BOARD, chip == VM_CHIP ? "match" : "DIFFERENT", board == VM_BOARD ? "match" : "DIFFERENT");
    } else {
        id = synthetic_identity();
        printf("source: SYNTHETIC build identity (invented; validates the request builders only, says nothing about any real build)\n");
    }

    /* the parameters, as get_tss_response() in idevicerestore.c fills them (mirrored) */
    plist_t p = plist_new_dict();
    if (!omit || strcmp(omit, "ApECID")) plist_dict_set_item(p, "ApECID", plist_new_uint(0x1122334455667788ULL));
    if (!omit || strcmp(omit, "ApNonce")) plist_dict_set_item(p, "ApNonce", fake_data(0x33, 32));
    if (!omit || strcmp(omit, "ApSepNonce")) plist_dict_set_item(p, "ApSepNonce", fake_data(0x44, 20));
    plist_dict_set_item(p, "ApProductionMode", plist_new_bool(1));
    plist_dict_set_item(p, "ApSecurityMode", plist_new_bool(1));
    plist_dict_set_item(p, "ApSupportsImg4", plist_new_bool(1));
    int rc = 0;
    if (tss_parameters_add_from_manifest(p, id, true) < 0) { printf("tss_parameters_add_from_manifest failed\n"); rc = 1; }
    plist_t req = tss_request_new(NULL);
    if (!req || tss_request_add_common_tags(req, p, NULL) < 0 || tss_request_add_ap_tags(req, p, NULL) < 0 || tss_request_add_ap_img4_tags(req, p) < 0) {
        printf("a request builder failed\n"); rc = 1;
    }
    if (req) {
        describe("", req, 0);
        int bad = 0;
        need(req, "@HostPlatformInfo", PLIST_STRING, &bad); need(req, "@VersionInfo", PLIST_STRING, &bad); need(req, "@UUID", PLIST_STRING, &bad);
        need(req, "@ApImg4Ticket", PLIST_BOOLEAN, &bad);
        need(req, "ApECID", PLIST_INT, &bad); need(req, "ApChipID", PLIST_INT, &bad); need(req, "ApBoardID", PLIST_INT, &bad);
        need(req, "ApSecurityDomain", PLIST_INT, &bad); need(req, "UniqueBuildID", PLIST_DATA, &bad);
        need(req, "ApNonce", PLIST_DATA, &bad); need(req, "SepNonce", PLIST_DATA, &bad);
        need(req, "ApProductionMode", PLIST_BOOLEAN, &bad); need(req, "ApSecurityMode", PLIST_BOOLEAN, &bad);
        plist_t n = plist_dict_get_item(req, "ApNonce"); uint64_t l = 0; char *d = NULL;
        if (n && plist_get_node_type(n) == PLIST_DATA) { plist_get_data_val(n, &d, &l); free(d); if (l != 32) { printf("ApNonce length is not 32\n"); bad++; } }
        n = plist_dict_get_item(req, "SepNonce");
        if (n && plist_get_node_type(n) == PLIST_DATA) { plist_get_data_val(n, &d, &l); free(d); if (l != 20) { printf("SepNonce length is not 20\n"); bad++; } }
        printf(bad ? "STRUCTURE: %d required field(s) missing or malformed\n" : "STRUCTURE: all required fields present with the right types\n", bad);
        if (bad) rc = 1;
        plist_free(req);
    }
    printf("NOT CHECKED: whether Apple signs any build for this machine; nothing was sent.\n");
    plist_free(p); plist_free(id);
    return rc;
}
