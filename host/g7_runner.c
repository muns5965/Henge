/* The G7 runner: the program that, in its EXECUTION build only, makes the installation pair, creates the writable machine and runs the restore. Two builds from this one source:
     default (tools/g7-runner-build.sh default): NO execution stage. Links the preflight and the install-disk policy and nothing of the machine, the restore tool, the network or
       disk I/O; a valid request ends at the switch (exit 3). The same binary-level static check as g7_probe (tools/check-g7-probe-safe.sh) holds. This is what could be packaged.
     real (tools/g7-runner-build.sh real, -DHV_G7_REAL_EXECUTION): host/g7_runner_real.c supplies the execution table (the real disk creator, hv_vm_create, the restore). It is built
       by CI for the link check only; no APK step packages it, and running it makes the Apple signing requests and writes the disk: that is a separately approved step.
   The ECID is NOT an option: it is read from vm_ecid.txt (--ecid-dir) once, held for the whole run, never printed, never created here.
   usage: g7_runner --dir D --ipsw-dir D --ecid-dir D --rom FILE [--workdir D] [--ipsw-sha256 HEX] [--ram-mb N] [--cpus N] [--jit-mb N] [--timeout-min N] [--grace-s N]
   exit:  0 the pair was finished (real build only) | 2 refused or failed (a started pair is kept in_progress) | 3 valid request, stopped by the switch | 64 usage */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runner_g7_core.h"
#include "g7_restore.h"
#ifdef HV_G7_REAL_EXECUTION
#include "g7_runner_real.h"
#endif

static const char USAGE[] = "usage: g7_runner --dir D --ipsw-dir D --ecid-dir D --rom FILE [--workdir D] [--ipsw-sha256 HEX] [--ram-mb N] [--cpus N] [--jit-mb N] [--timeout-min N] [--grace-s N]\n";
static int usage(const char *why) { fprintf(stderr, "g7_runner: %s\n%s", why, USAGE); return 64; }
static int num(const char *s, unsigned long long *v, int base) {
    char *end = NULL;
    errno = 0;
    if (!s || !*s || *s == '-' || *s == '+' || *s == ' ') return -1;
    *v = strtoull(s, &end, base);
    return (errno || !end || *end) ? -1 : 0;
}

int main(int argc, char **argv) {
    /* stdout is a pipe to the app: without this it is block-buffered, so the last lines of a stalled run (and the log's end) never arrive. Line-buffered, set before the first output.
       Only stdout: the libraries' own stderr lines (usb diag, usbmuxd) are unbuffered already. A line without a newline, or a library writing to fd 1 itself, is not covered. */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) fprintf(stderr, "g7: stdout could not be made line-buffered; the log may lag behind the run\n");
    HvG7Request r = {.ram_mb = 2048, .ncpus = 2, .jit_mb = 64};
    const char *workdir = NULL, *ipsw_sha = NULL;
    long timeout_s = 2 * 3600, grace_s = 60;   /* the provisional defaults of HV_G7R_DEFAULT_LIMITS; only the time limits are settings (bounds in host/g7_restore.h) */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        unsigned long long v;
        if (!strcmp(a, "--dir") && more) r.dir = argv[++i];
        else if (!strcmp(a, "--ipsw-dir") && more) r.ipsw_dir = argv[++i];
        else if (!strcmp(a, "--ecid-dir") && more) r.ecid_dir = argv[++i];
        else if (!strcmp(a, "--rom") && more) r.rom = argv[++i];
        else if (!strcmp(a, "--workdir") && more) workdir = argv[++i];
        else if (!strcmp(a, "--ipsw-sha256") && more) ipsw_sha = argv[++i];
        else if (!strcmp(a, "--ram-mb") && more) { if (num(argv[++i], &v, 10) || v > 1000000) return usage("bad --ram-mb"); r.ram_mb = v; }
        else if (!strcmp(a, "--cpus") && more) { if (num(argv[++i], &v, 10) || v > 1000) return usage("bad --cpus"); r.ncpus = (int)v; }
        else if (!strcmp(a, "--jit-mb") && more) { if (num(argv[++i], &v, 10) || v > 100000) return usage("bad --jit-mb"); r.jit_mb = (size_t)v; }
        else if (!strcmp(a, "--timeout-min") && more) { if (num(argv[++i], &v, 10) || v > 100000) return usage("bad --timeout-min"); timeout_s = (long)v * 60; }
        else if (!strcmp(a, "--grace-s") && more) { if (num(argv[++i], &v, 10) || v > 100000) return usage("bad --grace-s"); grace_s = (long)v; }
        else if (!strcmp(a, "--ecid")) return usage("the ECID is read from vm_ecid.txt, it is not an option");
        else return usage("unknown or incomplete option");
    }
    if (!r.dir || !r.ipsw_dir || !r.ecid_dir || !r.rom) return usage("--dir, --ipsw-dir, --ecid-dir and --rom are required");
    (void)workdir; (void)ipsw_sha;
    HvG7RestoreLimits lim;
    if (hv_g7_limits_make(timeout_s, grace_s, &lim) != 0) return usage("--timeout-min must be 10..720 and --grace-s 10..600");

    /* the ECID, once, from the file the app keeps; the value never leaves this variable except into the machine's configuration */
    HvG7Ecid e = hv_g7_ecid_read(r.ecid_dir, &r.ecid);
    if (e != HV_G7_ECID_MATCH || !r.ecid) {
        printf("G7RUN RESULT: REFUSED stage=0: %s is %s; none is made here\n", HV_G7_ECID_FILE, e == HV_G7_ECID_MISSING ? "missing" : e == HV_G7_ECID_CORRUPT ? "not a valid ecid file" : "not readable");
        return 2;
    }

    HvG7Result res;
    int rc;
#ifdef HV_G7_REAL_EXECUTION
    HvG7Api api;
    if (!workdir || !ipsw_sha) return usage("the execution build needs --workdir and --ipsw-sha256");
    if (g7_real_setup(&r, workdir, ipsw_sha, &api) != 0) { printf("G7RUN RESULT: REFUSED stage=0: the execution table could not be set up\n"); return 2; }
    g7_real_set_limits(&lim);
    rc = hv_g7_run(&r, &api, &res);
    g7_real_end();
#else
    rc = hv_g7_run(&r, NULL, &res);
#endif
    char text[2048];
    hv_g7_report_text(&res.preflight, text, sizeof text);
    printf("%s", text);
    static const char *const VERDICT[] = {"REFUSED", "BLOCKED_BY_SWITCH", "FAILED", "RAN"};
    printf("G7RUN RESULT: %s stage=%d: %s\n", VERDICT[res.verdict], (int)res.stage, res.reason);
    fflush(stdout);
    if (res.verdict == HV_G7_BLOCKED_BY_SWITCH) return 3;
    return (rc == 0 && res.verdict == HV_G7_RAN) ? 0 : 2;
}
