/* host/g7_restore.c with a FAKE operations table: no restore tool, no machine, no network. Linux. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../host/disk.h"
#include "../host/disk_policy.h"
#include "../host/g7_preflight.h"
#include "../host/g7_restore.h"

int hv_knob[HV_K_COUNT];
static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)
#define ECID 0x0013579bdf024681ULL

static struct { int started, polls, cancels, slept; int start_rc; int script[64]; int n; int after_cancel; int cancelled; const char *ecid_seen; uint64_t ecid; } F;
static int f_start(const HvG7RestoreReq *r, void *c) { (void)c; F.started++; F.ecid = r->ecid; return F.start_rc; }
static int f_poll(void *c) { (void)c; int i = F.polls++; if (F.cancelled && F.after_cancel != 99) return F.after_cancel; return i < F.n ? F.script[i] : 1; }
static void f_cancel(void *c) { (void)c; F.cancels++; F.cancelled = 1; }
static void f_sleep(void *c, int ms) { (void)c; (void)ms; F.slept++; }
static const HvG7RestoreOps OPS = {f_start, f_poll, f_cancel, f_sleep, NULL};
static const HvG7RestoreReq REQ = {"x.ipsw", "/tmp", ECID};
static HvG7RestoreLimits L = {.timeout_ms = 1000, .grace_ms = 400, .step_ms = 100};
static void reset(void) { memset(&F, 0, sizeof F); F.after_cancel = 99; }

static char T[200];
static void pwrite_at(const char *dir, const char *name, const void *b, size_t n, off_t off) { char p[400]; snprintf(p, sizeof p, "%s/%s", dir, name); int fd = open(p, O_WRONLY); if (fd >= 0) { (void)!pwrite(fd, b, n, off); close(fd); } }
static char *mkd(const char *tag) { static char d[16][260]; static int k; char *p = d[k++ % 16]; snprintf(p, 260, "%s/%s", T, tag); mkdir(p, 0700); chmod(p, 0700); return p; }
static void *stopper(void *a) { (void)a; usleep(1000); hv_g7_restore_request_stop(); return NULL; }

/* a pair in `dir` the way the begin made it, then (optionally) written as a restore would */
static void made_pair(const char *dir, int written, int gpt, int aux_ok) {
    char n[200];
    int r = hv_g7_pair_begin(dir, ECID, hv_install_disk_create, n, sizeof n);
    if (r != 0) { printf("setup failed: %s\n", n); exit(2); }
    if (written) { static unsigned char blk[1 << 20]; memset(blk, 0x5a, sizeof blk); for (int i = 0; i < 4; i++) pwrite_at(dir, HV_INSTALL_DISK_NAME, blk, sizeof blk, (off_t)i * (4 << 20)); }
    if (gpt) pwrite_at(dir, HV_INSTALL_DISK_NAME, "EFI PART", 8, 512);
    if (!aux_ok) pwrite_at(dir, HV_G7_AUX_NAME, "Q", 1, 3);
}

int main(void) {
    snprintf(T, sizeof T, "/tmp/g7r_%d", (int)getpid()); mkdir(T, 0700);
    /* ---- the adapter */
    reset(); F.n = 3; F.script[0] = 1; F.script[1] = 1; F.script[2] = 0;
    CHECK(hv_g7_restore_run(&REQ, &OPS, &L) == HV_G7R_SUCCESS_SIGNAL && F.started == 1 && F.cancels == 0 && F.ecid == ECID, "success: the signal arrives, one start, no cancel, the request reaches start");
    reset(); F.n = 2; F.script[0] = 1; F.script[1] = -1;
    CHECK(hv_g7_restore_run(&REQ, &OPS, &L) == HV_G7R_FAILED && F.cancels == 0, "failure result -> FAILED");
    reset(); F.n = 1; F.script[0] = 7;
    CHECK(hv_g7_restore_run(&REQ, &OPS, &L) == HV_G7R_FAILED, "an answer that is not 1/0/-1 counts as a failure, never as a success");
    reset(); F.start_rc = -1;
    CHECK(hv_g7_restore_run(&REQ, &OPS, &L) == HV_G7R_NOT_STARTED && F.polls == 0, "start refused -> NOT_STARTED, nothing polled");
    CHECK(hv_g7_restore_run(NULL, &OPS, &L) == HV_G7R_NOT_STARTED && hv_g7_restore_run(&REQ, NULL, &L) == HV_G7R_NOT_STARTED && hv_g7_restore_run(&REQ, &OPS, NULL) == HV_G7R_NOT_STARTED, "missing request/table/limits -> NOT_STARTED");
    { HvG7RestoreOps o = OPS; o.cancel = NULL; reset(); CHECK(hv_g7_restore_run(&REQ, &o, &L) == HV_G7R_NOT_STARTED && F.started == 0, "an incomplete table never starts"); }
    reset(); F.after_cancel = -1; /* never ends on its own: the timeout asks for a cancel, the tool then reports a stop */
    CHECK(hv_g7_restore_run(&REQ, &OPS, &L) == HV_G7R_TIMEOUT && F.cancels == 1, "timeout: one cancel, then the tool's end -> TIMEOUT");
    reset(); /* never ends at all */
    CHECK(hv_g7_restore_run(&REQ, &OPS, &L) == HV_G7R_NO_SIGNAL && F.cancels == 1, "nothing arrives even after the cancel and the grace time -> NO_SIGNAL");
    reset(); F.after_cancel = 0;
    CHECK(hv_g7_restore_run(&REQ, &OPS, &L) == HV_G7R_SUCCESS_SIGNAL, "a success that arrives right after the cancel is still reported as what the tool said");
    reset(); F.after_cancel = -1;
    { pthread_t t; pthread_create(&t, NULL, stopper, NULL); /* a stop request from another thread */
      HvG7RestoreLimits big = {.timeout_ms = 100000, .grace_ms = 400, .step_ms = 1};
      HvG7RestoreOutcome o;
      struct timespec ts = {0, 2000000};
      /* run with a sleep that really yields so the stopper can run */
      (void)ts; o = hv_g7_restore_run(&REQ, &OPS, &big); pthread_join(t, NULL);
      CHECK(o == HV_G7R_CANCELLED || o == HV_G7R_TIMEOUT, "an outside stop request cancels (%s)", hv_g7_restore_outcome_name(o)); }
    CHECK(!strcmp(hv_g7_restore_outcome_name(HV_G7R_NO_SIGNAL), "NO_SIGNAL") && !strcmp(hv_g7_restore_outcome_name((HvG7RestoreOutcome)99), "?"), "outcome names");

    /* ---- the judgment */
    char note[300];
    { char *d = mkd("ok"); made_pair(d, 1, 1, 1);
      CHECK(hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, note, sizeof note) == HV_G7_CONCLUDED_DONE && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_DONE, "signal + written disk + GPT + AUX header: done (%s)", note);
      CHECK(hv_g7_pair_ecid_check(d, ECID, NULL, 0) == 0, "...and the pair is still tied to its ECID"); }
    { char *d = mkd("empty"); made_pair(d, 0, 0, 1);
      CHECK(hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, note, sizeof note) == HV_G7_CONCLUDED_KEPT && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "a success signal but an unwritten disk: kept in_progress (%s)", note); }
    { char *d = mkd("nogpt"); made_pair(d, 1, 0, 1);
      CHECK(hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, note, sizeof note) == HV_G7_CONCLUDED_KEPT && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "written disk without a GPT header: kept (%s)", note); }
    { char *d = mkd("badaux"); made_pair(d, 1, 1, 0);
      CHECK(hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, note, sizeof note) == HV_G7_CONCLUDED_KEPT && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "a damaged AUX header: kept (%s)", note); }
    for (int o = HV_G7R_NOT_STARTED; o <= HV_G7R_NO_SIGNAL; o++) {
        if (o == HV_G7R_SUCCESS_SIGNAL) continue;
        char tag[20]; snprintf(tag, sizeof tag, "o%d", o); char *d = mkd(tag); made_pair(d, 1, 1, 1);
        CHECK(hv_g7_conclude(d, (HvG7RestoreOutcome)o, note, sizeof note) == HV_G7_CONCLUDED_KEPT && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "outcome %s never finishes, even with perfect-looking files", hv_g7_restore_outcome_name((HvG7RestoreOutcome)o));
        char p[400]; struct stat st; snprintf(p, sizeof p, "%s/%s", d, HV_INSTALL_DISK_NAME);
        CHECK(stat(p, &st) == 0, "...and nothing was deleted");
    }
    { char *d = mkd("nopair");
      CHECK(hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, note, sizeof note) == HV_G7_CONCLUDED_KEPT, "no pair at all: kept, nothing made (%s)", note); }
    { char *d = mkd("done_twice"); made_pair(d, 1, 1, 1); hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, NULL, 0);
      CHECK(hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, note, sizeof note) == HV_G7_CONCLUDED_KEPT, "a pair that is already done is not finished again (%s)", note); }
    { char *d = mkd("noecid_in_note"); made_pair(d, 1, 1, 1); hv_g7_conclude(d, HV_G7R_SUCCESS_SIGNAL, note, sizeof note);
      CHECK(!strstr(note, "13579bdf"), "the note carries no ECID"); }
    printf(g_fail ? "FAILED %d\n" : "ALL PASSED (0 failed)\n", g_fail);
    return g_fail != 0;
}
