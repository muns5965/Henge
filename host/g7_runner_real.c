/* THE EXECUTION TABLE OF THE G7 RUNNER. Compiled only by tools/g7-runner-build.sh real (-DHV_G7_REAL_EXECUTION); never by an APK step, never by the default build, never by a test.
   Running a binary built from this makes the Apple signing requests the restore needs and WRITES the install disk and the AUX: that is a separately approved step, and several
   conditions are still open (see the end of this comment). What each member does:
     pair_begin    hv_g7_pair_begin with the real disk creator (host/disk_policy.c): state file (with the ECID tag), the 128 GiB sparse disk, the AUX, the AUX copy
     create_vm     hv_vm_create with the configuration hv_g7_config_ok accepted (installer, writable storage, the ECID of the file), then hv_vm_start
     start_restore the IPSW identity is re-checked, then hv_g7_restore_run (the restore tool through hv_install_*) and, after the machine's disks are flushed and it is stopped,
                   hv_g7_conclude, the only caller of hv_g7_pair_finish. It returns 0 only for a finished pair; every other end keeps the pair in_progress, deleting and moving nothing.
   Open conditions, all NOT done: (1) the net allow-list (host/install_g7.c) holds only the three TSS hosts G2-G6 use, port 443; the URLAsset, image-key and FDR destinations are chosen by the
   device and unknown, so they are refused and the refusals are printed below (host name and port, no URL) for a separately approved step to read; approving them is NOT done; (2) a launcher and its approval (see docs/g7-new-destinations.md for how a new host is approved); (3) the judgment's file checks
   (GPT header, allocated size, AUX header) have never been compared with a real restore; (4) timing: the default limits and their bounds (--timeout-min 10..720, --grace-s 10..600) are guesses; the end of every run prints one "g7-result:" line (outcome, last step, pair state, refusals; no ECID/URL/path); (5) what a full restore does after the restore
   options (restored_start_restore and later) has never run on this engine. */
#define _GNU_SOURCE
#include "g7_runner_real.h"
#include "net_guard.h"
#include "g7_watch.h"
#include "g7_mem.h"
#include "g7_tssdiag.h"
#include "curl_last.h"
#include <dirent.h>
#include <sys/resource.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disk_policy.h"
#include "g7_preflight.h"
#include "g7_restore.h"
#include "install.h"
#include "mbedtls/sha256.h"
#include "g7_ipsw_check.h"
#include "usb_host.h"
#include "vm.h"
#include "dispatch.h"
#include "virtio_blk.h"
#include "bdif.h"
#include "../devices/vmapple.h"

static struct {
    char dir[4096], workdir[4096], ipsw[4096];
    uint64_t ecid;
    struct stat ipsw_id;   /* the identity of the file that was hashed */
    int set;
    HvG7RestoreLimits lim;
} S;

/* 1 = hash equals `hex`, 0 = it does not, -1 = the file cannot be read. The report (MATCH, or MISMATCH with the expected and the calculated digest) is printed here, from the digest that is computed ONCE. */
static int ipsw_hash_ok(const char *path, const char *hex, struct stat *id) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, id) != 0 || !S_ISREG(id->st_mode)) { close(fd); return -1; }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    static unsigned char buf[1 << 20];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) mbedtls_sha256_update(&c, buf, (size_t)n);
    unsigned char d[32];
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
    close(fd);
    if (n != 0) return -1;
    char rep[256];
    int ok = hv_g7_ipsw_compare(d, hex, rep, sizeof rep);
    fputs(rep, stdout);
    return ok;
}

static int ipsw_same_file(void) {
    struct stat st;
    if (lstat(S.ipsw, &st) != 0) return 0;
    return st.st_dev == S.ipsw_id.st_dev && st.st_ino == S.ipsw_id.st_ino && st.st_size == S.ipsw_id.st_size && st.st_mtime == S.ipsw_id.st_mtime;
}

/* ---- the table */
static int real_pair(const char *dir, uint64_t ecid, char *note, int cap) { return hv_g7_pair_begin(dir, ecid, hv_install_disk_create, note, cap); }

static void on_machine_exit(int reason) { (void)reason; }

static int real_create_vm(const HvVmConfig *cfg, char *err, size_t errlen) {
    if (hv_vm_create(cfg, err, errlen) != 0) return -1;
    hv_vm_start(1, on_machine_exit);
    if (hv_vm_start_failed()) { if (err && errlen) snprintf(err, errlen, "the machine could not start all its threads"); return -1; }
    return 0;
}

static void on_progress(int step, double p, void *u) { (void)u; hv_g7_trace_step(step, p); }

static int op_start(const HvG7RestoreReq *q, void *ctx) {
    (void)ctx;
    HvVmapple *b = hv_vm_board();
    if (!b || !b->vusb) return -1;
    if (hv_usb_wait_online(b->vusb, 60000) < 0) return -1;   /* the guest did not appear on the USB bus: the restore is not started */
    return hv_install_start(b->vusb, q->ipsw, q->workdir, q->ecid, on_progress, NULL);
}
static int op_poll(void *ctx) { (void)ctx; return hv_install_result(); }
static void op_cancel(void *ctx) { (void)ctx; hv_install_cancel(); }
static void op_sleep(void *ctx, int ms) { (void)ctx; usleep((useconds_t)ms * 1000u); }
static void on_term(int sig) { (void)sig; hv_g7_restore_request_stop(); }

/* Watchdog (host/g7_watch.h): every HV_G7W_TICK_S seconds one `g7-alive:` line, or `g7-stall:` after two quiet samples, built from read-only counters. It reports only; it never
   touches the restore, the VM or the disk. Started right before the restore runs, stopped and joined right after. */
static int g_watch_stop;   /* __atomic */
static void *watch_main(void *arg) {
    const char *dir_copy = (const char *)arg;   /* the directory that holds the install disk (the pair directory) */
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    HvG7WatchState st; memset(&st, 0, sizeof st);
    struct timespec last = t0;   /* the last sample: the period is measured on CLOCK_MONOTONIC, not counted in sleeps (a sleep that comes back late must not stretch the period) */
    while (!__atomic_load_n(&g_watch_stop, __ATOMIC_RELAXED)) {
        usleep(1000 * 1000);
        if (__atomic_load_n(&g_watch_stop, __ATOMIC_RELAXED)) break;
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((uint64_t)(t1.tv_sec - last.tv_sec) < HV_G7W_TICK_S) continue;
        last = t1;
        HvG7WatchSample s; memset(&s, 0, sizeof s);
        s.t_s = (uint64_t)(t1.tv_sec - t0.tv_sec);
        s.blocks[0] = hv_diag_blocks(0); s.blocks[1] = hv_diag_blocks(1);
        uint64_t age_us = 0; hv_vblk_diag(&s.disk_done, &s.disk_inflight, &age_us, &s.disk_last_type);
        s.disk_last_age_s = age_us / 1000000u;
        s.usb_cmds = hv_bdif_usb_cmds();
        char line[400];
        hv_g7w_step(&st, &s, line, sizeof line);
        { ssize_t r = write(2, line, strlen(line)); (void)r; }   /* one write: no stdio lock */
        /* the memory line (host/g7_mem.h): two procfs reads, which do not wait on storage, so it comes before the disk probe and is not lost behind it */
        { HvG7Mem mem; hv_g7m_sample(&mem); hv_g7m_line(line, sizeof line, s.t_s, &mem);
          ssize_t rm = write(2, line, strlen(line)); (void)rm; }
        /* the disk probe comes AFTER the lines above: stat/statvfs may block on stuck storage, and then only this thread waits */
        { HvDiskInfo di; memset(&di, 0, sizeof di); char note[64]; int ok = hv_install_disk_check(dir_copy, &di, note, (int)sizeof note) == HV_DISK_OK;
          hv_g7w_disk_line(line, sizeof line, ok, di.size, di.allocated, di.dir_free);
          ssize_t r2 = write(2, line, strlen(line)); (void)r2; }
    }
    return NULL;
}

/* Per-attempt TSS line (host/g7_tssdiag.h). libtatsu calls this on the thread that ran the transfer, right after each attempt, so the thread-local record of the curl stand-in
   (hv_curl_last) is the one for that attempt. It only writes one line to stderr; it decides nothing and keeps nothing. */
void tss_set_attempt_hook(void (*hook)(int, int, long, long long, int, long long));
void tss_set_wait_abort(int (*fn)(void));
extern int global_quit_flag;   /* set by hv_install_cancel(), the same flag the curl stand-in watches */
static int tss_wait_abort_cb(void) { return __atomic_load_n(&global_quit_flag, __ATOMIC_SEQ_CST); }
const char *tss_last_diag(void);
int hv_curl_deny_active(void);
static void tss_attempt_line(int attempt, int curl_rc, long http_code, long long ms, int url_index, long long since_start_ms) {
    HvCurlLast l; hv_curl_last(&l);
    HvG7TssTry t; memset(&t, 0, sizeof t);
    t.attempt = attempt; t.curl_rc = curl_rc; t.http_code = http_code; t.ms = ms; t.elapsed_ms = since_start_ms;
    t.deny = hv_curl_deny_active();
    t.policy_denied = l.policy_denied; t.addrs_denied = l.addrs_denied; t.addrs_tried = l.addrs_tried; t.connected = l.connected; t.tls_done = l.tls_done;
    t.request_sent = l.request_sent; t.http_status = l.http_status; t.too_large = l.too_large; t.body_to_cb = l.body_to_cb;
    t.verify_flags = l.verify_flags; t.url_index = url_index;
    t.gai_rc = l.gai_rc; t.sock_errno = l.sock_errno; t.conn_errno = l.conn_errno; t.tls_rc = l.tls_rc;
    t.fds_open = -1; t.fds_limit = -1;
    { DIR *d = opendir("/proc/self/fd"); if (d) { int n = 0; while (readdir(d)) n++; closedir(d); t.fds_open = n - 3; } }   /* minus ".", ".." and the directory's own descriptor */
    { struct rlimit rl; if (getrlimit(RLIMIT_NOFILE, &rl) == 0) t.fds_limit = rl.rlim_cur == RLIM_INFINITY ? 0 : (int)rl.rlim_cur; }
    char line[600];
    hv_g7t_line(line, sizeof line, &t, tss_last_diag());
    ssize_t r = write(2, line, strlen(line)); (void)r;   /* one write: no stdio lock */
}

static int real_restore(void) {
    if (!ipsw_same_file()) { printf("g7: the IPSW is not the file that was verified; the restore was not started\n"); return -1; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    HvG7RestoreReq req = {.ipsw = S.ipsw, .workdir = S.workdir, .ecid = S.ecid};
    HvG7RestoreOps ops = {op_start, op_poll, op_cancel, op_sleep, NULL};
    HvG7RestoreLimits lim = S.lim;
    hv_g7_trace_reset();
    hv_net_guard_observe_request(1);   /* TEMPORARY (user-approved 2026-11-24): Apple sub-hosts on 443 are let through and reported as g7-net-observe lines; remove once the exact list is known */
    hv_net_guard_live_log(1);   /* each refused destination is printed when it happens (a stalled run never reaches the closing list below); reports only, decides nothing */
    tss_set_wait_abort(tss_wait_abort_cb);   /* a cancel is noticed while TSS waits to retry */
    tss_set_attempt_hook(tss_attempt_line);   /* one g7-tss-try line per TSS attempt: numbers only (host/g7_tssdiag.h) */
    pthread_t wd; int wd_on = pthread_create(&wd, NULL, watch_main, S.dir) == 0;   /* failure only means no watchdog lines */
    HvG7RestoreOutcome o = hv_g7_restore_run(&req, &ops, &lim);
    if (wd_on) { __atomic_store_n(&g_watch_stop, 1, __ATOMIC_RELAXED); pthread_join(wd, NULL); }
    printf("g7: restore outcome %s\n", hv_g7_restore_outcome_name(o));
    { char dl[1024]; if (hv_net_guard_denied_log(dl, sizeof dl)) printf("g7: refused destinations (name/address:port reason):\n%s", dl); }   /* what was missing from the allow-list: names and ports only */
    HvNetStats ns; hv_net_guard_stats(&ns);
    char line[512];
    if (o == HV_G7R_NO_SIGNAL) { hv_g7_summary(S.dir, o, ns.denied, &lim, line, sizeof line); printf("%s\n", line); fflush(stdout); _exit(5); }   /* the restore did not end even after the cancel: nothing may be freed or closed, the process just ends */
    hv_vm_flush_disks();
    hv_vm_stop();
    char note[300];
    HvG7Conclusion c = hv_g7_conclude(S.dir, o, note, sizeof note);
    printf("g7: %s\n", note);
    hv_g7_summary(S.dir, o, ns.denied, &lim, line, sizeof line);   /* after the conclusion, so pair= is the state the next run will find */
    printf("%s\n", line);
    return c == HV_G7_CONCLUDED_DONE ? 0 : -1;
}

int g7_real_setup(const HvG7Request *r, const char *workdir, const char *ipsw_sha256, HvG7Api *api) {
    memset(&S, 0, sizeof S);
    if (!r || !r->dir || !r->ipsw_dir || !workdir || !ipsw_sha256 || !api) return -1;
    if (snprintf(S.dir, sizeof S.dir, "%s", r->dir) >= (int)sizeof S.dir || snprintf(S.workdir, sizeof S.workdir, "%s", workdir) >= (int)sizeof S.workdir
        || snprintf(S.ipsw, sizeof S.ipsw, "%s/%s", r->ipsw_dir, HV_G7_IPSW_NAME) >= (int)sizeof S.ipsw) return -1;
    S.ecid = r->ecid;
    { int h = ipsw_hash_ok(S.ipsw, ipsw_sha256, &S.ipsw_id);
      if (h < 0) { printf("g7: the IPSW cannot be read (not hashed)\n"); return -1; }
      if (h == 0) { printf("g7: the IPSW's SHA-256 is not the expected one: REFUSED, nothing was created\n"); return -1; } }
    S.set = 1;
    S.lim = (HvG7RestoreLimits)HV_G7R_DEFAULT_LIMITS;
    api->pair_begin = real_pair;
    api->create_vm = real_create_vm;
    api->start_restore = real_restore;
    return 0;
}

void g7_real_set_limits(const HvG7RestoreLimits *lim) { if (lim && lim->timeout_ms > 0) S.lim = *lim; }

void g7_real_end(void) { memset(&S, 0, sizeof S); }
