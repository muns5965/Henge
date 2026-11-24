/* Network probe (a separate small program, nothing to do with the VM, the disk or the restore tool).
   It exercises the SAME connection code the G7 restore uses for its signing requests (host/curl_mbedtls.c: name lookup, TCP connect, TLS handshake with the same trust
   anchors and the same certificate and host-name checks) against the three built-in TSS addresses, and only that far: it opens a connection, ends the handshake and closes it.
   No request is written, so no signing request, no identifier and no ticket exists here. Hard limits, not options: the three addresses below, port 443, https, at most
   NP_ROUNDS rounds one per NP_INTERVAL_S seconds, at most NP_ROUNDS*3 connections in all. The order of the addresses is rotated every round, so that a change over time is not
   mistaken for a property of one address. SIGTERM/SIGINT end the run at the next second. Output: one line per connection, numbers and fixed words only, then a summary. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include "curl_last.h"

#define NP_ROUNDS 10
#define NP_INTERVAL_S 30
#define NP_LIMIT_MS 15000

static const char *const HOSTS[3] = {"gs.apple.com", "gs.apple.com.akadns.net", "gs.apple.com.v.aaplimg.com"};
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d)) n++;
    closedir(d);
    return n - 3;   /* ".", ".." and the directory's own descriptor */
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    printf("net-probe-start: addresses 1=%s 2=%s 3=%s port=443 https handshake-only rounds=%d interval=%ds limit=%dms\n", HOSTS[0], HOSTS[1], HOSTS[2], NP_ROUNDS, NP_INTERVAL_S, NP_LIMIT_MS);
    int ok[3] = {0, 0, 0}, dnsfail[3] = {0, 0, 0}, tcpfail[3] = {0, 0, 0}, tlsfail[3] = {0, 0, 0}, other[3] = {0, 0, 0}, n = 0;
    for (int round = 1; round <= NP_ROUNDS && !g_stop; round++) {
        for (int k = 0; k < 3 && !g_stop; k++) {
            int h = (round - 1 + k) % 3;   /* rotation */
            struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a);
            HvCurlLast l;
            int rc = hv_curl_probe(HOSTS[h], "443", 1, NP_LIMIT_MS, &l);
            struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
            long total_ms = (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
            struct rlimit rl; int lim = getrlimit(RLIMIT_NOFILE, &rl) == 0 ? (rl.rlim_cur == RLIM_INFINITY ? 0 : (int)rl.rlim_cur) : -1;
            printf("net-probe: t=%lds round=%d url=%d curl=%d total_ms=%ld dns_ms=%d gai=%d v4=%d v6=%d tcp_ms=%d tried=%d conn=%d connerr=%d sockerr=%d tls_ms=%d tls=%d tlsrc=%d vflags=0x%x fds=%d/%d\n",
                   (long)(a.tv_sec - t0.tv_sec), round, h + 1, rc, total_ms, l.dns_ms, l.gai_rc, l.addrs_v4, l.addrs_v6, l.tcp_ms, l.addrs_tried, l.connected, l.conn_errno, l.sock_errno,
                   l.tls_ms, l.tls_done, l.tls_rc, l.verify_flags, open_fds(), lim);
            n++;
            if (rc == 0 && l.tls_done) ok[h]++;
            else if (l.gai_rc) dnsfail[h]++;
            else if (!l.connected) tcpfail[h]++;
            else if (!l.tls_done) tlsfail[h]++;
            else other[h]++;
        }
        for (int s = 0; round < NP_ROUNDS && s < NP_INTERVAL_S && !g_stop; s++) sleep(1);
    }
    for (int h = 0; h < 3; h++)
        printf("net-probe-summary: url=%d ok=%d dns_fail=%d tcp_fail=%d tls_fail=%d other=%d\n", h + 1, ok[h], dnsfail[h], tcpfail[h], tlsfail[h], other[h]);
    printf("net-probe-end: connections=%d%s\n", n, g_stop ? " stopped=yes" : "");
    return 0;
}
