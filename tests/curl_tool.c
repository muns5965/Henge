/* Test driver for host/curl_mbedtls.c: curl_tool URL [--post DATA] [--follow] [--headers] [--abort-after N] [--discard] [--also URL]... [--ca]
   Prints the body on stdout; a summary line "RC=<code> HTTP=<status> BYTES=<n> ERR=<message>" on stderr. */
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "curl/curl.h"
int hv_curl_ca_count(const char **where);
void hv_curl_deny_network(int on);
int hv_curl_attempt_count(void);
int hv_curl_denied_count(void);
int hv_curl_ca_unparsed(void);
int hv_curl_tls_completed(void);
const char *hv_curl_tls_diag(void);
static size_t bytes, abort_after = 0;
static long timeout_ms, connect_timeout_ms, cancel_after_ms, global_cancel_after_ms, abort_after_ms;
static volatile int g_flag;
static CURL *g_handle;
static double t_start;
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static int count_fds(void) { int n = 0; DIR *d = opendir("/proc/self/fd"); if (!d) return -1; while (readdir(d)) n++; closedir(d); return n - 3; } /* ".", ".." and the DIR itself */
static void *canceller(void *arg) { long ms = (long)arg; usleep((useconds_t)ms * 1000); hv_curl_cancel(g_handle); return NULL; }
static void *flagger(void *arg) { long ms = (long)arg; usleep((useconds_t)ms * 1000); g_flag = 1; return NULL; }
static int xfer(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) { (void)u, (void)a, (void)b, (void)c, (void)d; return (now_s() - t_start) * 1000 >= abort_after_ms; }
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
    if (!strcmp(argv[1], "--ca")) { const char *w = ""; int n = hv_curl_ca_count(&w); fprintf(stderr, "CA=%d FROM=%s UNPARSED_FILES=%d\n", n, w, hv_curl_ca_unparsed()); return n ? 0 : 1; }
    char err[CURL_ERROR_SIZE] = "";
    int follow = 0, nalso = 0;
    const char *also[16];
    const char *post = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--post") && i + 1 < argc) post = argv[++i];
        else if (!strcmp(argv[i], "--follow")) follow = 1;
        else if (!strcmp(argv[i], "--headers")) show_headers = 1;
        else if (!strcmp(argv[i], "--discard")) discard = 1;
        else if (!strcmp(argv[i], "--deny")) hv_curl_deny_network(1);
        else if (!strcmp(argv[i], "--also") && i + 1 < argc && nalso < 16) also[nalso++] = argv[++i];
        else if (!strcmp(argv[i], "--abort-after") && i + 1 < argc) abort_after = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) timeout_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--connect-timeout-ms") && i + 1 < argc) connect_timeout_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cancel-after-ms") && i + 1 < argc) cancel_after_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--global-cancel-after-ms") && i + 1 < argc) global_cancel_after_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--abort-after-ms") && i + 1 < argc) abort_after_ms = atol(argv[++i]);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *c = curl_easy_init();
    struct curl_slist *h = curl_slist_append(NULL, "X-Henge-Test: yes");
    h = curl_slist_append(h, "Expect:"); /* libtatsu does this; with libcurl it removes the header */
    curl_easy_setopt(c, CURLOPT_URL, argv[1]);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hcb);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); /* the callers do this; the shim must still verify */
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, (long)follow);
    if (post) { curl_easy_setopt(c, CURLOPT_POSTFIELDS, post); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(post)); }
    if (timeout_ms) curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    if (connect_timeout_ms) curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    if (abort_after_ms) { curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer); curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L); }
    g_handle = c;
    hv_curl_set_cancel_flag(&g_flag);
    pthread_t th;
    if (cancel_after_ms) pthread_create(&th, NULL, canceller, (void *)cancel_after_ms);
    if (global_cancel_after_ms) pthread_create(&th, NULL, flagger, (void *)global_cancel_after_ms);
    int fds0 = count_fds();
    t_start = now_s();
    CURLcode rc = curl_easy_perform(c);
    double el = (now_s() - t_start) * 1000;
    fprintf(stderr, "ELAPSED_MS=%.0f FDS_BEFORE=%d FDS_AFTER=%d\n", el, fds0, count_fds());
    hv_curl_cancel_reset(c);
    g_flag = 0;
    cancel_after_ms = global_cancel_after_ms = abort_after_ms = timeout_ms = 0;
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 0L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    fprintf(stderr, "TLS_VERIFIED=%d\n", hv_curl_tls_completed());
    if (hv_curl_tls_diag()[0]) fprintf(stderr, "TLS_DIAG:\n%s", hv_curl_tls_diag());
    fprintf(stderr, "REQ 1 DIAG_BYTES=%zu\n", strlen(hv_curl_tls_diag()));
    fprintf(stderr, "ATTEMPTS=%d DENIED=%d\n", hv_curl_attempt_count(), hv_curl_denied_count());
    fprintf(stderr, "RC=%d HTTP=%ld BYTES=%zu ERR=%s\n", (int)rc, code, bytes, err);
    /* more requests on the same handle: shows that per-request state (diagnostics, response code) does not leak between them */
    for (int i = 0; i < nalso; i++) {
        bytes = 0; err[0] = 0;
        curl_easy_setopt(c, CURLOPT_URL, also[i]);
        rc = curl_easy_perform(c);
        code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        fprintf(stderr, "REQ %d RC=%d HTTP=%ld TLS_VERIFIED=%d DIAG_BYTES=%zu ERR=%s\n", i + 2, (int)rc, code, hv_curl_tls_completed(), strlen(hv_curl_tls_diag()), err);
    }
    fprintf(stderr, "FINAL ATTEMPTS=%d DENIED=%d\n", hv_curl_attempt_count(), hv_curl_denied_count());
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    return (int)rc;
}
