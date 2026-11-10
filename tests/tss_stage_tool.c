/* Test driver for host/tss_stage.c: the real libtatsu + curl stand-in + connection policy against a fake TSS server on this machine. Linux only.
   usage: tss_stage_tool URL [--allow-port P] [--attempts K] [--n N] [--resolve MODE] [--cancel-after-ms MS] [--direct]
     --allow-port P   hv_net_guard_test_allow_loopback(P): exactly 127.0.0.1:P; without it every connection is refused by the policy
     --resolve MODE   replaces the answer for the name "localhost": mixed_dead = 127.0.0.2 then 127.0.0.1, mixed_ok = same (the caller points both at a live server),
                      private = 10.1.2.3 then 127.0.0.2
     --direct         one curl_easy_perform without libtatsu; prints the curl code and the last-request record
   Output is numbers and fixed words only; the request holds a dummy ECID that must never be printed. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <libimobiledevice-glue/socket.h>
#include <plist/plist.h>

#include "net_guard.h"
#include "tss_stage.h"

int (*hv_urlasset_guard)(const char *url) = NULL;
int (*limd_glue_connect_guard)(const struct sockaddr *, socklen_t) = NULL;
__thread int limd_glue_connect_purpose;
void tss_set_max_attempts(int n);
void tss_set_debug_level(int l);
void hv_curl_set_cancel_flag(volatile int *flag);

static volatile int g_connects;
int __real_connect(int fd, const struct sockaddr *a, socklen_t l);
int __wrap_connect(int fd, const struct sockaddr *a, socklen_t l) { __atomic_add_fetch(&g_connects, 1, __ATOMIC_SEQ_CST); return __real_connect(fd, a, l); }

static const char *g_resolve;
static int g_port_for_resolve;
int __real_getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r);
int __wrap_getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r) {
    if (!g_resolve || !n || strcmp(n, "localhost")) return __real_getaddrinfo(n, s, h, r);
    const char *ips[2] = {"127.0.0.2", "127.0.0.1"};
    if (!strcmp(g_resolve, "private")) ips[0] = "10.1.2.3", ips[1] = "127.0.0.2";
    struct addrinfo *head = NULL, **tail = &head;
    for (int i = 0; i < 2; i++) {
        struct addrinfo *a = calloc(1, sizeof *a);
        struct sockaddr_in *sa = calloc(1, sizeof *sa);
        sa->sin_family = AF_INET;
        sa->sin_port = htons((uint16_t)(s ? atoi(s) : 0));
        inet_pton(AF_INET, ips[i], &sa->sin_addr);
        a->ai_family = AF_INET, a->ai_socktype = SOCK_STREAM, a->ai_protocol = IPPROTO_TCP, a->ai_addrlen = sizeof *sa, a->ai_addr = (struct sockaddr *)sa;
        *tail = a, tail = &a->ai_next;
    }
    *r = head;
    return 0;
}

static volatile int g_flag;
static void *canceller(void *arg) { usleep((useconds_t)(long)arg * 1000); g_flag = 1; return NULL; }
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static size_t sink(char *p, size_t s, size_t n, void *u) { (void)p, (void)u; return s * n; }

static plist_t mkreq(void) {
    plist_t r = plist_new_dict();
    plist_dict_set_item(r, "@ApImg4Ticket", plist_new_bool(1));
    plist_dict_set_item(r, "ApECID", plist_new_uint(0x2d2a8d94f32ba4d2ULL));   /* dummy; must never appear in any output */
    plist_dict_set_item(r, "ApChipID", plist_new_uint(0xFE00));
    return r;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *url = argv[1];
    int allow = 0, attempts = 0, n = 1, direct = 0;
    long cancel_ms = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--allow-port") && i + 1 < argc) allow = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--attempts") && i + 1 < argc) attempts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--resolve") && i + 1 < argc) g_resolve = argv[++i];
        else if (!strcmp(argv[i], "--cancel-after-ms") && i + 1 < argc) cancel_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--direct")) direct = 1;
    }
    (void)g_port_for_resolve;
    hv_net_guard_install();
    if (allow) hv_net_guard_test_allow_loopback((uint16_t)allow);
    if (cancel_ms) { hv_curl_set_cancel_flag(&g_flag); pthread_t th; pthread_create(&th, NULL, canceller, (void *)cancel_ms); }
    if (direct) {
        CURL *c = curl_easy_init();
        char err[CURL_ERROR_SIZE] = "";
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
        CURLcode rc = curl_easy_perform(c);
        HvCurlLast l;
        hv_curl_last(&l);
        printf("DIRECT RC=%d POLICY_DENIED=%d DENIED=%d TRIED=%d CONNECTS=%d\n", (int)rc, l.policy_denied, l.addrs_denied, l.addrs_tried, g_connects);
        return 0;
    }
    tss_set_debug_level(1);
    if (attempts) tss_set_max_attempts(attempts);
    plist_t *reqs = calloc((size_t)n, sizeof *reqs);
    HvTssResult *res = calloc((size_t)n, sizeof *res);
    for (int i = 0; i < n; i++) reqs[i] = mkreq();
    double t0 = now_s();
    int stopped = -1;
    int sent = hv_tss_run_requests(reqs, n, url, res, &stopped);
    double el = now_s() - t0;
    for (int i = 0; i < sent; i++) {
        const HvTssResult *r = &res[i];
        printf("REQ %d CODE=%s ATTEMPTS=%d HTTP=%d TSS=%d CURL=%d DENIED=%d TRIED=%d BODY=%lld MAXBUF=%zu\n", i, hv_tss_code_name(r->code), r->attempts, r->http, r->tss_status,
               r->curl_code, r->addrs_denied, r->addrs_tried, r->body_to_lib, r->max_buf);
    }
    printf("SENT=%d STOPPED_AT=%d CONNECTS=%d ELAPSED_MS=%.0f\n", sent, stopped, g_connects, el * 1000);
    return 0;
}
