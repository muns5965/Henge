#define _GNU_SOURCE
#include "g7_restore.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disk_policy.h"
#include "g7_preflight.h"

static atomic_int g_stop;
void hv_g7_restore_request_stop(void) { atomic_store(&g_stop, 1); }

const char *hv_g7_restore_outcome_name(HvG7RestoreOutcome o) {
    static const char *const n[] = {"NOT_STARTED", "SUCCESS_SIGNAL", "FAILED", "CANCELLED", "TIMEOUT", "NO_SIGNAL"};
    return (unsigned)o < sizeof n / sizeof *n ? n[o] : "?";
}

HvG7RestoreOutcome hv_g7_restore_run(const HvG7RestoreReq *req, const HvG7RestoreOps *ops, const HvG7RestoreLimits *lim) {
    if (!req || !ops || !ops->start || !ops->poll || !ops->cancel || !ops->sleep_ms || !lim || lim->step_ms < 1 || lim->timeout_ms < 1) return HV_G7R_NOT_STARTED;
    atomic_store(&g_stop, 0);
    if (ops->start(req, ops->ctx) != 0) return HV_G7R_NOT_STARTED;
    long elapsed = 0, cancel_at = -1;
    int timed_out = 0, asked = 0;
    for (;;) {
        int p = ops->poll(ops->ctx);
        if (p == 0) return HV_G7R_SUCCESS_SIGNAL;   /* the tool's own result, even if a stop was asked a moment ago */
        if (p != 1) return asked ? (timed_out ? HV_G7R_TIMEOUT : HV_G7R_CANCELLED) : HV_G7R_FAILED;
        if (!asked && (atomic_load(&g_stop) || elapsed >= lim->timeout_ms)) {
            timed_out = !atomic_load(&g_stop);
            asked = 1; cancel_at = elapsed;
            ops->cancel(ops->ctx);
        }
        if (asked && elapsed - cancel_at >= lim->grace_ms) return HV_G7R_NO_SIGNAL;
        ops->sleep_ms(ops->ctx, lim->step_ms);
        elapsed += lim->step_ms;
    }
}

/* ---------------------------------------------------------------- evidence */
int hv_g7_collect_evidence(const char *dir, HvG7Evidence *ev) {
    memset(ev, 0, sizeof *ev);
    char p[4096];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, HV_INSTALL_DISK_NAME);
    int dfd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return -1;
    if (fstat(dfd, &st) != 0 || !S_ISREG(st.st_mode)) { close(dfd); return -1; }
    ev->disk_allocated = (unsigned long long)st.st_blocks * 512ULL;
    ev->disk_written = ev->disk_allocated >= HV_G7_MIN_WRITTEN_BYTES;
    unsigned char b[8];
    ev->gpt_header = pread(dfd, b, 8, 512) == 8 && !memcmp(b, "EFI PART", 8);
    close(dfd);
    snprintf(p, sizeof p, "%s/%s", dir, HV_G7_AUX_NAME);
    int afd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (afd < 0) return -1;
    unsigned char h[HV_G7_AUX_HEADER_BYTES];
    ev->aux_size_ok = fstat(afd, &st) == 0 && S_ISREG(st.st_mode) && (unsigned long long)st.st_size == HV_G7_AUX_BYTES;
    ev->aux_header = pread(afd, h, sizeof h, 0) == (ssize_t)sizeof h && hv_g7_aux_header_ok(h);
    close(afd);
    return 0;
}

HvG7Conclusion hv_g7_conclude(const char *dir, HvG7RestoreOutcome outcome, char *note, int cap) {
#define NOTE(...) do { if (note && cap > 0) snprintf(note, (size_t)cap, __VA_ARGS__); } while (0)
    if (outcome != HV_G7R_SUCCESS_SIGNAL) { NOTE("not finished: the restore ended as %s; the pair stays in_progress and is kept as it is", hv_g7_restore_outcome_name(outcome)); return HV_G7_CONCLUDED_KEPT; }
    HvG7Evidence ev;
    if (hv_g7_collect_evidence(dir, &ev) != 0) { NOTE("not finished: the disk or the AUX cannot be looked at; the pair stays in_progress"); return HV_G7_CONCLUDED_KEPT; }
    if (!ev.disk_written) { NOTE("not finished: the restore reported success but the disk has only %llu allocated bytes (floor %llu); the pair stays in_progress", ev.disk_allocated, (unsigned long long)HV_G7_MIN_WRITTEN_BYTES); return HV_G7_CONCLUDED_KEPT; }
    if (!ev.gpt_header) { NOTE("not finished: the restore reported success but the disk has no GPT header; the pair stays in_progress"); return HV_G7_CONCLUDED_KEPT; }
    if (!ev.aux_size_ok || !ev.aux_header) { NOTE("not finished: the restore reported success but the AUX is not the expected storage (size or header); the pair stays in_progress"); return HV_G7_CONCLUDED_KEPT; }
    char fn[200];
    if (hv_g7_pair_finish(dir, fn, sizeof fn) != 0) { NOTE("not finished: %s", fn); return HV_G7_CONCLUDED_KEPT; }
    NOTE("finished: success signal, disk written, GPT header and AUX as expected (evidence, not proof that macOS boots)");
    return HV_G7_CONCLUDED_DONE;
#undef NOTE
}
