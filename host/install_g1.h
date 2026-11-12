#ifndef HENGEVM_INSTALL_G1_H
#define HENGEVM_INSTALL_G1_H
/* G1: the Android restore entry point (host/install.c) and the real restore tool run on a PC / in CI, up to and including the signing requests, against a SYNTHETIC
   signing server on 127.0.0.1 (TLS, a test CA) and the stand-in ROM of tests/test_irecv.c. A test build only: tools/install-g1-test.sh compiles host/install.c and
   host/install_g1.c with -DHV_INSTALL_G1; nothing shipped sets it (tools/check-no-test-flags.sh).

   What G1 checks is the tool's own signing path under the session's connection policy: the requests it builds and sends, how each outcome is classified (the tool
   returns 0 only when all three requests of the macOS erase path came back; anything else is -1), that nothing else leaves the process (one exact 127.0.0.1:port is
   approved, everything else stays refused), that no ticket or other file is written, that nothing but ordinary control requests reach the device, and that the
   session and the tool's threads are cleaned up. The tool stops by itself after the third reply (FLAG_SHSHONLY: no root filesystem step, nothing sent to the device).
   This is NOT a restore and no real signing server is involved.

   The session still classifies a return of 0 as "restore succeeded" (final report 1); G1 reads the engine's return value and the session's report separately and
   compares both with what the scenario expects.

   The tool's own log is NOT printed: it carries the device nonce (a hex dump at info level) and, in a real run, the ECID. Only a short list of known-harmless lines
   passes (host/install_g1.c); everything else is counted. */
#include <stdint.h>
#include <stdio.h>

#include "virtio_usb.h"

struct idevicerestore_client_t;

/* host/install.c, G1 build, on the restore thread AFTER the session was begun and taken over (the session's begin empties the connection policy, so approving the
   port any earlier would be undone): FLAG_SHSHONLY, the signing URL forced to the test server, and the one loopback port approved. Returns the flags to add. */
int hv_g1_extra_flags(struct idevicerestore_client_t *c);
/* host/install.c, G1 build: what the restore thread saw, recorded before it publishes the result. */
void hv_g1_record(int engine_rc, int closed, int cleanup_mask, int final_report);

/* What libtatsu's own diagnosis (tss_last_diag, the HTTPS-only build: a fixed vocabulary, never the server's text) must say about the LAST request of the run. */
enum { HV_G1D_UNCHECKED = 0, HV_G1D_SUCCESS, HV_G1D_STATUS, HV_G1D_NOT_REPLY, HV_G1D_TRANSPORT, HV_G1D_EMPTY, HV_G1D_NOT_PLIST };
typedef struct {
    int engine_zero;      /* 1: the tool must return 0 (all three requests answered); 0: it must return non-zero */
    int tss_min, tss_max; /* the number of requests the curl client made (attempts, retries included) */
    int denied_min, denied_max; /* connections the policy refused (0 for every normal scenario; >0 only where a refusal IS the scenario) */
    int diag_kind, diag_val;    /* HV_G1D_*; diag_val: the TSS status (HV_G1D_STATUS) or the curl code (HV_G1D_TRANSPORT), -1 = any. HV_G1D_UNCHECKED only prints what was seen */
} HvG1Expect;

/* The tool's log decision, for the direct test of the filter: the very function the real log hook calls. 1: the line was printed, 0: withheld. Counters: lines shown, withheld, withheld as sensitive, printed unfiltered. */
int hv_g1_filter_line(const char *line);
void hv_g1_log_counts(int *shown, int *withheld, int *sensitive, int *bypassed);

/* Options of the test program (defaults: the port is approved, ticket files stay disabled). */
void hv_g1_set_server(const char *url, int port);   /* the forced signing URL ("https://127.0.0.1:port/path") and the port to approve (0: approve nothing) */
void hv_g1_set_log_bypass(int on);                  /* control: the tool's log is printed unfiltered (synthetic data only); the table then FAILS its tool_log row, by design */
void hv_g1_test_allow_tickets(int on);              /* control: lets the tool write its ticket file (hv_no_ticket_files = 0) so the verdict must catch a real write */

/* Before the run: the work directory snapshot, the thread snapshot, the counters; the log filter on. 0, or -1 with the reason printed. */
int hv_g1_begin(const char *workdir, uint64_t ecid);
/* Waits (60 s) for the guest on the USB bus, then starts the restore through hv_install_start. 0, or -1. */
int hv_g1_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
/* Waits for the end of the run (cap_ms), asks it to stop once if it does not end, waits 20 s more. 1: ended by itself, 0: needed the cancel, -1: did not end. */
int hv_g1_wait(int cap_ms);
/* The verdict table to `out`. Returns the number of FAILED rows (0 = G1 passed). The last line is "install g1 RESULT: g1_passed ..." or "... FAILED ...". */
int hv_g1_finish(FILE *out, const char *workdir, const HvG1Expect *x);

#endif
