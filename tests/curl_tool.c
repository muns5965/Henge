/* Test driver for host/curl_mbedtls.c: curl_tool URL [--post DATA] [--follow] [--headers] [--abort-after N] [--discard] [--ca]
   Prints the body on stdout; a summary line "RC=<code> HTTP=<status> BYTES=<n> ERR=<message>" on stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "curl/curl.h"
int hv_curl_ca_count(const char **where);
static size_t bytes, abort_after = 0;
static int show_headers, discard;
static size_t wcb(char *p, size_t s, size_t n, void *u) {
    (void)u;
    bytes += s * n;
    if (abort_after && bytes >= abort_after) return 0;
    if (!discard) fwrite(p, s, n, stdout);
    return s * n;
}
static size_t hcb(char *p, size_t s, size_t n, void *u) { (void)u; if (show_headers) fprintf(stderr, "H: %.*s", (int)(s * n), p); return s * n; }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "--ca")) { const char *w = ""; int n = hv_curl_ca_count(&w); fprintf(stderr, "CA=%d FROM=%s\n", n, w); return n ? 0 : 1; }
    char err[CURL_ERROR_SIZE] = "";
    int follow = 0;
    const char *post = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--post") && i + 1 < argc) post = argv[++i];
        else if (!strcmp(argv[i], "--follow")) follow = 1;
        else if (!strcmp(argv[i], "--headers")) show_headers = 1;
        else if (!strcmp(argv[i], "--discard")) discard = 1;
        else if (!strcmp(argv[i], "--abort-after") && i + 1 < argc) abort_after = (size_t)atol(argv[++i]);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *c = curl_easy_init();
    struct curl_slist *h = curl_slist_append(NULL, "X-Henge-Test: yes");
    curl_easy_setopt(c, CURLOPT_URL, argv[1]);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hcb);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); /* the callers do this; the shim must still verify */
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, (long)follow);
    if (post) { curl_easy_setopt(c, CURLOPT_POSTFIELDS, post); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(post)); }
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    fprintf(stderr, "RC=%d HTTP=%ld BYTES=%zu ERR=%s\n", (int)rc, code, bytes, err);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    return (int)rc;
}
