/* See net_guard.h. */
#define _GNU_SOURCE
#include "net_guard.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>

#include <libimobiledevice-glue/socket.h>

extern int (*hv_urlasset_guard)(const char *url);                                                     /* restore.c */
extern int (*hv_curl_connect_guard)(const char *host, const struct sockaddr *a, socklen_t len);   /* curl_mbedtls.c */

#define MAXL 8
#define NAMEMAX 128
typedef struct { char ip[64]; uint16_t port; } Local;
typedef struct { char name[NAMEMAX]; uint16_t port; } Host;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Local g_local[MAXL];
static int g_nlocal;
static char g_unix[MAXL][108];
static int g_nunix;
static Host g_host[MAXL];
static int g_nhost;
static HvNetStats g_st;
static char g_log[MAXL][96];
static int g_nlog;
static int g_active;
static int g_engine;   /* engine threads inside hv_net_guard_engine_enter/leave */
#ifdef HV_NET_TEST_LOOPBACK
/* TEST BUILDS ONLY (never compiled into the app; tools/check-no-test-flags.sh checks the shipped objects): one exact 127.0.0.1:port for the HTTP purpose, so a
   fake TSS server on this machine can be reached. Every other loopback port, other private addresses, and every other purpose stay refused. */
static uint16_t g_test_lo_port;
void hv_net_guard_test_allow_loopback(uint16_t port) { pthread_mutex_lock(&g_mu); g_test_lo_port = port; pthread_mutex_unlock(&g_mu); }
#endif

static void note_denied(const char *ip, unsigned port, const char *why) {
    if (g_nlog == MAXL) { memmove(g_log[0], g_log[1], sizeof g_log[0] * (MAXL - 1)); g_nlog--; }
    snprintf(g_log[g_nlog++], sizeof g_log[0], "%s:%u %s", ip, port, why);
}

/* 0: public. Otherwise a short class name. IPv4-mapped IPv6 is judged as the IPv4 address it carries. */
static const char *classify(const struct sockaddr *a, char *ip, size_t ipcap, unsigned *port) {
    ip[0] = 0; *port = 0;
    if (a->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)a;
        inet_ntop(AF_INET, &s->sin_addr, ip, (socklen_t)ipcap);
        *port = ntohs(s->sin_port);
        uint32_t v = ntohl(s->sin_addr.s_addr);
        if ((v >> 24) == 127) return "loopback";
        if (v == 0) return "unspecified";
        if ((v >> 24) == 10 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8) return "private";
        if ((v >> 16) == 0xA9FE) return "link-local";
        if ((v >> 22) == (100u << 2 | 1u)) return "shared-100.64/10";
        if ((v >> 28) == 0xE) return "multicast";
        if (v == 0xFFFFFFFFu) return "broadcast";
        return NULL;
    }
    if (a->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)a;
        inet_ntop(AF_INET6, &s->sin6_addr, ip, (socklen_t)ipcap);
        *port = ntohs(s->sin6_port);
        const uint8_t *b = s->sin6_addr.s6_addr;
        static const uint8_t zero[16];
        if (!memcmp(b, zero, 16)) return "unspecified";
        static const uint8_t lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (!memcmp(b, lo, 16)) return "loopback";
        static const uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF};
        if (!memcmp(b, mapped, 12)) {
            struct sockaddr_in v4 = {0};
            v4.sin_family = AF_INET; memcpy(&v4.sin_addr, b + 12, 4); v4.sin_port = s->sin6_port;
            char tmp[64]; unsigned p;
            const char *c = classify((struct sockaddr *)&v4, tmp, sizeof tmp, &p);
            return c ? c : NULL;
        }
        if ((b[0] & 0xFE) == 0xFC) return "unique-local";
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return "link-local";
        if (b[0] == 0xFF) return "multicast";
        return NULL;
    }
    snprintf(ip, ipcap, "family-%d", a->sa_family);
    return "unknown-family";
}

static void setwhy(char *why, size_t cap, const char *s) { if (why && cap) snprintf(why, cap, "%s", s); }

int hv_net_guard_check_p(const struct sockaddr *a, socklen_t len, const char *name, int purpose, char *why, size_t cap) {
    char ip[64] = "-", reason[64] = "";
    unsigned port = 0;
    int verdict = HV_NET_DENY;
    /* the local exceptions belong to the library's own link to the private usbmuxd; an address the DEVICE chose (FDR) or a web request never uses them */
    const int local_ok = (purpose == HV_NET_P_DEFAULT);
    pthread_mutex_lock(&g_mu);
    g_st.attempts++;
    /* length first: the family field, then the size that family needs, before anything else of the structure is read */
    if (!a || len < (socklen_t)(offsetof(struct sockaddr, sa_family) + sizeof a->sa_family)) snprintf(reason, sizeof reason, "address too short");
    else if (a->sa_family == AF_INET && len < sizeof(struct sockaddr_in)) snprintf(reason, sizeof reason, "address too short");
    else if (a->sa_family == AF_INET6 && len < sizeof(struct sockaddr_in6)) snprintf(reason, sizeof reason, "address too short");
    else if (a->sa_family == AF_UNIX) {
        const struct sockaddr_un *u = (const struct sockaddr_un *)a;
        char path[sizeof u->sun_path + 1] = "";
        size_t have = len > offsetof(struct sockaddr_un, sun_path) ? (size_t)len - offsetof(struct sockaddr_un, sun_path) : 0;
        if (have > sizeof u->sun_path) have = sizeof u->sun_path;
        memcpy(path, u->sun_path, have);   /* bounded by what the caller says it passed; path[] ends in 0 */
        snprintf(ip, sizeof ip, "unix");
        for (int i = 0; i < g_nunix && local_ok; i++)
            if (!strcmp(g_unix[i], path)) { verdict = HV_NET_ALLOW; snprintf(reason, sizeof reason, "registered unix path"); }
        if (verdict == HV_NET_DENY) snprintf(reason, sizeof reason, local_ok ? "unix path not registered" : "unix path refused for this purpose");
    } else {
        const char *cls = classify(a, ip, sizeof ip, &port);
        for (int i = 0; i < g_nlocal && verdict == HV_NET_DENY && local_ok; i++)
            if (g_local[i].port == port && !strcmp(g_local[i].ip, ip)) { verdict = HV_NET_ALLOW; snprintf(reason, sizeof reason, "registered local endpoint"); }
#ifdef HV_NET_TEST_LOOPBACK
        if (verdict == HV_NET_DENY && purpose == HV_NET_P_HTTP && g_test_lo_port && port == g_test_lo_port && a->sa_family == AF_INET &&
            ntohl(((const struct sockaddr_in *)a)->sin_addr.s_addr) == 0x7F000001u) { verdict = HV_NET_ALLOW; snprintf(reason, sizeof reason, "test loopback endpoint"); }
#endif
        if (verdict == HV_NET_DENY) {
            if (cls) snprintf(reason, sizeof reason, "%s address", cls);
            else if (!name) snprintf(reason, sizeof reason, "public address without an approved name");
            else {
                for (int i = 0; i < g_nhost && verdict == HV_NET_DENY; i++)
                    if (g_host[i].port == port && !strcasecmp(g_host[i].name, name)) { verdict = HV_NET_ALLOW; snprintf(reason, sizeof reason, "approved host"); }
                if (verdict == HV_NET_DENY) snprintf(reason, sizeof reason, "host not on the allow list");
            }
        }
    }
    if (verdict == HV_NET_ALLOW) g_st.allowed++;
    else { g_st.denied++; note_denied(ip, port, reason); }
    pthread_mutex_unlock(&g_mu);
    setwhy(why, cap, reason);
    return verdict;
}
int hv_net_guard_check(const struct sockaddr *a, socklen_t len, const char *name, char *why, size_t cap) {
    return hv_net_guard_check_p(a, len, name, HV_NET_P_DEFAULT, why, cap);
}

int hv_net_guard_check_url(const char *url, char *why, size_t cap) {
    pthread_mutex_lock(&g_mu);
    g_st.url_attempts++;
    pthread_mutex_unlock(&g_mu);
    const char *reason = NULL;
    char host[NAMEMAX] = "";
    unsigned port = 443;
    if (!url || strncasecmp(url, "https://", 8)) reason = "not an https URL";
    else {
        const char *p = url + 8, *e = p;
        while (*e && *e != '/' && *e != '?' && *e != '#') e++;
        size_t n = (size_t)(e - p);
        const char *at = memchr(p, '@', n);
        if (at) reason = "URL carries user info";
        else {
            const char *colon = memchr(p, ':', n);
            size_t hn = colon ? (size_t)(colon - p) : n;
            if (!hn || hn >= sizeof host || (p[0] == '[')) reason = "unusable host";
            else {
                memcpy(host, p, hn); host[hn] = 0;
                if (colon) { char *end; long v = strtol(colon + 1, &end, 10); if (end == colon + 1 || end != p + n || v < 1 || v > 65535) reason = "bad port"; else port = (unsigned)v; }
            }
        }
    }
    int verdict = HV_NET_DENY;
    char line[96];
    pthread_mutex_lock(&g_mu);
    if (!reason) {
        for (int i = 0; i < g_nhost && verdict == HV_NET_DENY; i++)
            if (g_host[i].port == port && !strcasecmp(g_host[i].name, host)) verdict = HV_NET_ALLOW;
        if (verdict == HV_NET_DENY) reason = "host not on the allow list";
    }
    if (verdict == HV_NET_DENY) {
        g_st.url_denied++;
        /* the host name and port only: no path, query or user info */
        snprintf(line, sizeof line, "%s", reason);
        note_denied(host[0] ? host : "-", port, line);
    }
    pthread_mutex_unlock(&g_mu);
    setwhy(why, cap, verdict == HV_NET_ALLOW ? "approved host" : reason);
    return verdict;
}

void hv_net_guard_reset(void) {
    pthread_mutex_lock(&g_mu);
    g_nlocal = g_nunix = g_nhost = g_nlog = 0;
#ifdef HV_NET_TEST_LOOPBACK
    g_test_lo_port = 0;
#endif
    memset(&g_st, 0, sizeof g_st);
    pthread_mutex_unlock(&g_mu);
}
void hv_net_guard_allow_local(const char *ip, uint16_t port) {
    pthread_mutex_lock(&g_mu);
    if (g_nlocal < MAXL) { snprintf(g_local[g_nlocal].ip, sizeof g_local[0].ip, "%s", ip); g_local[g_nlocal++].port = port; }
    pthread_mutex_unlock(&g_mu);
}
void hv_net_guard_allow_unix(const char *path) {
    pthread_mutex_lock(&g_mu);
    if (g_nunix < MAXL) snprintf(g_unix[g_nunix++], sizeof g_unix[0], "%s", path);
    pthread_mutex_unlock(&g_mu);
}
void hv_net_guard_allow_host(const char *name, uint16_t port) {
    pthread_mutex_lock(&g_mu);
    if (g_nhost < MAXL) { snprintf(g_host[g_nhost].name, NAMEMAX, "%s", name); g_host[g_nhost++].port = port; }
    pthread_mutex_unlock(&g_mu);
}
void hv_net_guard_stats(HvNetStats *s) { pthread_mutex_lock(&g_mu); *s = g_st; pthread_mutex_unlock(&g_mu); }
size_t hv_net_guard_denied_log(char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nlog; i++) n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s\n", g_log[i]);
    pthread_mutex_unlock(&g_mu);
    return n;
}

static int glue_guard(const struct sockaddr *a, socklen_t len) {
    return hv_net_guard_check_p(a, len, NULL, limd_glue_connect_purpose == LIMD_GLUE_PURPOSE_FDR ? HV_NET_P_FDR : HV_NET_P_DEFAULT, NULL, 0);
}
static int curl_guard(const char *host, const struct sockaddr *a, socklen_t len) { return hv_net_guard_check_p(a, len, host, HV_NET_P_HTTP, NULL, 0); }
static int url_guard(const char *url) { return hv_net_guard_check_url(url, NULL, 0); }

void hv_net_guard_install(void) {
    pthread_mutex_lock(&g_mu);
    limd_glue_connect_guard = glue_guard;
    hv_curl_connect_guard = curl_guard;
    hv_urlasset_guard = url_guard;
    g_active = 1;
    pthread_mutex_unlock(&g_mu);
}
/* Taking the hooks away turns "refuse" into "allow", so it is refused while any engine thread is registered (hv_net_guard_engine_enter). */
int hv_net_guard_uninstall(void) {
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    if (g_engine > 0) rc = -1;
    else {
        limd_glue_connect_guard = NULL;
        hv_curl_connect_guard = NULL;
        hv_urlasset_guard = NULL;
        g_active = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}
int hv_net_guard_active(void) {
    pthread_mutex_lock(&g_mu);
    int r = g_active;
    pthread_mutex_unlock(&g_mu);
    return r;
}
/* All three hooks are really ours (not NULL, not replaced). */
int hv_net_guard_hooks_intact(void) {
    pthread_mutex_lock(&g_mu);
    int r = g_active && limd_glue_connect_guard == glue_guard && hv_curl_connect_guard == curl_guard && hv_urlasset_guard == url_guard;
    pthread_mutex_unlock(&g_mu);
    return r;
}
int hv_net_guard_engine_enter(void) {
    pthread_mutex_lock(&g_mu);
    int ok = g_active && limd_glue_connect_guard == glue_guard && hv_curl_connect_guard == curl_guard && hv_urlasset_guard == url_guard;
    if (ok) g_engine++;
    pthread_mutex_unlock(&g_mu);
    return ok ? 0 : -1;
}
void hv_net_guard_engine_leave(void) {
    pthread_mutex_lock(&g_mu);
    if (g_engine > 0) g_engine--;
    pthread_mutex_unlock(&g_mu);
}
int hv_net_guard_engine_count(void) {
    pthread_mutex_lock(&g_mu);
    int r = g_engine;
    pthread_mutex_unlock(&g_mu);
    return r;
}
