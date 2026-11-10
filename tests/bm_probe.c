/* One-purpose program for the phone: reads BuildManifest.plist out of the macOS 15.6.1 (24G90) VirtualMac2,1 IPSW with HTTP Range requests (host/remote_zip.c)
   and says which build identities match the virtual machine's chip/board (ApChipID 0xFE00, ApBoardID 0x20).
   Limits are fixed: at most 24 requests, 4 MiB of body, a strong ETag is required, BuildManifest.plist only, in memory. No TSS request, no full download,
   nothing written to disk. The URL is not printed. usage: bm_probe [--file BuildManifest.plist] [URL]   (--file: only the chip/board scan, local, for tests) */
#include <plist/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remote_zip.h"

#define DEFAULT_URL "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw"
#define VM_CHIP 0xFE00ULL
#define VM_BOARD 0x20ULL

static unsigned long long num(plist_t n) {
    unsigned long long v = 0;
    if (!n) return ~0ULL;
    if (plist_get_node_type(n) == PLIST_INT) { uint64_t u = 0; plist_get_uint_val(n, &u); return u; }
    if (plist_get_node_type(n) == PLIST_STRING) { char *s = NULL; plist_get_string_val(n, &s); if (s) v = strtoull(s, 0, 0); free(s); return v; }
    return ~0ULL;
}

static int scan(const uint8_t *buf, size_t len) {
    plist_t root = NULL;
    plist_from_memory((const char *)buf, (uint32_t)len, &root, NULL);
    plist_t ids = root ? plist_dict_get_item(root, "BuildIdentities") : NULL;
    if (!ids || plist_get_node_type(ids) != PLIST_ARRAY) { printf("MANIFEST: no BuildIdentities array\n"); return 1; }
    uint32_t n = plist_array_get_size(ids), both = 0, chip = 0;
    printf("MANIFEST: %u build identities\n", n);
    for (uint32_t i = 0; i < n; i++) {
        plist_t id = plist_array_get_item(ids, i);
        unsigned long long c = num(plist_dict_get_item(id, "ApChipID")), b = num(plist_dict_get_item(id, "ApBoardID"));
        plist_t info = plist_dict_get_item(id, "Info");
        char *var = NULL, *dev = NULL;
        plist_t v = info ? plist_dict_get_item(info, "Variant") : NULL, d = info ? plist_dict_get_item(info, "DeviceClass") : NULL;
        if (v && plist_get_node_type(v) == PLIST_STRING) plist_get_string_val(v, &var);
        if (d && plist_get_node_type(d) == PLIST_STRING) plist_get_string_val(d, &dev);
        int cm = c == VM_CHIP, bm = b == VM_BOARD;
        if (cm) chip++;
        if (cm && bm) both++;
        printf("  identity %u: chip 0x%llx %s, board 0x%llx %s, class %s, variant %s\n", i, c, cm ? "MATCH" : "differs", b, bm ? "MATCH" : "differs", dev ? dev : "?", var ? var : "?");
        free(var); free(dev);
    }
    printf("MANIFEST: %u identities with chip 0x%llx, %u with chip and board 0x%llx. This is the BuildManifest only: it does not show that Apple signs this build for this machine.\n", chip, VM_CHIP, both, VM_BOARD);
    plist_free(root);
    return both ? 0 : 2;
}

int main(int argc, char **argv) {
    const char *url = DEFAULT_URL, *file = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i]; else url = argv[i]; }
    if (file) {
        FILE *f = fopen(file, "rb"); if (!f) { printf("cannot open %s\n", file); return 3; }
        static uint8_t b[8 << 20]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
        return scan(b, n);
    }
    RzLimits L; rz_limits_default(&L);
    L.max_requests = 24; L.max_bytes = 4u << 20; L.require_strong_etag = 1; L.trace = stdout;
    printf("PLAN: Range requests only, at most %u requests and %llu bytes, strong ETag required, BuildManifest.plist only, nothing stored, no TSS\n", L.max_requests, (unsigned long long)L.max_bytes);
    char err[300] = "";
    RemoteZip *z = rz_open(url, &L, err, sizeof err);
    const RzStats *s = z ? rz_stats(z) : NULL;
    if (!z) { printf("OPEN FAILED: %s\n", err); printf("(a refusal for a missing strong ETag or an unexpected reply is a result of this test, not a verdict on the server)\n"); return 1; }
    printf("OPEN: ok, archive %llu bytes, strong ETag %s, consistency %s\n", (unsigned long long)s->size, s->strong_etag ? "yes" : "no", s->consistency_weak ? "weak" : "strong");
    uint8_t *d = NULL; size_t n = 0;
    if (rz_read_entry(z, "BuildManifest.plist", &d, &n, err, sizeof err)) { printf("ENTRY FAILED: %s\nSTATS requests=%u bytes=%llu wire=%llu\n", err, s->requests, (unsigned long long)s->bytes, (unsigned long long)s->wire_bytes); rz_close(z); return 1; }
    printf("ENTRY: BuildManifest.plist %zu bytes (uncompressed)\nSTATS requests=%u bytes=%llu wire=%llu\n", n, s->requests, (unsigned long long)s->bytes, (unsigned long long)s->wire_bytes);
    int rc = scan(d, n);
    free(d); rz_close(z);
    return rc;
}
