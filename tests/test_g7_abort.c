/* The G7 restore adapter and the judgment, ended in four ways by a SEPARATE test process: killed (SIGKILL), terminated (SIGTERM handled like host/g7_runner_real.c), timed out, and the
   tool that never ends (NO_SIGNAL, the runner's _exit(5)). After each, the parent looks at the directory: the pair is still in_progress, nothing was deleted, moved, renamed or reset,
   a second begin on the same directory is refused, the judgment keeps it, and the summary line carries no ECID and no path. Fake operations table: no restore tool, machine or network. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../host/disk.h"
#include "../host/disk_policy.h"
#include "../host/g7_preflight.h"
#include "../host/g7_restore.h"

int hv_knob[HV_K_COUNT];
static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)
#define ECID 0x0013579bdf024681ULL

static char T[200];
static int g_mode;   /* what the child's fake tool does */
static int g_polls;
static void write_like_a_restore(const char *dir) {
    char p[400]; snprintf(p, sizeof p, "%s/%s", dir, HV_INSTALL_DISK_NAME);
    int fd = open(p, O_WRONLY); if (fd < 0) _exit(90);
    static unsigned char blk[1 << 20]; memset(blk, 0x5a, sizeof blk);
    for (int i = 0; i < 4; i++) if (pwrite(fd, blk, sizeof blk, (off_t)i * (4 << 20)) != (ssize_t)sizeof blk) _exit(91);
    (void)!pwrite(fd, "EFI PART", 8, 512);
    close(fd);
}
static const char *g_dir;
static int f_start(const HvG7RestoreReq *r, void *c) { (void)c; (void)r; write_like_a_restore(g_dir); hv_g7_trace_reset(); hv_g7_trace_step(12, 0.37); return 0; }
static int g_cancelled;
static int f_poll(void *c) {
    (void)c; g_polls++;
    if (g_mode == 0 && g_polls == 3) raise(SIGKILL);                 /* the process is killed in the middle of the restore */
    if (g_mode == 1 && g_polls == 3) raise(SIGTERM);                 /* the handler asks the adapter to stop */
    if (g_cancelled && (g_mode == 1 || g_mode == 2)) return -1;       /* after the cancel the tool reports a stop; mode 3 never answers */
    return 1;
}
static void f_cancel(void *c) { (void)c; g_cancelled = 1; }
static void f_sleep(void *c, int ms) { (void)c; (void)ms; }
static void on_term(int s) { (void)s; hv_g7_restore_request_stop(); }

static int child(const char *dir, int mode) {
    g_mode = mode; g_dir = dir;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL);
    char n[200];
    if (hv_g7_pair_begin(dir, ECID, hv_install_disk_create, n, sizeof n) != 0) return 90;
    HvG7RestoreOps ops = {f_start, f_poll, f_cancel, f_sleep, NULL};
    HvG7RestoreReq req = {"x.ipsw", "/tmp", ECID};
    HvG7RestoreLimits lim = {.timeout_ms = 1000, .grace_ms = 400, .step_ms = 100};
    HvG7RestoreOutcome o = hv_g7_restore_run(&req, &ops, &lim);
    return 10 + (int)o;   /* the outcome, for the parent */
}

static unsigned long fnv(const char *dir, const char *name, size_t n) {
    char p[400]; snprintf(p, sizeof p, "%s/%s", dir, name); int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    static unsigned char b[1 << 16]; unsigned long h = 1469598103934665603UL; size_t left = n;
    off_t off = 0;
    while (left) { size_t k = left < sizeof b ? left : sizeof b; ssize_t r = pread(fd, b, k, off); if (r <= 0) break; for (ssize_t i = 0; i < r; i++) h = (h ^ b[i]) * 1099511628211UL; off += r; left -= (size_t)r; }
    close(fd); return h;
}
static unsigned long snapshot(const char *dir, int *entries, off_t *size, long *blocks) {
    unsigned long h = fnv(dir, HV_INSTALL_DISK_NAME, 16 << 20) ^ (fnv(dir, HV_G7_AUX_NAME, 1 << 20) * 31) ^ (fnv(dir, "g7_state.txt", 4096) * 131);
    DIR *d = opendir(dir); int n = 0; struct dirent *e; while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++; if (d) closedir(d);
    char p[400]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, HV_INSTALL_DISK_NAME); if (stat(p, &st) == 0) { *size = st.st_size; *blocks = (long)st.st_blocks; }
    *entries = n; return h;
}

static void one(const char *tag, int mode, const char *what, int expect_outcome, int sigexpect) {
    char dir[260]; snprintf(dir, sizeof dir, "%s/%s", T, tag); mkdir(dir, 0700); chmod(dir, 0700);
    pid_t pid = fork();
    if (pid == 0) _exit(child(dir, mode));
    int st = 0; waitpid(pid, &st, 0);
    if (sigexpect) CHECK(WIFSIGNALED(st) && WTERMSIG(st) == sigexpect, "%s: the test process was ended by signal %d", what, sigexpect);
    else CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 10 + expect_outcome, "%s: outcome %s (exit %d)", what, hv_g7_restore_outcome_name((HvG7RestoreOutcome)expect_outcome), WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    int e0, e1; off_t s0 = 0, s1 = 0; long b0 = 0, b1 = 0;
    unsigned long h0 = snapshot(dir, &e0, &s0, &b0);
    CHECK(e0 == 4, "%s: the directory holds exactly the pair (disk, AUX, AUX copy, state): %d entries", what, e0);
    CHECK(hv_g7_pair_state(dir, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "%s: the pair is still in_progress", what);
    char n[300];
    CHECK(hv_g7_pair_begin(dir, ECID, hv_install_disk_create, n, sizeof n) != 0, "%s: a second begin on the same directory is refused (no auto-resume, no overwrite): %s", what, n);
    CHECK(hv_g7_conclude(dir, (HvG7RestoreOutcome)(sigexpect ? HV_G7R_NOT_STARTED : expect_outcome), n, sizeof n) == HV_G7_CONCLUDED_KEPT, "%s: the judgment keeps it (%s)", what, n);
    CHECK(hv_g7_pair_ecid_check(dir, ECID, NULL, 0) == 0, "%s: the pair is still tied to its ECID", what);
    unsigned long h1 = snapshot(dir, &e1, &s1, &b1);
    CHECK(h0 == h1 && e0 == e1 && s0 == s1 && b0 == b1, "%s: contents, sizes and allocation unchanged by everything done afterwards", what);
    char line[600]; HvG7RestoreLimits lim = {.timeout_ms = 1000, .grace_ms = 400, .step_ms = 100};
    int len = hv_g7_summary(dir, sigexpect ? HV_G7R_NOT_STARTED : (HvG7RestoreOutcome)expect_outcome, 2, &lim, line, sizeof line);
    CHECK(len > 0 && strstr(line, "pair=in_progress") && strstr(line, "refused_destinations=2"), "%s: summary: %s", what, line);
    CHECK(!strstr(line, "13579bdf") && !strstr(line, "/tmp") && !strstr(line, "http") && !strstr(line, T), "%s: the summary has no ECID, path or URL", what);
}

int main(void) {
    snprintf(T, sizeof T, "/tmp/g7a_%d", (int)getpid()); mkdir(T, 0700);
    one("kill", 0, "SIGKILL in the middle", 0, SIGKILL);
    one("term", 1, "SIGTERM (the runner's handler asks the adapter to stop)", HV_G7R_CANCELLED, 0);
    one("timeout", 2, "the time ran out", HV_G7R_TIMEOUT, 0);
    one("nosignal", 3, "the tool never ends (the runner would _exit(5))", HV_G7R_NO_SIGNAL, 0);
    printf(g_fail ? "ABORT FAILED %d\n" : "ABORT ALL PASSED (0 failed)\n", g_fail);
    return g_fail != 0;
}
