/* hv_g7_ipsw_compare: match, mismatch with both digests printed, bad expected values, case, nothing else printed. */
#include "../host/g7_ipsw_check.h"
#include <stdio.h>
#include <string.h>
static int F = 0;
#define CK(n, c) do { if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); F++; } } while (0)
int main(void) {
    static const char REF[] = "3d87686b691ac765eb6a6b3082b2334e2af9710096a00432dd519af89ff2ea78";   /* the public macOS 15.6.1 reference value, used here only as test data */
    unsigned char d[32];
    for (int i = 0; i < 32; i++) { unsigned v; sscanf(REF + 2 * i, "%2x", &v); d[i] = (unsigned char)v; }
    char calc[65]; static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { calc[2*i] = H[d[i] >> 4]; calc[2*i+1] = H[d[i] & 15]; }
    calc[64] = 0;
    CK("test data round-trips", strcmp(calc, REF) == 0);
    char out[512], up[65];
    CK("match -> 1 and a MATCH line", hv_g7_ipsw_compare(d, calc, out, sizeof out) == 1 && strstr(out, "MATCH") && !strstr(out, "MISMATCH"));
    for (int i = 0; i < 64; i++) { up[i] = (calc[i] >= 'a' && calc[i] <= 'f') ? (char)(calc[i] - 32) : calc[i]; }
    up[64] = 0;
    CK("upper-case expected also matches", hv_g7_ipsw_compare(d, up, out, sizeof out) == 1);
    char other[65]; memcpy(other, calc, 65); other[63] = (other[63] == '0') ? '1' : '0';
    CK("one digit different -> 0, MISMATCH, both full 64-digit values shown", hv_g7_ipsw_compare(d, other, out, sizeof out) == 0 && strstr(out, "MISMATCH") && strstr(out, other) && strstr(out, calc));
    CK("too short -> 0 and says not 64 hex digits, the typed text is not echoed", hv_g7_ipsw_compare(d, "abc", out, sizeof out) == 0 && strstr(out, "not 64 hex digits") && !strstr(out, "abc") && strstr(out, calc));
    char bad[65]; memcpy(bad, calc, 65); bad[10] = 'g';
    CK("non-hex digit -> 0, not echoed", hv_g7_ipsw_compare(d, bad, out, sizeof out) == 0 && strstr(out, "not 64 hex digits") && !strstr(out, bad));
    CK("NULL expected -> 0, no crash", hv_g7_ipsw_compare(d, NULL, out, sizeof out) == 0);
    CK("a small buffer is cut, never overrun, still NUL-terminated", hv_g7_ipsw_compare(d, other, out, 20) == 0 && strlen(out) < 20);
    CK("no path, no ECID words in the report", hv_g7_ipsw_compare(d, other, out, sizeof out) == 0 && !strstr(out, "/") && !strstr(out, "ecid") && !strstr(out, "ECID"));
    printf("failures: %d\n", F);
    return F != 0;
}
