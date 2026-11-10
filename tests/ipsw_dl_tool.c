/* Command line around host/ipsw_dl.c. Shipped in the APK as libipswdl.so and started by the app's button; also the program the local tests drive.
   The shipped build (no HV_DL_TEST) is fixed to the macOS 15.6.1 (24G90) IPSW: URL, size, SHA-256 and file name are compiled in, it cannot be pointed
   anywhere else. It downloads only when told how far: --full (to the end, then verify) or --stop-after BYTES (pause there). The app passes --full only after its
   confirmation dialog. A test build (HV_DL_TEST, only tools/ipswdl-test.sh) also takes --url --size --sha256 --name --commit-every --reserve --retries --retry-base-ms.
   Usage: ipsw_dl --dir DIR (--full | --stop-after BYTES) [--avail-cap BYTES] | --status --dir DIR | --delete-partial --dir DIR
   Output lines start with "dl: "; the last one is "dl: RESULT <code> <text>". Exit code = the code (0 done, 2 refused, 3 state, 4 server, 5 interrupted,
   6 hash mismatch, 7 paused, 8 stopped on request). SIGTERM/SIGINT stop the transfer cleanly (state flushed, code 8). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipsw_dl.h"

#define IPSW_URL "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw"
#define IPSW_SIZE 16814137790ULL
/* From ipsw.me only (no second source yet): a match proves the file equals what that site lists, not that the site is right. */
#define IPSW_SHA256 "3d87686b691ac765eb6a6b3082b2334e2af9710096a00432dd519af89ff2ea78"
#define IPSW_NAME "UniversalMac_15.6.1_24G90_Restore.ipsw"

static volatile int g_cancel;
static void on_sig(int s) { (void)s; g_cancel = 1; }

int main(int argc, char **argv) {
    DlCfg c; dl_cfg_default(&c);
    c.url = IPSW_URL; c.expect_size = IPSW_SIZE; c.expect_sha256 = IPSW_SHA256; c.name = IPSW_NAME; c.out = stdout; c.cancel = &g_cancel;
    int status = 0, del = 0, full = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (fprintf(stderr, "dl: RESULT 2 %s needs a value\n", a), exit(2), (char *)0))
        if (!strcmp(a, "--dir")) c.dir = NEXT();
        else if (!strcmp(a, "--stop-after")) c.stop_after = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--avail-cap")) c.avail_cap = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--full")) full = 1;
        else if (!strcmp(a, "--status")) status = 1;
        else if (!strcmp(a, "--delete-partial")) del = 1;
#ifdef HV_DL_TEST
        else if (!strcmp(a, "--url")) c.url = NEXT();
        else if (!strcmp(a, "--size")) c.expect_size = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--sha256")) c.expect_sha256 = NEXT();
        else if (!strcmp(a, "--name")) c.name = NEXT();
        else if (!strcmp(a, "--commit-every")) c.commit_every = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--reserve")) c.reserve = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--retries")) c.max_retries = (unsigned)strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--retry-base-ms")) c.retry_base_ms = (unsigned)strtoul(NEXT(), NULL, 10);
#endif
        else { printf("dl: RESULT 2 unknown argument %s\n", a); return 2; }
    }
    if (!c.dir) { printf("dl: RESULT 2 --dir is required\n"); return 2; }
    char msg[400] = "";
    if (status) { int r = ipsw_dl_status(&c, stdout); printf("dl: RESULT %d status\n", r); return r; }
    if (del) { int r = ipsw_dl_delete_partial(&c, msg, sizeof msg); printf("dl: RESULT %d %s\n", r, msg); return r; }
#ifndef HV_DL_TEST
    if (!full && !c.stop_after) { printf("dl: RESULT 2 nothing to do: say how far to download with --full or --stop-after BYTES. Nothing was sent.\n"); return 2; }
#endif
    if (full && c.stop_after) { printf("dl: RESULT 2 --full and --stop-after exclude each other\n"); return 2; }
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
    int r = ipsw_dl_run(&c, msg, sizeof msg);
    printf("dl: RESULT %d %s\n", r, msg);
    return r;
}
