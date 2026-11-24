/* Calls hv_curl_probe (host/curl_mbedtls.c) for one host:port and prints the record as key=value, for tests/test_net_probe.py. */
#include <stdio.h>
#include <stdlib.h>
#include "../host/curl_last.h"
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    HvCurlLast l;
    int rc = hv_curl_probe(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 1, 0, &l);
    printf("curl=%d gai=%d v4=%d v6=%d tried=%d denied=%d conn=%d connerr=%d sockerr=%d tls=%d tlsrc=%d vflags=0x%x sent=%d status=%d dns_ms=%d tcp_ms=%d tls_ms=%d\n",
           rc, l.gai_rc, l.addrs_v4, l.addrs_v6, l.addrs_tried, l.addrs_denied, l.connected, l.conn_errno, l.sock_errno, l.tls_done, l.tls_rc, l.verify_flags, l.request_sent, l.http_status,
           l.dns_ms, l.tcp_ms, l.tls_ms);
    return 0;
}
