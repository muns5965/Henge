/* Headless runner for the virtual Mac on a non-Apple host (Android app's child process, Linux).
   Starts the real engine with the files given on the command line and prints, every few seconds and at the end,
   where CPU 0 is: pc, exception level, the last exception's ESR/FAR/ELR, why the machine stopped. The point is to
   learn the FIRST place a real boot ROM goes wrong. A clean exit of this program says nothing about the guest
   having booted; read the status lines. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vm.h"

static volatile int exited;
static volatile int exit_reason = -1;
static void on_exit_cb(int reason) {
    exit_reason = reason;
    exited = 1;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void status(const char *tag, double t) {
    int n = hv_vm_ncpus();
    for (int i = 0; i < n; i++) {
        HvCpu *c = hv_vm_cpu(i);
        if (!c) continue;
        printf("vmrun %s t=%.1fs cpu%d pc=%llx el=%u elr_el1=%llx esr_el1=%llx far_el1=%llx vbar_el1=%llx sctlr_el1=%llx halted=%d\n",
               tag, t, i, (unsigned long long)c->pc, c->el, (unsigned long long)c->elr_el1,
               (unsigned long long)c->esr_el1, (unsigned long long)c->far_el1, (unsigned long long)c->vbar_el1,
               (unsigned long long)c->sctlr_el1, c->halted);
    }
    HvBootDisplay d;
    if (hv_vm_boot_display(&d) == 0) printf("vmrun %s boot display present %ux%u\n", tag, d.width, d.height);
    else printf("vmrun %s boot display none\n", tag);
    fflush(stdout);
}

static int write_bmp(const char *path, const uint8_t *bgra, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t size = 54 + (uint32_t)w * (uint32_t)h * 4;
    uint8_t hd[54] = {'B', 'M'};
    memcpy(hd + 2, &size, 4);
    uint32_t off = 54, dib = 40, planes_bpp = 1 | (32u << 16), img = (uint32_t)w * (uint32_t)h * 4;
    int32_t ww = w, hh = -h; /* top-down */
    memcpy(hd + 10, &off, 4);
    memcpy(hd + 14, &dib, 4);
    memcpy(hd + 18, &ww, 4);
    memcpy(hd + 22, &hh, 4);
    memcpy(hd + 26, &planes_bpp, 4);
    memcpy(hd + 34, &img, 4);
    fwrite(hd, 1, 54, f);
    fwrite(bgra, 1, img, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HvVmConfig cfg = {.ram_mb = 2048, .ncpus = 2, .aux_offset = 16384, .no_net = 1, .no_sound = 1};
    double seconds = 30;
    const char *snap = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--rom") && more) cfg.rom = argv[++i];
        else if (!strcmp(a, "--disk") && more) cfg.disk = argv[++i];
        else if (!strcmp(a, "--aux") && more) cfg.aux = argv[++i];
        else if (!strcmp(a, "--aux-offset") && more) cfg.aux_offset = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--ecid") && more) cfg.ecid = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed") && more) cfg.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--ram-mb") && more) cfg.ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--cpus") && more) cfg.ncpus = atoi(argv[++i]);
        else if (!strcmp(a, "--jit-mb") && more) cfg.jit_bytes = (size_t)strtoull(argv[++i], NULL, 0) << 20;
        else if (!strcmp(a, "--log") && more) cfg.log = atoi(argv[++i]);
        else if (!strcmp(a, "--seconds") && more) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--snapshot") && more) snap = argv[++i];
        else {
            fprintf(stderr, "usage: vmrun --rom F --disk F --aux F [--ecid N --seed N --aux-offset N --ram-mb N --cpus N --jit-mb N --log N --seconds N --snapshot out.bmp]\n");
            return 2;
        }
    }
    if (!cfg.rom || !cfg.disk || !cfg.aux) {
        fprintf(stderr, "vmrun: --rom, --disk and --aux are required\n");
        return 2;
    }
    printf("vmrun config: ram %llu MB, cpus %d, jit %zu MB, ecid %llx, seed %llx, aux_offset %llu, seconds %.0f\n",
           (unsigned long long)cfg.ram_mb, cfg.ncpus, cfg.jit_bytes >> 20, (unsigned long long)cfg.ecid,
           (unsigned long long)cfg.seed, (unsigned long long)cfg.aux_offset, seconds);
    char err[512] = "";
    double t0 = now();
    if (hv_vm_create(&cfg, err, sizeof err) != 0) {
        printf("vmrun RESULT: CREATE FAILED: %s\n", err);
        return 1;
    }
    printf("vmrun created in %.0f ms\n", (now() - t0) * 1e3);
    status("start", 0);
    hv_vm_start(1, on_exit_cb);
    double t1 = now(), next = 2;
    while (!exited && now() - t1 < seconds) {
        usleep(100000);
        if (now() - t1 >= next) {
            status("run", now() - t1);
            next += next < 10 ? 2 : 10;
        }
    }
    int by_us = 0;
    if (!exited) {
        by_us = 1;
        hv_vm_stop();
        double ts = now();
        while (!exited && now() - ts < 5.0) usleep(10000);
    }
    status("final", now() - t1);
    if (snap) {
        enum { W = 640, H = 400 };
        static uint8_t px[W * H * 4];
        if (hv_vm_snapshot(px, W, H) == 0 && write_bmp(snap, px, W, H) == 0) printf("vmrun snapshot written: %s\n", snap);
        else printf("vmrun snapshot: nothing to show\n");
    }
    hv_vm_flush_disks();
    if (exited)
        printf("vmrun RESULT: stopped %s after %.1fs, exit reason %d (%s)\n", by_us ? "by vmrun's time limit" : "by the guest/engine",
               now() - t1, exit_reason, hv_vm_reason_name(exit_reason));
    else
        printf("vmrun RESULT: time limit reached and the machine did not confirm stopping within 5 s\n");
    return 0;
}
