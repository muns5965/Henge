/* G7: the IPSW SHA-256 comparison and its report text. Pure (no file, no mbedtls): host/g7_runner_real.c hashes the file once and hands the digest here.
   The expected value is what the person typed; the calculated one is the public IPSW's digest (not secret). Nothing else (no ECID, no path) is ever printed. */
#ifndef HV_G7_IPSW_CHECK_H
#define HV_G7_IPSW_CHECK_H
#include <stddef.h>

/* 1 = the digest equals `expected` (64 hex digits, either case), 0 = it does not (or `expected` is not 64 hex digits).
   `out` receives the report: one "IPSW SHA-256: MATCH" line, or MISMATCH with the expected and the calculated 64-digit values (the typed value is shown only if it is 64 hex digits, lower-cased;
   anything else is reported as "not 64 hex digits"). */
int hv_g7_ipsw_compare(const unsigned char digest[32], const char *expected, char *out, size_t cap);

#endif
