#define _GNU_SOURCE 1
/* The slice of libcurl's easy API the restore tools use (see third_party/curl_include/curl/curl.h), for hosts that have
   neither libcurl nor Apple's networking: Android. HTTP/1.1 over sockets, HTTPS through the vendored mbedtls.
   Policy, on purpose and not an accident of the implementation:
   - HTTPS ALWAYS verifies the certificate chain and the host name. CURLOPT_SSL_VERIFYPEER is accepted and ignored (the restore
     tools set it to 0 "to allow untrusted locations"; the iOS stand-in, NSURLSession, verifies regardless, and so does this).
     If no trusted CA certificate can be loaded the request fails; it never falls back to not checking.
   - A redirect from https to plain http is refused. Redirects are followed only with CURLOPT_FOLLOWLOCATION, at most 10.
   - Plain http to Apple's TSS hosts (gs.apple.com and its two alternate names) is refused on every hop, first request or redirect, in every build. No option re-enables it.
   - Trusted CAs come from the first of these that yields certificates: $HV_CA_PATH, the Android system stores
     (/apex/com.android.conscrypt/cacerts, /system/etc/security/cacerts) and /etc/ssl/certs. Loaded once per process.
   - Extra trust anchors are per host, never global: Apple's TSS server (gs.apple.com) chains to Apple Root CA, which public stores lack, so that
     one certificate (third_party/apple_ca, SHA-256 checked at build and again at run time) is added to the system store for connections to exactly that
     host name. Every other host, including after a redirect, gets the system store only. Chain and host name are verified as for any host.
   Synchronous like curl_easy_perform; the body is streamed to the write callback. Not implemented: proxies, cookies, HTTP/2,
   compressed transfer, keep-alive (every request is "Connection: close"). */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "curl/curl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha256.h"
#include "psa/crypto.h"
#include "apple_root_ca.h"

#define CONNECT_TIMEOUT_MS 30000
#define IO_TIMEOUT_S 60
#define MAX_REDIRECTS 10
#define MAX_HEADER_BYTES 65536

typedef struct {
    char *url, *useragent;
    const void *postfields;
    long postsize;
    int post, follow, noprogress;
    curl_write_callback write_cb, header_cb;
    void *write_data, *header_data;
    curl_xferinfo_callback xferinfo;
    curl_progress_callback progress;
    void *xferinfo_data;
    struct curl_slist *headers;
    char *errorbuffer;
    long response_code;
    long max_redir;   /* CURLOPT_MAXREDIRS; 0 = MAX_REDIRECTS */
    long timeout_ms, connect_timeout_ms;   /* CURLOPT_TIMEOUT[_MS]: the whole transfer, redirects included; CURLOPT_CONNECTTIMEOUT[_MS] */
    volatile int cancel;                   /* hv_curl_cancel(): set from any thread, sticky until hv_curl_cancel_reset() */
    int stop;                              /* why the running transfer is being stopped: 0 no, 1 deadline, 2 cancel, 3 a progress callback said stop */
    long long deadline_ns, last_tick_ns;   /* monotonic; deadline 0 = none */
    long long content_length;              /* of the final response; -1 when absent or chunked (CURLINFO_CONTENT_LENGTH_DOWNLOAD_T) */
    long long dl_now, dl_expected;         /* for the progress callbacks that also run while the peer is silent */
} Easy;

static volatile int *g_cancel_flag; /* hv_curl_set_cancel_flag(): a process-wide flag, e.g. the restore tool's "user wants to quit" */

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void hv_curl_cancel(CURL *c) { if (c) __atomic_store_n(&((Easy *)c)->cancel, 1, __ATOMIC_SEQ_CST); }
void hv_curl_cancel_reset(CURL *c) { if (c) __atomic_store_n(&((Easy *)c)->cancel, 0, __ATOMIC_SEQ_CST); }
void hv_curl_set_cancel_flag(volatile int *flag) { g_cancel_flag = flag; }

/* Deny mode (a program that must not reach any server, such as the restore preflight): every transfer started through this client is refused at the top of
   curl_easy_perform, before any name lookup, socket or TLS step, and counted. It covers this client only: another library or a direct socket call in the same
   process is not stopped by it. */
static volatile int g_deny;
static volatile int g_attempts, g_denied;
/* host/net_guard.c sets this: asked with the host name and the address about to be connected, before every connect(); 0 = allowed. NULL = no policy. */
int (*hv_curl_connect_guard)(const char *host, const struct sockaddr *a, socklen_t len) = NULL;

void hv_curl_deny_network(int on) { __atomic_store_n(&g_deny, on ? 1 : 0, __ATOMIC_SEQ_CST); }
int hv_curl_deny_active(void) { return __atomic_load_n(&g_deny, __ATOMIC_SEQ_CST); }
int hv_curl_attempt_count(void) { return __atomic_load_n(&g_attempts, __ATOMIC_SEQ_CST); }   /* curl_easy_perform calls so far */
int hv_curl_denied_count(void) { return __atomic_load_n(&g_denied, __ATOMIC_SEQ_CST); }       /* of those, refused by deny mode */

/* Is the running transfer to be stopped? Sets e->stop (first reason wins). Cheap: called before every wait and every body piece. */
static int stop_check(Easy *e) {
    if (!e) return 0;
    if (e->stop) return e->stop;
    if (e->deadline_ns && now_ns() >= e->deadline_ns) e->stop = 1;
    else if (__atomic_load_n(&e->cancel, __ATOMIC_SEQ_CST) || (g_cancel_flag && __atomic_load_n(g_cancel_flag, __ATOMIC_SEQ_CST))) e->stop = 2;
    return e->stop;
}

/* libcurl calls the progress callbacks about once a second even when no data arrives; a restore tool relies on that to notice "quit" during a stall. */
static void tick(Easy *e) {
    if (!e || e->noprogress || (!e->xferinfo && !e->progress)) return;
    long long n = now_ns();
    if (n - e->last_tick_ns < 1000000000LL) return;
    e->last_tick_ns = n;
    double t = e->dl_expected > 0 ? (double)e->dl_expected : 0;
    if ((e->xferinfo && e->xferinfo(e->xferinfo_data, e->dl_expected > 0 ? e->dl_expected : 0, e->dl_now, 0, 0) != 0) ||
        (e->progress && e->progress(e->xferinfo_data, t, (double)e->dl_now, 0, 0) != 0))
        if (!e->stop) e->stop = 3;
}

/* Waits for the socket in slices of at most 200 ms so that the deadline, a cancel and the progress callbacks are noticed.
   1 ready; 0 no activity for limit_ms; -1 stopped (e->stop says why) or poll failed. */
static int wait_fd(Easy *e, int fd, short ev, long limit_ms) {
    long long t0 = now_ns();
    for (;;) {
        if (stop_check(e)) return -1;
        long slice = 200;
        if (e && e->deadline_ns) {
            long long rem = (e->deadline_ns - now_ns() + 999999) / 1000000;
            if (rem < slice) slice = rem < 1 ? 1 : (long)rem;
        }
        struct pollfd p = {fd, ev, 0};
        int r = poll(&p, 1, (int)slice);
        if (r > 0) return 1;
        if (r < 0 && errno != EINTR) return -1;
        tick(e);
        if (e && e->stop) return -1;
        if ((now_ns() - t0) / 1000000 >= limit_ms) return 0;
    }
}

/* ---- trusted CAs, once per process ---- */
static pthread_mutex_t ca_mu = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_x509_crt ca_chain;
static int ca_state; /* 0 not tried, 1 loaded, -1 none */
static int ca_count, ca_failed;
static __thread char tls_diag[12288];
static __thread size_t chain_len;
static __thread int tls_done; /* the last request on this thread finished its TLS handshake with a verified certificate */
static char ca_where[256];

static int load_cas(void) {
    pthread_mutex_lock(&ca_mu);
    if (!ca_state) {
        mbedtls_x509_crt_init(&ca_chain);
        psa_crypto_init();
#ifdef HV_ALLOW_CA_ENV /* tests only: it widens or narrows trust, so release builds do not read it */
        const char *env = getenv("HV_CA_PATH");
#else
        const char *env = NULL;
#endif
        /* An explicit $HV_CA_PATH is the only store used: if it yields nothing the request fails rather than widening trust. */
        const char *sys[] = {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts", "/etc/ssl/certs", NULL};
        const char *only[] = {env, NULL};
        const char **dirs = env && env[0] ? only : sys;
        for (int i = 0; dirs[i] && ca_state == 0; i++) {
            mbedtls_x509_crt_init(&ca_chain);
            int bad = mbedtls_x509_crt_parse_path(&ca_chain, dirs[i]); /* the number of files it could not parse (or a negative error); what parsed stays */
            int n = 0;
            for (const mbedtls_x509_crt *c = &ca_chain; c && c->raw.p; c = c->next) n++;
            if (n > 0) { ca_state = 1; ca_count = n; ca_failed = bad; snprintf(ca_where, sizeof ca_where, "%s", dirs[i]); }
            else mbedtls_x509_crt_free(&ca_chain);
        }
        if (ca_state == 0) ca_state = -1;
    }
    int ok = ca_state == 1;
    pthread_mutex_unlock(&ca_mu);
    return ok;
}

/* How many CA certificates were loaded and from where (for tests and logs). */
int hv_curl_ca_count(const char **where) {
    load_cas();
    if (where) *where = ca_where;
    return ca_state == 1 ? ca_count : 0;
}

/* Files in the CA directory that could not be parsed (negative: the directory read itself failed). */
int hv_curl_ca_unparsed(void) { load_cas(); return ca_state == 1 ? ca_failed : 0; }

/* Why the last certificate check on this thread failed: flags, and every certificate the server sent (subject, issuer, signature, key). Empty otherwise. */
const char *hv_curl_tls_diag(void) { return tls_diag; }

/* 1 if the last request on this thread completed a TLS handshake whose certificate chain and host name were verified. */
int hv_curl_tls_completed(void) { return tls_done; }

static int fcntl_nonblock(int fd, int on) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}

/* ---- connection ---- */
typedef struct {
    int fd, tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context ent;
    mbedtls_x509_crt plus; /* system CAs + the extra anchor of this host, when it has one */
    int has_plus;
    Easy *owner;        /* whose transfer this is: its deadline, cancel flag and progress callbacks apply to every wait on the socket */
    char err[CURL_ERROR_SIZE];
} Conn; /* fd must stay the first member: the TLS layer is handed the Conn, the plain path the same pointer */

static int bio_send(void *ctx, const unsigned char *b, size_t n) {
    Conn *c = ctx;
    for (;;) {
        int w = wait_fd(c->owner, c->fd, POLLOUT, IO_TIMEOUT_S * 1000L);
        if (w <= 0) { errno = w == 0 ? ETIMEDOUT : ECANCELED; return -0x004E; }
        ssize_t r = send(c->fd, b, n, MSG_NOSIGNAL);
        if (r >= 0) return (int)r;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return -0x004E; /* a send failure, as mbedtls' own net code reports it */
    }
}

static int bio_recv(void *ctx, unsigned char *b, size_t n) {
    Conn *c = ctx;
    for (;;) {
        int w = wait_fd(c->owner, c->fd, POLLIN, IO_TIMEOUT_S * 1000L); /* idle limit; the whole-transfer deadline is checked inside */
        if (w <= 0) { errno = w == 0 ? ETIMEDOUT : ECANCELED; return -0x004C; }
        ssize_t r = recv(c->fd, b, n, 0);
        if (r >= 0) return (int)r;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return -0x004C;
    }
}

static int conn_write(Conn *c, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n) {
        int r = c->tls ? mbedtls_ssl_write(&c->ssl, b, n) : bio_send(c, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return -1;
        b += r, n -= (size_t)r;
    }
    return 0;
}

/* >0 bytes, 0 orderly close, -1 error or timeout. */
static int conn_read(Conn *c, void *p, size_t n) {
    for (;;) {
        int r = c->tls ? mbedtls_ssl_read(&c->ssl, p, n) : bio_recv(c, p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r < 0 ? -1 : r;
    }
}

static void conn_close(Conn *c) {
    if (c->has_plus) mbedtls_x509_crt_free(&c->plus), c->has_plus = 0;
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

/* ---- per-host extra trust anchors ---- */
typedef struct { const char *host; char *pem_owned; const char *pem; const char *sha256; } Anchor;

static void hex(const unsigned char *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* 1 = this host has an anchor (filled in), 0 = none, -1 = the anchor is configured but unusable (fail closed). */
static int find_anchor(const char *host, Anchor *a, char *err) {
    memset(a, 0, sizeof *a);
    if (!strcasecmp(host, "gs.apple.com")) {
        a->host = "gs.apple.com", a->pem = apple_root_ca_pem, a->sha256 = APPLE_ROOT_CA_SHA256;
        return 1;
    }
#ifdef HV_ALLOW_CA_ENV /* tests: HV_EXTRA_ANCHOR="host|pem-file|sha256-hex" (the hash may be empty for "no pin") */
    const char *e = getenv("HV_EXTRA_ANCHOR");
    if (e && *e) {
        char spec[1024];
        snprintf(spec, sizeof spec, "%s", e);
        char *bar1 = strchr(spec, '|'), *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
        if (!bar2) { snprintf(err, CURL_ERROR_SIZE, "bad HV_EXTRA_ANCHOR"); return -1; }
        *bar1 = *bar2 = 0;
        if (strcasecmp(host, spec)) return 0;
        FILE *f = fopen(bar1 + 1, "rb");
        if (!f) { snprintf(err, CURL_ERROR_SIZE, "cannot read the extra anchor file"); return -1; }
        char *buf = malloc(65536);
        size_t n = buf ? fread(buf, 1, 65535, f) : 0;
        fclose(f);
        if (!buf || !n) { free(buf); return -1; }
        buf[n] = 0;
        static __thread char pin[80];
        snprintf(pin, sizeof pin, "%s", bar2 + 1);
        a->host = host, a->pem_owned = buf, a->pem = buf, a->sha256 = pin[0] ? pin : NULL;
        return 1;
    }
#else
    (void)err;
#endif
    return 0;
}

/* The system store plus the host's anchor, after checking the anchor's SHA-256. Fail closed on any problem. */
static int build_plus(Conn *c, const Anchor *a) {
    mbedtls_x509_crt extra;
    mbedtls_x509_crt_init(&extra);
    mbedtls_x509_crt_init(&c->plus);
    c->has_plus = 1;
    int rc = -1;
    if (mbedtls_x509_crt_parse(&extra, (const unsigned char *)a->pem, strlen(a->pem) + 1) != 0 || !extra.raw.p || extra.next) {
        snprintf(c->err, sizeof c->err, "the extra trust anchor for %.80s could not be parsed; not connecting", a->host);
        goto out;
    }
    if (a->sha256) {
        unsigned char h[32];
        char hx[65];
        if (mbedtls_sha256(extra.raw.p, extra.raw.len, h, 0) != 0) goto out;
        hex(h, 32, hx);
        if (strcasecmp(hx, a->sha256)) {
            snprintf(c->err, sizeof c->err, "the extra trust anchor for %.80s does not match its pinned SHA-256; not connecting", a->host);
            goto out;
        }
    }
    for (const mbedtls_x509_crt *p = &ca_chain; p && p->raw.p; p = p->next)
        if (mbedtls_x509_crt_parse_der(&c->plus, p->raw.p, p->raw.len) != 0) goto copyfail;
    if (mbedtls_x509_crt_parse_der(&c->plus, extra.raw.p, extra.raw.len) != 0) goto copyfail;
    rc = 0;
    goto out;
copyfail:
    snprintf(c->err, sizeof c->err, "could not assemble the trust store for %.80s; not connecting", a->host);
out:
    mbedtls_x509_crt_free(&extra);
    return rc;
}

/* Called by mbedtls for every certificate of the chain while it verifies it (leaf first). It only records; the verdict is untouched. */
static int record_cert(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)ctx;
    size_t left = sizeof tls_diag - chain_len;
    if (left < 1400) return 0;
    chain_len += (size_t)snprintf(tls_diag + chain_len, left, "CERT depth %d, flags for it 0x%x:\n", depth, (unsigned)*flags);
    int n = mbedtls_x509_crt_info(tls_diag + chain_len, sizeof tls_diag - chain_len, "  ", crt);
    if (n > 0) chain_len += (size_t)n;
    return 0;
}

static CURLcode conn_open(Conn *c, Easy *e, int https, const char *host, const char *port) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->owner = e;
    tls_done = 0, tls_diag[0] = 0, chain_len = 0; /* diagnostics describe the connection just made, never an earlier one */
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *res = NULL;
    if (stop_check(e)) return CURLE_ABORTED_BY_CALLBACK; /* the caller turns e->stop into the right code */
    /* getaddrinfo cannot be interrupted: a deadline or cancel that arrives during name resolution takes effect when it returns */
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        snprintf(c->err, sizeof c->err, "could not resolve host %s", host);
        return stop_check(e) ? CURLE_ABORTED_BY_CALLBACK : CURLE_COULDNT_RESOLVE_HOST;
    }
    if (stop_check(e)) { freeaddrinfo(res); return CURLE_ABORTED_BY_CALLBACK; }
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        struct timeval tv = {IO_TIMEOUT_S, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        /* connect with a timeout: non-blocking, wait, check */
        fcntl_nonblock(s, 1);
        if (hv_curl_connect_guard && hv_curl_connect_guard(host, a->ai_addr, a->ai_addrlen) != 0) { close(s); continue; }   /* host/net_guard.c */
        int r = connect(s, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            if (wait_fd(e, s, POLLOUT, e && e->connect_timeout_ms > 0 ? e->connect_timeout_ms : CONNECT_TIMEOUT_MS) == 1) {
                int e = 0;
                socklen_t l = sizeof e;
                getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l);
                r = e ? -1 : 0;
            } else r = -1;
        }
        fcntl_nonblock(s, 0);
        if (r == 0) fd = s; else close(s);
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(c->err, sizeof c->err, "could not connect to %s:%s", host, port);
        return stop_check(e) ? CURLE_ABORTED_BY_CALLBACK : CURLE_COULDNT_CONNECT;
    }
    c->fd = fd;
    if (!https) return CURLE_OK;
    if (!load_cas()) {
        snprintf(c->err, sizeof c->err, "no trusted CA certificates could be loaded; not connecting without checking the server");
        close(fd), c->fd = -1;
        return CURLE_SSL_CACERT_BADFILE;
    }
    const mbedtls_x509_crt *roots = &ca_chain;
    Anchor an;
    int fa = find_anchor(host, &an, c->err);
    if (fa < 0 || (fa == 1 && build_plus(c, &an) != 0)) {
        if (!c->err[0]) snprintf(c->err, sizeof c->err, "the extra trust anchor for %.80s is unusable; not connecting", host);
        free(an.pem_owned);
        conn_close(c);
        return CURLE_SSL_CACERT_BADFILE;
    }
    free(an.pem_owned);
    if (fa == 1) roots = &c->plus;
    c->tls = 1;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->ent);
    int ret = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent, (const unsigned char *)"henge-curl", 10);
    if (ret == 0) ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED); /* the policy: always */
        mbedtls_ssl_conf_ca_chain(&c->conf, (mbedtls_x509_crt *)roots, NULL);
        mbedtls_ssl_conf_verify(&c->conf, record_cert, NULL);
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
        ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    }
    if (ret == 0) ret = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (ret == 0) {
        mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
        while ((ret = mbedtls_ssl_handshake(&c->ssl)) == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    }
    if (ret != 0 && stop_check(e)) { conn_close(c); return CURLE_ABORTED_BY_CALLBACK; }
    if (ret != 0) {
        char m[160];
        mbedtls_strerror(ret, m, sizeof m);
        uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || (flags && flags != 0xFFFFFFFF)) {
            char v[512] = "";
            mbedtls_x509_crt_verify_info(v, sizeof v, "", flags); /* one line per flag, each ending in a newline */
            for (char *q = v; *q; q++) if (*q == '\n') *q = ';';
            snprintf(c->err, sizeof c->err, "certificate check failed for %.80s: flags=0x%x %.150s", host, (unsigned)flags, v);
            /* the whole story goes to the diagnostics buffer, for the person reading the log */
            /* the chain was recorded by record_cert while mbedtls verified it; put the summary in front */
            char head[200];
            int hn = snprintf(head, sizeof head, "VERIFY_FLAGS=0x%x\nCA_COUNT=%d CA_FROM=%s\n", (unsigned)flags, ca_count, ca_where);
            if (hn > 0 && chain_len + (size_t)hn < sizeof tls_diag) {
                memmove(tls_diag + hn, tls_diag, chain_len + 1);
                memcpy(tls_diag, head, (size_t)hn);
                chain_len += (size_t)hn;
            }
            if (chain_len == (size_t)hn || chain_len == 0) snprintf(tls_diag + chain_len, sizeof tls_diag - chain_len, "(no certificate was presented)\n");
            conn_close(c);
            return CURLE_PEER_FAILED_VERIFICATION;
        }
        char m2[160];
        mbedtls_strerror(ret, m2, sizeof m2);
        snprintf(c->err, sizeof c->err, "TLS handshake with %.100s failed: %.80s", host, m2);
        conn_close(c);
        return CURLE_SSL_CONNECT_ERROR;
    }
    tls_done = 1;
    tls_diag[0] = 0, chain_len = 0; /* verified: the chain dump is for failures only */
    return CURLE_OK;
}

/* ---- options ---- */
CURLcode curl_global_init(long flags) { (void)flags; return CURLE_OK; }
void curl_global_cleanup(void) {}

CURL *curl_easy_init(void) {
    Easy *e = calloc(1, sizeof *e);
    if (e) e->noprogress = 1, e->content_length = -1;
    return (CURL *)e;
}

void curl_easy_cleanup(CURL *c) {
    Easy *e = (Easy *)c;
    if (!e) return;
    free(e->url);
    free(e->useragent);
    free(e);
}

CURLcode curl_easy_setopt(CURL *c, CURLoption opt, ...) {
    Easy *e = (Easy *)c;
    va_list ap;
    va_start(ap, opt);
    switch (opt) {
    case CURLOPT_URL: free(e->url); e->url = strdup(va_arg(ap, const char *)); break;
    case CURLOPT_USERAGENT: free(e->useragent); e->useragent = strdup(va_arg(ap, const char *)); break;
    case CURLOPT_POSTFIELDS: e->postfields = va_arg(ap, const void *); e->post = 1; break;
    case CURLOPT_POSTFIELDSIZE: e->postsize = va_arg(ap, long); break;
    case CURLOPT_HTTPGET: e->post = 0; (void)va_arg(ap, long); break;
    case CURLOPT_FOLLOWLOCATION: e->follow = (int)va_arg(ap, long); break;
    case CURLOPT_WRITEFUNCTION: e->write_cb = va_arg(ap, curl_write_callback); break;
    case CURLOPT_WRITEDATA: e->write_data = va_arg(ap, void *); break;
    case CURLOPT_HEADERFUNCTION: e->header_cb = va_arg(ap, curl_write_callback); break;
    case CURLOPT_HEADERDATA: e->header_data = va_arg(ap, void *); break;
    case CURLOPT_HTTPHEADER: e->headers = va_arg(ap, struct curl_slist *); break;
    case CURLOPT_ERRORBUFFER: e->errorbuffer = va_arg(ap, char *); break;
    case CURLOPT_XFERINFOFUNCTION: e->xferinfo = va_arg(ap, curl_xferinfo_callback); break;
    case CURLOPT_PROGRESSFUNCTION: e->progress = va_arg(ap, curl_progress_callback); break;
    case CURLOPT_XFERINFODATA: e->xferinfo_data = va_arg(ap, void *); break;
    case CURLOPT_NOPROGRESS: e->noprogress = (int)va_arg(ap, long); break;
    case CURLOPT_MAXREDIRS: e->max_redir = va_arg(ap, long); break;
    case CURLOPT_TIMEOUT: e->timeout_ms = va_arg(ap, long) * 1000L; break;
    case CURLOPT_TIMEOUT_MS: e->timeout_ms = va_arg(ap, long); break;
    case CURLOPT_CONNECTTIMEOUT: e->connect_timeout_ms = va_arg(ap, long) * 1000L; break;
    case CURLOPT_CONNECTTIMEOUT_MS: e->connect_timeout_ms = va_arg(ap, long); break;
    default: (void)va_arg(ap, void *); break; /* VERBOSE; SSL_VERIFYPEER is ignored on purpose (see the top) */
    }
    va_end(ap);
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *c, CURLINFO info, ...) {
    Easy *e = (Easy *)c;
    va_list ap;
    va_start(ap, info);
    if (info == CURLINFO_RESPONSE_CODE) *va_arg(ap, long *) = e->response_code;
    else if (info == CURLINFO_CONTENT_LENGTH_DOWNLOAD_T) *va_arg(ap, long long *) = e->content_length;
    va_end(ap);
    return CURLE_OK;
}

const char *curl_easy_strerror(CURLcode c) {
    switch (c) {
    case CURLE_OK: return "No error";
    case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
    case CURLE_URL_MALFORMAT: return "URL using bad/illegal format or missing URL";
    case CURLE_COULDNT_RESOLVE_HOST: return "Could not resolve hostname";
    case CURLE_COULDNT_CONNECT: return "Could not connect to server";
    case CURLE_WRITE_ERROR: return "Failed writing received data to disk/application";
    case CURLE_OUT_OF_MEMORY: return "Out of memory";
    case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
    case CURLE_ABORTED_BY_CALLBACK: return "Operation was aborted by an application callback";
    case CURLE_TOO_MANY_REDIRECTS: return "Number of redirects hit maximum amount";
    case CURLE_GOT_NOTHING: return "Server returned nothing (no headers, no data)";
    case CURLE_SEND_ERROR: return "Failed sending data to the peer";
    case CURLE_RECV_ERROR: return "Failure when receiving data from the peer";
    case CURLE_SSL_CONNECT_ERROR: return "SSL connect error";
    case CURLE_PEER_FAILED_VERIFICATION: return "SSL peer certificate or SSH remote key was not OK";
    case CURLE_SSL_CACERT_BADFILE: return "Problem with the SSL CA cert (path? access rights?)";
    default: return "Unknown error";
    }
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s) {
    struct curl_slist *n = calloc(1, sizeof *n);
    if (!n) return list;
    n->data = strdup(s);
    if (!list) return n;
    struct curl_slist *p = list;
    while (p->next) p = p->next;
    p->next = n;
    return list;
}

void curl_slist_free_all(struct curl_slist *list) {
    while (list) {
        struct curl_slist *n = list->next;
        free(list->data);
        free(list);
        list = n;
    }
}

/* ---- URL ---- */
typedef struct { int https; char host[256], port[8], path[2048]; } Url;

static int parse_url(const char *u, Url *o) {
    memset(o, 0, sizeof *o);
    if (!strncasecmp(u, "https://", 8)) o->https = 1, u += 8;
    else if (!strncasecmp(u, "http://", 7)) u += 7;
    else return CURLE_UNSUPPORTED_PROTOCOL;
    const char *end = u + strcspn(u, "/?#");
    const char *at = memchr(u, '@', (size_t)(end - u));
    if (at) u = at + 1;
    const char *hostend = end, *colon = NULL;
    if (*u == '[') { /* [v6]:port */
        const char *rb = memchr(u, ']', (size_t)(end - u));
        if (!rb) return CURLE_URL_MALFORMAT;
        if (rb + 1 < end && rb[1] == ':') colon = rb + 1;
        hostend = rb;
        u++;
    } else {
        for (const char *p = u; p < end; p++) if (*p == ':') colon = p;
        if (colon) hostend = colon;
    }
    size_t hl = (size_t)(hostend - u);
    if (hl == 0 || hl >= sizeof o->host) return CURLE_URL_MALFORMAT;
    memcpy(o->host, u, hl);
    if (colon) {
        size_t pl = (size_t)(end - colon - 1);
        if (pl == 0 || pl >= sizeof o->port) return CURLE_URL_MALFORMAT;
        memcpy(o->port, colon + 1, pl);
    } else snprintf(o->port, sizeof o->port, "%s", o->https ? "443" : "80");
    const char *rest = end;
    if (*rest == '/') {
        size_t n = strcspn(rest, "#");
        if (n >= sizeof o->path) return CURLE_URL_MALFORMAT;
        memcpy(o->path, rest, n);
    } else if (*rest == '?') {
        size_t n = strcspn(rest, "#");
        if (n + 1 >= sizeof o->path) return CURLE_URL_MALFORMAT;
        o->path[0] = '/';
        memcpy(o->path + 1, rest, n);
    } else strcpy(o->path, "/");
    return CURLE_OK;
}

/* Resolve a Location header against the URL it answered. */
static int resolve_location(const Url *base, const char *loc, char *out, size_t cap) {
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) return snprintf(out, cap, "%s", loc) < (int)cap ? 0 : -1;
    int dflt = base->https ? !strcmp(base->port, "443") : !strcmp(base->port, "80");
    char auth[300];
    if (dflt) snprintf(auth, sizeof auth, strchr(base->host, ':') ? "[%s]" : "%s", base->host);
    else snprintf(auth, sizeof auth, strchr(base->host, ':') ? "[%s]:%s" : "%s:%s", base->host, base->port);
    const char *scheme = base->https ? "https" : "http";
    int n;
    if (loc[0] == '/' && loc[1] == '/') n = snprintf(out, cap, "%s:%s", scheme, loc);
    else if (loc[0] == '/') n = snprintf(out, cap, "%s://%s%s", scheme, auth, loc);
    else {
        char dir[2048];
        snprintf(dir, sizeof dir, "%s", base->path);
        dir[strcspn(dir, "?")] = 0;
        char *sl = strrchr(dir, '/');
        if (sl) sl[1] = 0; else strcpy(dir, "/");
        n = snprintf(out, cap, "%s://%s%s%s", scheme, auth, dir, loc);
    }
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* ---- one request/response ---- */
typedef struct { char *p; size_t len, cap; } Buf;

static int buf_add(Buf *b, const void *d, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return -1;
        b->p = np, b->cap = nc;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = 0;
    return 0;
}

typedef struct {
    Easy *e;
    Conn *c;
    long long dl;
    long long expected;
    CURLcode abort_code;
} Xfer;

/* Hands body bytes to the write callback and the progress callback. 0 ok, -1 stop (abort_code set). */
static int deliver(Xfer *x, const char *p, size_t n) {
    Easy *e = x->e;
    if (n) {
        size_t w = e->write_cb ? e->write_cb((char *)p, 1, n, e->write_data) : fwrite(p, 1, n, e->write_data ? (FILE *)e->write_data : stdout);
        if (w != n) { x->abort_code = CURLE_WRITE_ERROR; return -1; }
        x->dl += (long long)n;
        e->dl_now = x->dl, e->dl_expected = x->expected;
    }
    if (stop_check(e)) { x->abort_code = e->stop == 1 ? CURLE_OPERATION_TIMEDOUT : CURLE_ABORTED_BY_CALLBACK; return -1; } /* data arriving fast never reaches a wait, so check here too */
    if (!e->noprogress) {
        double t = x->expected > 0 ? (double)x->expected : 0;
        if ((e->xferinfo && e->xferinfo(e->xferinfo_data, x->expected > 0 ? x->expected : 0, x->dl, 0, 0) != 0) ||
            (e->progress && e->progress(e->xferinfo_data, t, (double)x->dl, 0, 0) != 0)) {
            x->abort_code = CURLE_ABORTED_BY_CALLBACK;
            return -1;
        }
    }
    return 0;
}

static int io_error_is_timeout(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT; }

/* Reads one line (CRLF or LF terminated, terminator stripped) from the connection through the buffer rb. */
typedef struct { unsigned char b[8192]; size_t pos, len; } Rd;

static int rd_fill(Conn *c, Rd *r) {
    int n = conn_read(c, r->b, sizeof r->b);
    if (n <= 0) return n;
    r->pos = 0, r->len = (size_t)n;
    return n;
}

static int rd_byte(Conn *c, Rd *r) {
    if (r->pos == r->len) {
        int n = rd_fill(c, r);
        if (n <= 0) return n == 0 ? -1 : -2;
    }
    return r->b[r->pos++];
}

/* 0 ok, -1 clean EOF before any byte, -2 error. */
static int rd_line(Conn *c, Rd *r, Buf *line) {
    line->len = 0;
    if (line->p) line->p[0] = 0;
    int ch, any = 0;
    while ((ch = rd_byte(c, r)) >= 0) {
        any = 1;
        if (ch == '\n') {
            if (line->len && line->p[line->len - 1] == '\r') line->p[--line->len] = 0;
            return 0;
        }
        if (buf_add(line, &ch, 1)) return -2;
        if (line->len > MAX_HEADER_BYTES) return -2;
    }
    return ch == -1 && !any ? -1 : -2;
}

/* Reads exactly the body (n bytes if n >= 0, else until close) and passes it on. */
static CURLcode body_sized(Xfer *x, Rd *r, long long n) {
    char tmp[16384];
    while (n != 0) {
        size_t take;
        if (r->pos < r->len) {
            take = r->len - r->pos;
            if (n > 0 && (long long)take > n) take = (size_t)n;
            if (deliver(x, (char *)r->b + r->pos, take)) return x->abort_code;
            r->pos += take;
        } else {
            size_t want = sizeof tmp;
            if (n > 0 && (long long)want > n) want = (size_t)n;
            int k = conn_read(x->c, tmp, want);
            if (k == 0) return n < 0 ? CURLE_OK : CURLE_RECV_ERROR; /* closed early on a sized body: error */
            if (k < 0) return io_error_is_timeout() ? CURLE_OPERATION_TIMEDOUT : CURLE_RECV_ERROR;
            if (deliver(x, tmp, (size_t)k)) return x->abort_code;
            take = (size_t)k;
        }
        if (n > 0) n -= (long long)take;
    }
    return CURLE_OK;
}

static CURLcode body_chunked(Xfer *x, Rd *r) {
    Buf line = {0};
    CURLcode rc = CURLE_OK;
    for (;;) {
        int lr = rd_line(x->c, r, &line);
        if (lr) { rc = CURLE_RECV_ERROR; break; }
        char *end;
        unsigned long long sz = strtoull(line.p, &end, 16);
        if (end == line.p) { rc = CURLE_RECV_ERROR; break; }
        if (sz == 0) {
            /* trailers up to the blank line */
            while (rd_line(x->c, r, &line) == 0 && line.len) {}
            break;
        }
        rc = body_sized(x, r, (long long)sz);
        if (rc) break;
        if (rd_line(x->c, r, &line)) { rc = CURLE_RECV_ERROR; break; } /* CRLF after the chunk */
    }
    free(line.p);
    return rc;
}

static void seterr(Easy *e, const char *fmt, ...) {
    if (!e->errorbuffer) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->errorbuffer, CURL_ERROR_SIZE, fmt, ap);
    va_end(ap);
}

static int has_header(struct curl_slist *h, const char *name) {
    size_t n = strlen(name);
    for (; h; h = h->next) if (!strncasecmp(h->data, name, n) && h->data[n] == ':') return 1;
    return 0;
}

/* One hop. On a redirect to follow, location is filled and *redirect set. */
static CURLcode do_request(Easy *e, const Url *u, int post, char *location, size_t loccap, int *redirect, long long *dl_total) {
    Conn c;
    *redirect = 0;
    CURLcode rc = conn_open(&c, e, u->https, u->host, u->port);
    if (rc) { seterr(e, "%s", c.err); return rc; }

    Buf req = {0};
    char tmp[512];
    long long plen = 0;
    if (post) plen = e->postsize >= 0 && e->postfields && e->postsize ? e->postsize : (e->postfields ? (long long)strlen(e->postfields) : 0);
    int dflt = u->https ? !strcmp(u->port, "443") : !strcmp(u->port, "80");
    /* build the Host line properly */
    buf_add(&req, post ? "POST " : "GET ", post ? 5 : 4);
    buf_add(&req, u->path, strlen(u->path));
    buf_add(&req, " HTTP/1.1\r\nHost: ", 17);
    if (strchr(u->host, ':')) buf_add(&req, "[", 1);
    buf_add(&req, u->host, strlen(u->host));
    if (strchr(u->host, ':')) buf_add(&req, "]", 1);
    if (!dflt) { snprintf(tmp, sizeof tmp, ":%s", u->port); buf_add(&req, tmp, strlen(tmp)); }
    buf_add(&req, "\r\nConnection: close\r\n", 21);
    if (!has_header(e->headers, "Accept")) buf_add(&req, "Accept: */*\r\n", 13);
    if (e->useragent && !has_header(e->headers, "User-Agent")) {
        snprintf(tmp, sizeof tmp, "User-Agent: %.400s\r\n", e->useragent);
        buf_add(&req, tmp, strlen(tmp));
    }
    for (struct curl_slist *h = e->headers; h; h = h->next) {
        if (!strchr(h->data, ':') || strpbrk(h->data, "\r\n")) continue; /* never let a header line smuggle another */
        { const char *colon = strchr(h->data, ':'); const char *v = colon + 1; while (*v == ' ' || *v == '\t') v++; if (!*v) continue; } /* as libcurl: "Expect:" removes a header */
        if (!strncasecmp(h->data, "Content-Length:", 15) || !strncasecmp(h->data, "Host:", 5) || !strncasecmp(h->data, "Connection:", 11)) continue;
        buf_add(&req, h->data, strlen(h->data));
        buf_add(&req, "\r\n", 2);
    }
    if (post) {
        snprintf(tmp, sizeof tmp, "Content-Length: %lld\r\n", plen);
        buf_add(&req, tmp, strlen(tmp));
        if (!has_header(e->headers, "Content-Type")) buf_add(&req, "Content-Type: application/x-www-form-urlencoded\r\n", strlen("Content-Type: application/x-www-form-urlencoded\r\n"));
    }
    buf_add(&req, "\r\n", 2);
    if (post && plen) buf_add(&req, e->postfields, (size_t)plen);

    if (!req.p || conn_write(&c, req.p, req.len)) {
        free(req.p);
        seterr(e, "send failed to %s", u->host);
        conn_close(&c);
        return CURLE_SEND_ERROR;
    }
    free(req.p);

    Rd *rd = calloc(1, sizeof *rd);
    Buf line = {0};
    if (!rd) { conn_close(&c); return CURLE_OUT_OF_MEMORY; }
    Xfer x = {.e = e, .c = &c, .expected = -1};
    int status = 0;
    rc = CURLE_OK;

    /* status line, skipping 1xx interim responses */
    for (;;) {
        int lr = rd_line(&c, rd, &line);
        if (lr) { rc = lr == -1 ? CURLE_GOT_NOTHING : (io_error_is_timeout() ? CURLE_OPERATION_TIMEDOUT : CURLE_RECV_ERROR); goto out; }
        if (strncmp(line.p, "HTTP/1.", 7) != 0) { rc = CURLE_RECV_ERROR; seterr(e, "not an HTTP response from %s", u->host); goto out; }
        status = atoi(line.p + 8);
        if (status < 100 || status > 999) { rc = CURLE_RECV_ERROR; goto out; }
        if (status >= 200 || status == 101) break;
        while (rd_line(&c, rd, &line) == 0 && line.len) {} /* headers of the interim response */
    }
    e->response_code = status;

    /* headers: pass each to the header callback in curl's shape, keep the few we act on */
    long long clen = -1;
    int chunked = 0;
    char *loc = NULL;
    if (e->header_cb) {
        char sl[64];
        int n = snprintf(sl, sizeof sl, "HTTP/1.1 %d\r\n", status);
        e->header_cb(sl, 1, (size_t)n, e->header_data);
    }
    for (;;) {
        int lr = rd_line(&c, rd, &line);
        if (lr) { rc = CURLE_RECV_ERROR; free(loc); goto out; }
        if (line.len == 0) break;
        if (e->header_cb) {
            buf_add(&line, "\r\n", 2);
            e->header_cb(line.p, 1, line.len, e->header_data);
            line.len -= 2, line.p[line.len] = 0;
        }
        char *colon = strchr(line.p, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(line.p, "Content-Length")) { char *end; long long l = strtoll(v, &end, 10); if (end != v && l >= 0) clen = l; }
        else if (!strcasecmp(line.p, "Transfer-Encoding") && strcasestr(v, "chunked")) chunked = 1;
        else if (!strcasecmp(line.p, "Location")) { free(loc); loc = strdup(v); }
    }
    if (e->header_cb) e->header_cb("\r\n", 1, 2, e->header_data);

    int is_redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (is_redirect && e->follow && loc) {
        if (resolve_location(u, loc, location, loccap)) { rc = CURLE_URL_MALFORMAT; free(loc); goto out; }
        *redirect = status;
        free(loc);
        goto out; /* body of the redirect is dropped; the connection closes */
    }
    free(loc);

    x.expected = chunked ? -1 : clen;
    e->content_length = x.expected;
    int nobody = status == 204 || status == 304 || (status >= 100 && status < 200);
    if (nobody) rc = CURLE_OK;
    else if (chunked) rc = body_chunked(&x, rd);
    else rc = body_sized(&x, rd, clen >= 0 ? clen : -1);
    *dl_total += x.dl;
    if (rc == CURLE_RECV_ERROR) seterr(e, "connection to %s lost during the transfer", u->host);
    else if (rc == CURLE_OPERATION_TIMEDOUT) seterr(e, "timed out reading from %s", u->host);
out:
    if (rc && !(e->errorbuffer && e->errorbuffer[0])) {
        if (rc == CURLE_GOT_NOTHING) seterr(e, "empty reply from %s", u->host);
        else if (rc == CURLE_RECV_ERROR) seterr(e, "receive failed from %s", u->host);
        else if (rc == CURLE_OPERATION_TIMEDOUT) seterr(e, "timed out reading from %s", u->host);
    }
    free(line.p);
    free(rd);
    conn_close(&c);
    return rc;
}

/* Apple's signing server must only ever be reached over TLS (second line of defence behind libtatsu's own check). */
static int is_tss_host(const char *h) {
    static const char *const names[] = {"gs.apple.com", "gs.apple.com.akadns.net", "gs.apple.com.v.aaplimg.com"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcasecmp(h, names[i])) return 1;
    return 0;
}

static CURLcode perform_hops(Easy *e) {
    if (e->errorbuffer) e->errorbuffer[0] = 0;
    e->response_code = 0;
    e->content_length = -1;
    if (!e->url) { seterr(e, "no URL set"); return CURLE_URL_MALFORMAT; }
    char cur[2100];
    snprintf(cur, sizeof cur, "%s", e->url);
    int post = e->post;
    long long total = 0;
    for (int hop = 0;; hop++) {
        Url u;
        int pr = parse_url(cur, &u);
        if (pr) { seterr(e, pr == CURLE_UNSUPPORTED_PROTOCOL ? "only http and https URLs are supported" : "malformed URL"); return (CURLcode)pr; }
        if (!u.https && is_tss_host(u.host)) { seterr(e, "refusing plain http to Apple's TSS server %s", u.host); return CURLE_UNSUPPORTED_PROTOCOL; }
        char next[2100];
        int redirect = 0;
        CURLcode rc = do_request(e, &u, post, next, sizeof next, &redirect, &total);
        if (rc) return rc;
        if (!redirect) return CURLE_OK;
        if (hop >= (e->max_redir > 0 && e->max_redir < MAX_REDIRECTS ? (int)e->max_redir : MAX_REDIRECTS)) { seterr(e, "more than %d redirects", MAX_REDIRECTS); return CURLE_TOO_MANY_REDIRECTS; }
        Url nu;
        if (parse_url(next, &nu) == 0 && u.https && !nu.https) {
            seterr(e, "refusing a redirect from https to plain http");
            return CURLE_UNSUPPORTED_PROTOCOL;
        }
        if (redirect == 301 || redirect == 302 || redirect == 303) post = 0; /* as curl: the request becomes a GET */
        snprintf(cur, sizeof cur, "%s", next);
    }
}

/* One transfer: the deadline starts here and covers every redirect hop; a deadline, hv_curl_cancel() or a progress callback ends it with the code
   libcurl would give (28 timed out, 42 aborted) whatever the half-finished read or TLS step reported. */
CURLcode curl_easy_perform(CURL *c) {
    Easy *e = (Easy *)c;
    __atomic_add_fetch(&g_attempts, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_deny, __ATOMIC_SEQ_CST)) {
        __atomic_add_fetch(&g_denied, 1, __ATOMIC_SEQ_CST);
        seterr(e, "Network access refused by the host program (deny mode); nothing was looked up, connected or sent");
        return CURLE_COULDNT_CONNECT;
    }
    e->stop = 0;
    e->dl_now = e->dl_expected = 0;
    e->last_tick_ns = now_ns();
    e->deadline_ns = e->timeout_ms > 0 ? e->last_tick_ns + e->timeout_ms * 1000000LL : 0;
    CURLcode rc = perform_hops(e);
    stop_check(e); /* a stop that arrived while the last bytes were being handled counts only if the transfer had not finished */
    if (rc && e->stop) {
        if (e->stop == 1) {
            rc = CURLE_OPERATION_TIMEDOUT;
            seterr(e, "Operation timed out after %ld milliseconds with %lld bytes received", e->timeout_ms, e->dl_now);
        } else {
            rc = CURLE_ABORTED_BY_CALLBACK;
            seterr(e, e->stop == 2 ? "Transfer cancelled (hv_curl_cancel or the process-wide cancel flag)" : "Operation was aborted by a progress callback");
        }
    } else if (rc == CURLE_ABORTED_BY_CALLBACK && e->errorbuffer && !e->errorbuffer[0]) {
        seterr(e, "Operation was aborted by a callback");
    }
    e->deadline_ns = 0;
    return rc;
}
