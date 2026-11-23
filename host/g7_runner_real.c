/* THE EXECUTION TABLE OF THE G7 RUNNER. Compiled only by tools/g7-runner-build.sh real (-DHV_G7_REAL_EXECUTION); never by an APK step, never by the default build, never by a test.
   Running a binary built from this makes the Apple signing requests the restore needs and WRITES the install disk and the AUX: that is a separately approved step, and several
   conditions are still open (see the end of this comment). What each member does:
     pair_begin    hv_g7_pair_begin with the real disk creator (host/disk_policy.c): state file (with the ECID tag), the 128 GiB sparse disk, the AUX, the AUX copy
     create_vm     hv_vm_create with the configuration hv_g7_config_ok accepted (installer, writable storage, the ECID of the file), then hv_vm_start
     start_restore the IPSW identity is re-checked, then hv_g7_restore_run (the restore tool through hv_install_*) and, after the machine's disks are flushed and it is stopped,
                   hv_g7_conclude, the only caller of hv_g7_pair_finish. It returns 0 only for a finished pair; every other end keeps the pair in_progress, deleting and moving nothing.
   Open conditions, all NOT done: (1) the net allow-list: no Apple host is approved here, so the restore's first signing request is refused by host/net_guard until a separately approved
   step adds the hosts (the TSS hosts G2-G6 use, and whatever the URLAsset downloads of a full restore need: not examined); (2) a launcher and its approval; (3) the judgment's file checks
   (GPT header, allocated size, AUX header) have never been compared with a real restore; (4) timing: the default limits are guesses; (5) what a full restore does after the restore
   options (restored_start_restore and later) has never run on this engine. */
#define _GNU_SOURCE
#include "g7_runner_real.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disk_policy.h"
#include "g7_preflight.h"
#include "g7_restore.h"
#include "install.h"
#include "mbedtls/sha256.h"
#include "usb_host.h"
#include "vm.h"
#include "../devices/vmapple.h"

static struct {
    char dir[4096], workdir[4096], ipsw[4096];
    uint64_t ecid;
    struct stat ipsw_id;   /* the identity of the file that was hashed */
    int set;
} S;

static int hex_eq(const unsigned char *d, const char *hex) {
    static const char H[] = "0123456789abcdef";
    if (strlen(hex) != 64) return 0;
    for (int i = 0; i < 32; i++) {
        char a = H[d[i] >> 4], b = H[d[i] & 15], x = hex[2 * i], y = hex[2 * i + 1];
        if (x >= 'A' && x <= 'F') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F') y = (char)(y - 'A' + 'a');
        if (a != x || b != y) return 0;
    }
    return 1;
}

static int ipsw_hash_ok(const char *path, const char *hex, struct stat *id) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    if (fstat(fd, id) != 0 || !S_ISREG(id->st_mode)) { close(fd); return 0; }
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
    return n == 0 && hex_eq(d, hex);
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
    return 0;
}

static int op_start(const HvG7RestoreReq *q, void *ctx) {
    (void)ctx;
    HvVmapple *b = hv_vm_board();
    if (!b || !b->vusb) return -1;
    if (hv_usb_wait_online(b->vusb, 60000) < 0) return -1;   /* the guest did not appear on the USB bus: the restore is not started */
    return hv_install_start(b->vusb, q->ipsw, q->workdir, q->ecid, NULL, NULL);
}
static int op_poll(void *ctx) { (void)ctx; return hv_install_result(); }
static void op_cancel(void *ctx) { (void)ctx; hv_install_cancel(); }
static void op_sleep(void *ctx, int ms) { (void)ctx; usleep((useconds_t)ms * 1000u); }
static void on_term(int sig) { (void)sig; hv_g7_restore_request_stop(); }

static int real_restore(void) {
    if (!ipsw_same_file()) { printf("g7: the IPSW is not the file that was verified; the restore was not started\n"); return -1; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    HvG7RestoreReq req = {.ipsw = S.ipsw, .workdir = S.workdir, .ecid = S.ecid};
    HvG7RestoreOps ops = {op_start, op_poll, op_cancel, op_sleep, NULL};
    HvG7RestoreLimits lim = HV_G7R_DEFAULT_LIMITS;
    HvG7RestoreOutcome o = hv_g7_restore_run(&req, &ops, &lim);
    printf("g7: restore outcome %s\n", hv_g7_restore_outcome_name(o));
    if (o == HV_G7R_NO_SIGNAL) { fflush(stdout); _exit(5); }   /* the restore did not end even after the cancel: nothing may be freed or closed, the process just ends */
    hv_vm_flush_disks();
    hv_vm_stop();
    char note[300];
    HvG7Conclusion c = hv_g7_conclude(S.dir, o, note, sizeof note);
    printf("g7: %s\n", note);
    return c == HV_G7_CONCLUDED_DONE ? 0 : -1;
}

int g7_real_setup(const HvG7Request *r, const char *workdir, const char *ipsw_sha256, HvG7Api *api) {
    memset(&S, 0, sizeof S);
    if (!r || !r->dir || !r->ipsw_dir || !workdir || !ipsw_sha256 || !api) return -1;
    if (snprintf(S.dir, sizeof S.dir, "%s", r->dir) >= (int)sizeof S.dir || snprintf(S.workdir, sizeof S.workdir, "%s", workdir) >= (int)sizeof S.workdir
        || snprintf(S.ipsw, sizeof S.ipsw, "%s/%s", r->ipsw_dir, HV_G7_IPSW_NAME) >= (int)sizeof S.ipsw) return -1;
    S.ecid = r->ecid;
    if (!ipsw_hash_ok(S.ipsw, ipsw_sha256, &S.ipsw_id)) { printf("g7: the IPSW's SHA-256 is not the expected one (or it cannot be read)\n"); return -1; }
    S.set = 1;
    api->pair_begin = real_pair;
    api->create_vm = real_create_vm;
    api->start_restore = real_restore;
    return 0;
}

void g7_real_end(void) { memset(&S, 0, sizeof S); }
