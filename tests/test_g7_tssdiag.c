/* host/g7_tssdiag.h: the per-attempt TSS line with synthetic values. Nothing is run, nothing is connected. */
#include <stdio.h>
#include <string.h>
#include "../host/g7_tssdiag.h"

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    char l[700];
    HvG7TssTry t; memset(&t, 0, sizeof t);
    t.attempt = 3; t.elapsed_ms = 4421; t.curl_rc = 7; t.ms = 2; t.addrs_tried = 1; t.conn_errno = 111; t.fds_open = 41; t.fds_limit = 1024;
    hv_g7t_line(l, sizeof l, &t, "TSS_ATTEMPTS: 3 / TRANSPORT: curl 7 COULDNT_CONNECT / MESSAGE_RAW: suppressed");
    CHECK(!strncmp(l, "g7-tss-try: n=3 elapsed_ms=4421 curl=7 http=0 ms=2 ", 51) && l[strlen(l) - 1] == '\n', "one line, starts with the attempt, the time since the request began, curl code, http code and transfer time");
    CHECK(strstr(l, "tried=1 conn=0 tls=0 sent=0 st=0 ") && strstr(l, "connerr=111") && strstr(l, "fds=41/1024"), "connect stage numbers and open descriptors are in the line");
    CHECK(strstr(l, "gai=0 sockerr=0 connerr=111 tlsrc=0") != NULL, "lookup, socket, connect and TLS codes come as numbers");
    CHECK(strstr(l, "diag=TSS_ATTEMPTS: 3 / TRANSPORT: curl 7 COULDNT_CONNECT / MESSAGE_RAW: suppressed\n") != NULL, "libtatsu's fixed-vocabulary diagnosis is appended last");
    { int nl = 0; for (const char *q = l; *q; q++) if (*q == '\n') nl++; CHECK(nl == 1, "exactly one newline"); }
    memset(&t, 0, sizeof t); t.attempt = 1; t.deny = 1; t.policy_denied = 1; t.addrs_denied = 2; t.gai_rc = -2; t.sock_errno = 24; t.tls_rc = -0x2700; t.verify_flags = 0x8; t.url_index = 3; t.fds_open = -1; t.fds_limit = -1;
    hv_g7t_line(l, sizeof l, &t, NULL);
    CHECK(strstr(l, "deny=1 den=2 tried=0") && strstr(l, "gai=-2 sockerr=24") && strstr(l, "tlsrc=-9984") && strstr(l, "policy=1") && strstr(l, "fds=-1/-1"), "deny mode, policy refusals, EMFILE (24) and negative codes are printed as numbers");
    CHECK(strstr(l, "tlsrc=-9984 vflags=0x8 url=3 fds=-1/-1") != NULL, "certificate verify flags (hex) and the built-in address number come right after the TLS code");
    CHECK(l[strlen(l) - 1] == '\n' && strstr(l, " diag=\n") != NULL, "a missing diagnosis gives an empty field, still one line");
    hv_g7t_line(l, sizeof l, &t, "A\nB\x01\xff https://x");
    CHECK(strstr(l, "diag=A?B??") != NULL && !strchr(strchr(l, '\n') + 1, '\n'), "control and non-ASCII characters in the diagnosis become '?', so it cannot add lines");
    { char big[400]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0; hv_g7t_line(l, sizeof l, &t, big);
      const char *d = strstr(l, "diag="); CHECK(d && strlen(d) <= 5 + 199 + 1, "the diagnosis is capped at 199 characters"); }
    { char s[40]; hv_g7t_line(s, sizeof s, &t, "x"); CHECK(strlen(s) < 40, "a small buffer is cut off, never overrun"); }
    { int bad = 0; hv_g7t_line(l, sizeof l, &t, "TSS_ATTEMPTS: 1"); if (strstr(l, "ecid") || strstr(l, "ECID") || strstr(l, "http://") || strstr(l, "gs.apple")) bad = 1;
      CHECK(!bad, "no identifier, URL or host name field exists in the line"); }
    printf(g_fail ? "G7TSSDIAG FAILED (%d)\n" : "G7TSSDIAG OK (%d failed)\n", g_fail);
    return g_fail != 0;
}
