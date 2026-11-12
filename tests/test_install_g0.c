/* host/install_g0.c with host/install.c (both -D__ANDROID__ -DHV_INSTALL_G0 -DHV_STOP_BEFORE_ROOTFS on Linux) and the real session, registry and connection policy.
   Link-time wrappers replace usbmuxd_main, idevicerestore_start (a fake tool that stops at stop A, or does not) and hv_usb_wait_online. The verdict table is read back
   row by row. Nothing touches a device or the network. The one thing this cannot show: a refused or attempted connection (needs the real tool). */
#define _GNU_SOURCE 1
#include "install.h"
#include "install_g0.h"
#include "disk.h"
#include "machine.h"
#include "idevicerestore/src/hv_stop.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int global_quit_flag;
extern int should_exit;
extern volatile int usbmuxd_stopped;

static int nfail;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); fflush(stdout); if (!ok_) nfail++; } while (0)

static volatile int eng_mode;   /* 0 stop A (hook + 217), 1 no hook but 217, 2 returns 0 without stopping, 3 waits for the quit flag, 4 ignores it until released */
static volatile int release_engine, fake_writes;
int __wrap_usbmuxd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    should_exit = 0;
    while (!should_exit) usleep(5000);
    usbmuxd_stopped = 1;
    return 0;
}
int __wrap_idevicerestore_start(void *c) {
    (void)c;
    if (eng_mode == 0) { struct hv_stop_a_info i; memset(&i, 0, sizeof i); if (hv_stop_a_hook) hv_stop_a_hook(&i); return HV_STOP_BEFORE_ROOTFS_RC; }
    if (eng_mode == 1) return HV_STOP_BEFORE_ROOTFS_RC;
    if (eng_mode == 2) return 0;
    if (eng_mode == 3) { for (int i = 0; i < 1000 && !global_quit_flag; i++) usleep(10000); return -1; }
    while (!release_engine) usleep(10000);
    return -1;
}
int64_t __wrap_hv_usb_wait_online(void *d, int ms) { (void)d, (void)ms; return 0; }
void hv_vm_disk_write_stats(uint64_t *calls, uint64_t *refused) { *calls = (uint64_t)fake_writes, *refused = (uint64_t)fake_writes; }

int hv_knob[HV_K_COUNT];   /* host/disk.c reads the flush knob; the machine that owns it is not linked here */
static char dummy_usb[64];
static char disk[256], aux[256];
static void mkfiles(void) {
    snprintf(disk, sizeof disk, "/tmp/g0t_disk_%d.img", getpid());
    snprintf(aux, sizeof aux, "/tmp/g0t_aux_%d.img", getpid());
    int fd = open(disk, O_RDWR | O_CREAT | O_TRUNC, 0600); ftruncate(fd, 1LL << 30); close(fd);        /* sparse, all hole */
    fd = open(aux, O_RDWR | O_CREAT | O_TRUNC, 0600); unsigned char h[24] = {0xa5, 0xd5}; write(fd, h, 24); ftruncate(fd, 33570816); close(fd);
}
static void poke(const char *path, long long off) { int fd = open(path, O_RDWR); unsigned char b = 0x5a; pwrite(fd, &b, 1, off); close(fd); }
/* "ok" / "FAILED" of the row `name` in the captured table, or "absent" */
static const char *row_status(const char *buf, const char *name) {
    char key[64]; snprintf(key, sizeof key, "install g0: %s", name);
    const char *l = strstr(buf, key);
    if (!l) return "absent";
    const char *eol = strchr(l, '\n');
    const char *f = strstr(l, "FAILED");
    return (f && (!eol || f < eol)) ? "FAILED" : "ok";
}
static char *run_g0(int cap_ms, int *failed, int *waited) {
    char *buf = NULL; size_t len = 0;
    int b = hv_g0_begin(disk, aux, 0x1F4C0DE);
    CHECK(b == 0, "begin: storage identity taken, deny mode on");
    int s = hv_g0_start((HvVirtioUsb *)dummy_usb, "x.ipsw", "/tmp", 0x1F4C0DE);
    *waited = s == 0 ? hv_g0_wait(cap_ms) : 1;
    FILE *o = open_memstream(&buf, &len);
    *failed = hv_g0_finish(o, disk, aux);
    fclose(o);
    fputs(buf, stdout); fflush(stdout);
    return buf;
}

int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
    int failed, waited;
    mkfiles();
    if (!strcmp(c, "pass")) {
        eng_mode = 0;
        char *t = run_g0(10000, &failed, &waited);
        CHECK(waited == 1 && failed == 0, "stop A reached, 217, session report 2, cleanup 0, storage untouched: G0 passes");
        CHECK(strstr(t, "g0_passed stopped=before_rootfs") != NULL, "the result line says so");
        CHECK(!strcmp(row_status(t, "stop_a_hook"), "ok") && !strcmp(row_status(t, "engine_rc"), "ok") && !strcmp(row_status(t, "session"), "ok") && !strcmp(row_status(t, "usbmuxd"), "ok"), "hook, 217, session and usbmuxd rows ok");
        CHECK(!strcmp(row_status(t, "disk_identity"), "ok") && !strcmp(row_status(t, "aux_identity"), "ok") && !strcmp(row_status(t, "storage_writes"), "ok") && !strcmp(row_status(t, "threads"), "ok"), "storage, write and thread rows ok");
    } else if (!strcmp(c, "disk_changed")) {
        eng_mode = 0;
        hv_g0_begin(disk, aux, 1);   /* identity first ... */
        poke(disk, 512LL << 20);     /* ... then one byte lands in the sparse hole */
        int s = hv_g0_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1);
        hv_g0_wait(10000);
        char *buf = NULL; size_t len = 0; FILE *o = open_memstream(&buf, &len);
        failed = hv_g0_finish(o, disk, aux); fclose(o); fputs(buf, stdout);
        CHECK(s == 0 && failed >= 1 && !strcmp(row_status(buf, "disk_identity"), "FAILED") && !strcmp(row_status(buf, "aux_identity"), "ok"), "one byte written into a hole of the sparse disk is seen; the aux row stays ok");
        CHECK(strstr(buf, "g0_passed") == NULL, "no pass line");
    } else if (!strcmp(c, "aux_changed")) {
        eng_mode = 0;
        hv_g0_begin(disk, aux, 1);
        poke(aux, 20000);
        hv_g0_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1);
        hv_g0_wait(10000);
        char *buf = NULL; size_t len = 0; FILE *o = open_memstream(&buf, &len);
        failed = hv_g0_finish(o, disk, aux); fclose(o); fputs(buf, stdout);
        CHECK(failed >= 1 && !strcmp(row_status(buf, "aux_identity"), "FAILED") && !strcmp(row_status(buf, "disk_identity"), "ok"), "a byte changed in the auxiliary storage is seen");
    } else if (!strcmp(c, "no_hook")) {
        eng_mode = 1;
        char *t = run_g0(10000, &failed, &waited);
        CHECK(failed >= 1 && !strcmp(row_status(t, "stop_a_hook"), "FAILED") && !strcmp(row_status(t, "engine_rc"), "ok"), "217 without the hook is not enough: stop_a_hook FAILED");
    } else if (!strcmp(c, "ran_on")) {
        eng_mode = 2;
        char *t = run_g0(10000, &failed, &waited);
        CHECK(failed >= 1 && !strcmp(row_status(t, "stop_a_hook"), "FAILED") && !strcmp(row_status(t, "engine_rc"), "FAILED") && !strcmp(row_status(t, "session"), "FAILED"), "a tool that returns 0 without stopping: hook, return value and session rows all FAILED");
    } else if (!strcmp(c, "write_asked")) {
        eng_mode = 0; fake_writes = 3;
        char *t = run_g0(10000, &failed, &waited);
        CHECK(failed >= 1 && !strcmp(row_status(t, "storage_writes"), "FAILED") && !strcmp(row_status(t, "disk_identity"), "ok"), "writes asked of the storage fail G0 even though the files are unchanged (refused)");
    } else if (!strcmp(c, "cancel")) {
        eng_mode = 3;
        char *t = run_g0(600, &failed, &waited);
        CHECK(waited == 0 && failed >= 1 && !strcmp(row_status(t, "ended"), "FAILED"), "a run that needs the cancel request to end fails G0 (ended FAILED), and the wait reports 0");
        CHECK(strstr(t, "g0_passed") == NULL, "no pass line");
    } else if (!strcmp(c, "stuck")) {
        eng_mode = 4;
        char *t = run_g0(600, &failed, &waited);
        CHECK(waited == -1 && failed >= 1 && !strcmp(row_status(t, "ended"), "FAILED") && strstr(t, "RESULT: FAILED") != NULL, "a run that ignores the cancel: wait reports -1, the table stops after 'ended' and says FAILED");
        CHECK(strstr(t, "stop_a_hook") == NULL, "no further rows are built on a thread that is still running");
        release_engine = 1; usleep(300000);
    } else if (!strcmp(c, "refuse_missing")) {
        unlink(aux);
        CHECK(hv_g0_begin(disk, aux, 1) == -1, "a missing storage file is refused: G0 creates nothing");
        CHECK(access(aux, F_OK) != 0, "and the file was not created");
        CHECK(hv_g0_begin(disk, "/nonexistent", 0) == -1, "a missing file and no ECID are refused");
    } else if (!strcmp(c, "no_ecid")) {
        CHECK(hv_g0_begin(disk, aux, 0) == -1, "no ECID is refused (G0 never makes one up)");
    } else if (!strcmp(c, "readonly_disk")) {
        HvDisk d; uint64_t calls, refused; unsigned char b[512] = {1};
        CHECK(hv_disk_open(&d, aux, 16384, 1) == 0, "open read-only");
        CHECK(hv_disk_write(&d, 0, b, 512) == -1, "a write is refused");
        hv_disk_write_stats(&d, &calls, &refused);
        CHECK(calls == 1 && refused == 1, "and counted (1 asked, 1 refused)");
        CHECK(hv_disk_read(&d, 0, b, 512) == 0 && hv_disk_flush(&d) == 0, "reads and flush work");
        hv_disk_close(&d);
        CHECK(hv_disk_open(&d, aux, 16384, 0) == 0 && hv_disk_write(&d, 0, b, 512) == 0, "read-write open still writes (the normal paths are unchanged)");
        hv_disk_write_stats(&d, &calls, &refused);
        CHECK(calls == 1 && refused == 0, "counted as asked, not refused");
        hv_disk_close(&d);
    } else { printf("unknown case\n"); return 2; }
    unlink(disk); unlink(aux);
    printf("%s\n", nfail ? "TEST FAILED" : "TEST OK");
    return nfail ? 1 : 0;
}
