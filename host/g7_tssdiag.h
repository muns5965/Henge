#ifndef HENGEVM_G7_TSSDIAG_H
#define HENGEVM_G7_TSSDIAG_H
/* The G7 runner's per-attempt TSS line (compiled into host/g7_runner_real.c only; header-only so that tests/test_g7_tssdiag.c can include the same code).
   libtatsu calls a hook after each attempt of a TSS request; the runner writes ONE line to stderr per attempt:
     g7-tss-try: n=<attempt> elapsed_ms=<ms since the request began> curl=<code> http=<code> ms=<transfer ms> deny=<0|1> den=<addrs refused> tried=<addrs connected to> conn=<0|1> tls=<0|1>
                 sent=<0|1> st=<status line> body=<bytes> big=<0|1> gai=<getaddrinfo rc> sockerr=<errno> connerr=<errno> tlsrc=<mbedtls rc> vflags=<verify flags> url=<1..3> fds=<open>/<limit>
                 diag=<libtatsu's fixed-vocabulary diagnosis>
   Numbers and fixed words only. No URL, no host name, no request or reply text, no identifier. The diagnosis string comes from libtatsu's own fixed vocabulary;
   it is still passed through a filter here (printable ASCII only, capped) so that nothing unexpected could ever reach the log. */
#include <stddef.h>
#include <stdio.h>

typedef struct {
    int attempt, curl_rc;
    long http_code;
    long long ms;
    long long elapsed_ms;                   /* since the start of this TSS request (monotonic clock) */
    int deny;                               /* the curl stand-in's deny mode is on */
    int policy_denied, addrs_denied, addrs_tried, connected, tls_done, request_sent, http_status, too_large;
    long long body_to_cb;
    int gai_rc, sock_errno, conn_errno, tls_rc;
    unsigned verify_flags;                  /* certificate verify flags of a failed handshake */
    int url_index;                          /* which built-in TSS address (1..3; 0 = named by the caller) */
    int fds_open, fds_limit;                /* -1 = unknown */
} HvG7TssTry;

/* Copies at most cap-1 printable ASCII characters of `in` into `out`; anything else becomes '?'. */
static inline void hv_g7t_clean(char *out, size_t cap, const char *in) {
    size_t n = 0;
    if (!cap) return;
    for (; in && *in && n + 1 < cap; in++) out[n++] = (*in >= 0x20 && *in < 0x7f) ? *in : '?';
    out[n] = 0;
}

/* Fills `line` (one line, ends in '\n'). */
static inline void hv_g7t_line(char *line, size_t cap, const HvG7TssTry *t, const char *diag) {
    char d[200];
    hv_g7t_clean(d, sizeof d, diag ? diag : "");
    snprintf(line, cap, "g7-tss-try: n=%d elapsed_ms=%lld curl=%d http=%ld ms=%lld deny=%d den=%d tried=%d conn=%d tls=%d sent=%d st=%d body=%lld big=%d "
             "gai=%d sockerr=%d connerr=%d tlsrc=%d vflags=0x%x url=%d fds=%d/%d policy=%d diag=%s\n",
             t->attempt, t->elapsed_ms, t->curl_rc, t->http_code, t->ms, t->deny ? 1 : 0, t->addrs_denied, t->addrs_tried, t->connected ? 1 : 0, t->tls_done ? 1 : 0,
             t->request_sent ? 1 : 0, t->http_status, t->body_to_cb, t->too_large ? 1 : 0, t->gai_rc, t->sock_errno, t->conn_errno, t->tls_rc, t->verify_flags, t->url_index,
             t->fds_open, t->fds_limit, t->policy_denied ? 1 : 0, d);
}

#endif
