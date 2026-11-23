#include "g7_ipsw_check.h"
#include <stdio.h>
#include <string.h>

static const char HEXD[] = "0123456789abcdef";

static int norm_expected(const char *in, char out[65]) {
    if (!in || strlen(in) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
        out[i] = c;
    }
    out[64] = 0;
    return 1;
}

int hv_g7_ipsw_compare(const unsigned char digest[32], const char *expected, char *out, size_t cap) {
    char calc[65], exp[65];
    for (int i = 0; i < 32; i++) { calc[2 * i] = HEXD[digest[i] >> 4]; calc[2 * i + 1] = HEXD[digest[i] & 15]; }
    calc[64] = 0;
    int valid = norm_expected(expected, exp);
    int ok = valid && strcmp(exp, calc) == 0;
    if (out && cap) {
        if (ok) snprintf(out, cap, "IPSW SHA-256: MATCH\n");
        else snprintf(out, cap, "IPSW SHA-256: MISMATCH\n  expected:   %s\n  calculated: %s\n", valid ? exp : "(not 64 hex digits)", calc);
    }
    return ok;
}
