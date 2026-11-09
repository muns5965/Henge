/* Smoke test of the virtual Mac's set-up and shut-down through host/vm.c, with no real firmware or disk:
   a zero-filled ROM and small blank disk files. What it shows: the VM is built (RAM, JIT region, devices),
   its CPU threads start and run guest code, hv_vm_stop ends them, and the exit callback and disk flush happen.
   What it does NOT show: that any guest instruction does what it should (the ROM is zeros; the CPU faults at once),
   that a real ROM boots, or anything about devices, graphics or input with a real guest. The exit reason is printed
   for information and is not checked. A second scenario (child process) runs a `b .` ROM and checks hv_vm_stop.
   Not covered: the real JIT region size, whether every secondary thread ended, flush results. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "harness.h"
#include "vm.h"

static int checks, failures;
#define CHECK(what, cond) \
    do { \
        checks++; \
        if (!(cond)) { \
            printf("  FAIL %s\n", what); \
            failures++; \
        } \
    } while (0)

static int exited;
static int exit_reason = -1;
static void on_exit_cb(int reason) {
    __atomic_store_n(&exit_reason, reason, __ATOMIC_SEQ_CST);
    __atomic_store_n(&exited, 1, __ATOMIC_SEQ_CST);
}
#define EXITED() __atomic_load_n(&exited, __ATOMIC_SEQ_CST)

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int make_file(const char *path, size_t bytes) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int r = ftruncate(fd, (off_t)bytes);
    close(fd);
    return r;
}


/* Resident and peak memory of this process, kB, from /proc/self/status (-1 if unavailable). */
static long proc_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    size_t n = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, n)) { v = atol(line + n); break; }
    if (f) fclose(f);
    return v;
}

/* Scenario run in a child process (so a hang or crash fails only this check): a ROM that is `b .` forever.
   The machine must keep running until hv_vm_stop, then stop and call back. Exit status 0 = all good. */
static int spin_child(const char *dir) {
    char rom[512], disk[512], aux[512], err[256];
    snprintf(rom, sizeof rom, "%s/vm_spin_rom.bin", dir);
    snprintf(disk, sizeof disk, "%s/vm_spin_disk.img", dir);
    snprintf(aux, sizeof aux, "%s/vm_spin_aux.img", dir);
    if (make_file(disk, 8u << 20) || make_file(aux, 2u << 20)) return 10;
    FILE *f = fopen(rom, "wb");
    if (!f) return 11;
    for (int i = 0; i < 16384; i++) {
        uint32_t w = 0x14000000u; /* b . */
        fwrite(&w, 4, 1, f);
    }
    fclose(f);
    HvVmConfig cfg = {.rom = rom, .disk = disk, .aux = aux, .ram_mb = 256, .ncpus = 2, .no_net = 1, .no_sound = 1, .jit_bytes = 64u << 20};
    if (hv_vm_create(&cfg, err, sizeof err) != 0) { printf("  spin: create failed: %s\n", err); return 12; }
    hv_vm_start(1, on_exit_cb);
    double t = now();
    while (!EXITED() && now() - t < 1.0) usleep(10000);
    if (EXITED()) { printf("  spin: stopped by itself (reason %d) but should have kept running\n", exit_reason); return 13; }
    unsigned long long pc = hv_vm_cpu(0)->pc;
    hv_vm_stop();
    double t2 = now();
    while (!EXITED() && now() - t2 < 5.0) usleep(5000);
    if (!EXITED()) { printf("  spin: did not stop within 5 s of hv_vm_stop\n"); return 14; }
    hv_vm_flush_disks();
    printf("  spin: ran 1 s on `b .` (pc %llx), hv_vm_stop took %.0f ms, exit reason %d (%s)\n", pc,
           (now() - t2) * 1e3, exit_reason, hv_vm_reason_name(exit_reason));
    unlink(rom); unlink(disk); unlink(aux);
    return 0;
}

static int run_spin_in_child(const char *dir) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) _exit(spin_child(dir));
    double t = now();
    int st = 0;
    while (now() - t < 30.0) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return 99;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    hv_test_report_crashes();
    const char *dir = getenv("HV_TEST_WORK");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char rom[512], disk[512], aux[512], missing[512], err[256];
    snprintf(rom, sizeof rom, "%s/vm_smoke_rom.bin", dir);
    snprintf(disk, sizeof disk, "%s/vm_smoke_disk.img", dir);
    snprintf(aux, sizeof aux, "%s/vm_smoke_aux.img", dir);
    snprintf(missing, sizeof missing, "%s/vm_smoke_missing.img", dir);
    CHECK("make the fake ROM (zeros)", make_file(rom, 64 * 1024) == 0);
    CHECK("make the blank disk", make_file(disk, 8u << 20) == 0);
    CHECK("make the blank aux storage", make_file(aux, 2u << 20) == 0);
    unlink(missing);

    int sr = run_spin_in_child(dir);
    if (sr != 0) printf("  spin scenario status %d\n", sr);
    CHECK("spin ROM (`b .`): runs until hv_vm_stop, then stops cleanly (child process)", sr == 0);

    /* Failures must be clean: a reason, no crash. */
    HvVmConfig bad = {.rom = NULL, .disk = disk, .aux = aux, .ram_mb = 64, .ncpus = 1, .no_net = 1, .no_sound = 1};
    err[0] = 0;
    CHECK("no ROM given is refused", hv_vm_create(&bad, err, sizeof err) == -1 && err[0]);
    bad.rom = rom;
    bad.disk = missing;
    err[0] = 0;
    CHECK("a missing disk is refused", hv_vm_create(&bad, err, sizeof err) == -1 && err[0]);
    printf("  refusals: \"%s\"\n", err);

    /* HV_TEST_BIG=1: the VM's default sizes (2048 MB RAM, 768 MB JIT) instead of the small test sizes. */
    int big = getenv("HV_TEST_BIG") != NULL;
    HvVmConfig cfg = {.rom = rom, .disk = disk, .aux = aux, .ram_mb = big ? 2048 : 256, .ncpus = 2, .no_net = 1, .no_sound = 1,
                      .jit_bytes = big ? (768u << 20) : (64u << 20)};
    double t0 = now();
    err[0] = 0;
    int r = hv_vm_create(&cfg, err, sizeof err);
    if (r != 0) printf("  create failed: %s\n", err);
    CHECK("hv_vm_create builds the machine", r == 0);
    if (r == 0) {
        HvVmapple *b = hv_vm_board();
        CHECK("guest RAM is mapped", b && b->ram && b->ram != (void *)-1);
        CHECK("guest RAM size is what was asked", b && b->ram_size == (big ? 2048ull << 20 : 256ull << 20));
        CHECK("two CPUs", hv_vm_ncpus() == 2);
        CHECK("CPU 0 exists", hv_vm_cpu(0) != NULL);
        double t_created = now();
        printf("  mem: after create%s: VmRSS %ld kB, VmHWM %ld kB\n", big ? " (big sizes)" : "", proc_kb("VmRSS:"), proc_kb("VmHWM:"));

        hv_vm_start(1, on_exit_cb);
        int stopped_by_us = 0;
        double t_start = now();
        while (!EXITED() && now() - t_start < 1.5) usleep(10000);
        if (!exited) {
            stopped_by_us = 1;
            hv_vm_stop();
            double t_stop = now();
            while (!EXITED() && now() - t_stop < 5.0) usleep(10000);
        }
        CHECK("the machine stopped and the exit callback ran", EXITED());
        printf("  mem: after run: VmRSS %ld kB, VmHWM %ld kB\n", proc_kb("VmRSS:"), proc_kb("VmHWM:"));
        HvBootDisplay d;
        int bd = hv_vm_boot_display(&d); /* only has to return, with no real guest it has nothing to show */
        hv_vm_flush_disks();
        printf("  info: create %.0f ms, ran %.0f ms%s, exit reason %d (%s), cpu0 pc %llx el%u, boot display %s\n",
               (t_created - t0) * 1e3, (now() - t_created) * 1e3, stopped_by_us ? " (stopped by the test)" : " (stopped by itself)",
               exit_reason, hv_vm_reason_name(exit_reason), (unsigned long long)hv_vm_cpu(0)->pc, hv_vm_cpu(0)->el,
               bd == 0 ? "present" : "none");
    }
    unlink(rom);
    unlink(disk);
    unlink(aux);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
