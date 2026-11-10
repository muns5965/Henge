/* Driver for host/remote_zip.c. usage: remote_zip_tool URL [--entry NAME --out FILE] [--raw OFF LEN] [--block N] [--max-requests N] [--max-bytes N]
   [--max-entry N] [--max-entries N] [--require-strong] [--cancel-after-ms N] [--trace]
   Prints OPEN/ENTRY/RAW lines, STATS and "RESULT: ok" or "RESULT: fail: <reason>" on stdout. Never prints entry content (--out writes it). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "remote_zip.h"

static volatile int g_cancel;
static void *canceller(void *a) { usleep((useconds_t)(long)a * 1000); g_cancel = 1; return NULL; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    RzLimits L; rz_limits_default(&L); L.cancel = &g_cancel;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--trace")) L.trace = stdout;
    const char *entry = NULL, *out = NULL; int raw = 0; unsigned long long roff = 0, rlen = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 2 < argc) { raw = 1; roff = strtoull(argv[++i], 0, 0); rlen = strtoull(argv[++i], 0, 0); }
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) L.block_size = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--max-requests") && i + 1 < argc) L.max_requests = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--max-bytes") && i + 1 < argc) L.max_bytes = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--max-entry") && i + 1 < argc) L.max_entry_bytes = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--max-entries") && i + 1 < argc) L.max_entries = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--require-strong")) L.require_strong_etag = 1;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) L.request_timeout_s = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cancel-after-ms") && i + 1 < argc) { pthread_t t; pthread_create(&t, 0, canceller, (void *)atol(argv[++i])); pthread_detach(t); }
    }
    char err[300] = "";
    RemoteZip *z = rz_open(argv[1], &L, err, sizeof err);
    if (!z) { printf("RESULT: fail: %s\n", err); return 1; }
    const RzStats *s = rz_stats(z);
    int rc = 0;
    if (raw) {
        char *b = malloc(rlen ? rlen : 1);
        if (rz_read_at(z, roff, b, rlen, err, sizeof err)) { printf("RESULT: fail: %s\n", err); rc = 1; } else printf("RAW %llu bytes\n", rlen);
        free(b);
    } else if (entry) {
        uint8_t *d; size_t n;
        if (rz_read_entry(z, entry, &d, &n, err, sizeof err)) { printf("RESULT: fail: %s\n", err); rc = 1; }
        else {
            printf("ENTRY %s %zu bytes\n", entry, n);
            if (out) { FILE *f = fopen(out, "wb"); fwrite(d, 1, n, f); fclose(f); }
            free(d);
        }
    }
    printf("STATS requests=%u bytes=%llu wire=%llu size=%llu strong_etag=%d consistency=%s broken=%d\n", s->requests, (unsigned long long)s->bytes,
           (unsigned long long)s->wire_bytes, (unsigned long long)s->size, s->strong_etag, s->consistency_weak ? "weak(size+LastModified-only)" : "strong-etag", s->broken);
    if (!rc) printf("RESULT: ok\n");
    rz_close(z);
    return rc;
}
