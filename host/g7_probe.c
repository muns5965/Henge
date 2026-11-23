/* g7_probe: the G7 request run through host/runner_g7_core.c in a build that has NO execution stage, as a program of its own. It links the preflight and the install-disk policy
   and nothing of the machine, the restore tool, the network or the disk I/O (tools/check-g7-probe-safe.sh checks the symbols of the finished binary).
   It reads the real files it is pointed at and changes none of them. A valid request ends at the switch (exit 3); a refused one says which check refused it (exit 2); this
   program can never exit 0, because it never does what a G7 run is for. The ECID is taken on the command line (as host/ui_runner_vm_installed.c does) and is never printed.
   usage: g7_probe --dir D --ipsw-dir D --ecid-dir D --rom FILE --ecid HEX [--ram-mb N] [--cpus N] [--jit-mb N]
   exit:  3 valid request, stopped by the switch | 2 refused | 64 usage */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runner_g7_core.h"

#ifdef HV_G7_MOCK_EXECUTION
#error "g7_probe must be built without HV_G7_MOCK_EXECUTION"
#endif

static int usage(const char *why) {
    fprintf(stderr, "g7_probe: %s\nusage: g7_probe --dir D --ipsw-dir D --ecid-dir D --rom FILE --ecid HEX [--ram-mb N] [--cpus N] [--jit-mb N]\n", why);
    return 64;
}
static int num(const char *s, unsigned long long *v, int base) {
    char *end = NULL;
    errno = 0;
    if (!s || !*s || *s == '-' || *s == '+' || *s == ' ') return -1;
    *v = strtoull(s, &end, base);
    return (errno || !end || *end) ? -1 : 0;
}

int main(int argc, char **argv) {
    HvG7Request r = {.ram_mb = 2048, .ncpus = 2, .jit_mb = 64};
    int have_ecid = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        unsigned long long v;
        if (!strcmp(a, "--dir") && more) r.dir = argv[++i];
        else if (!strcmp(a, "--ipsw-dir") && more) r.ipsw_dir = argv[++i];
        else if (!strcmp(a, "--ecid-dir") && more) r.ecid_dir = argv[++i];
        else if (!strcmp(a, "--rom") && more) r.rom = argv[++i];
        else if (!strcmp(a, "--ecid") && more) { if (num(argv[++i], &v, 16) || !v) return usage("--ecid must be a non-zero hex number"); r.ecid = v; have_ecid = 1; }
        else if (!strcmp(a, "--ram-mb") && more) { if (num(argv[++i], &v, 10) || v > 1000000) return usage("bad --ram-mb"); r.ram_mb = v; }
        else if (!strcmp(a, "--cpus") && more) { if (num(argv[++i], &v, 10) || v > 1000) return usage("bad --cpus"); r.ncpus = (int)v; }
        else if (!strcmp(a, "--jit-mb") && more) { if (num(argv[++i], &v, 10) || v > 100000) return usage("bad --jit-mb"); r.jit_mb = (size_t)v; }
        else return usage("unknown or incomplete option");
    }
    if (!r.dir || !r.ipsw_dir || !r.ecid_dir || !r.rom || !have_ecid) return usage("--dir, --ipsw-dir, --ecid-dir, --rom and --ecid are required");

    HvG7Result res;
    hv_g7_run(&r, NULL, &res);
    char text[2048];
    hv_g7_report_text(&res.preflight, text, sizeof text);
    printf("%s", text);
    static const char *const VERDICT[] = {"REFUSED", "BLOCKED_BY_SWITCH", "FAILED", "RAN"};
    printf("G7PROBE RESULT: %s stage=%d: %s\n", VERDICT[res.verdict], (int)res.stage, res.reason);
    fflush(stdout);
    return res.verdict == HV_G7_BLOCKED_BY_SWITCH ? 3 : 2;
}
