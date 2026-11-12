/* Headless runner for the virtual Mac on a non-Apple host (Android app's child process, Linux).
   Starts the real engine with the files given on the command line and prints, every few seconds and at the end,
   where CPU 0 is: pc, exception level, the last exception's ESR/FAR/ELR, why the machine stopped. The point is to
   learn the FIRST place a real boot ROM goes wrong. A clean exit of this program says nothing about the guest
   having booted; read the status lines. */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "vm.h"
#ifdef HV_IRECV_PROBE
#include "irecv_probe.h"
#include "dnload_probe.h"
#endif
#ifdef HV_TSS_PROBE
#include "tss_probe.h"
#endif
#ifdef HV_RESTORE_PREFLIGHT
#include "restore_preflight.h"
#endif
#ifdef HV_INSTALL_G0
#include "install_g0.h"
#endif
#include "vmapple.h"

static int exited;
static int exit_reason = -1;
static void on_exit_cb(int reason) {
    __atomic_store_n(&exit_reason, reason, __ATOMIC_SEQ_CST);
    __atomic_store_n(&exited, 1, __ATOMIC_SEQ_CST);
}
#define EXITED() __atomic_load_n(&exited, __ATOMIC_SEQ_CST)

/* Samples of CPU 0 taken from this thread while it runs on another. The fields are read one by one without a lock,
   so a sample can mix two moments (a torn read); treat each line as approximate. A "change" is logged whenever the
   exception level, ESR_EL1 or VBAR_EL1 differs from the sample before, so the first exceptions are kept even when the
   guest moves on: the first 40 changes are printed as they happen with the time of the sample. */
typedef struct { unsigned long long pc, esr, far, elr, vbar; unsigned el; } Sample;
static void sample(Sample *o) {
    HvCpu *c = hv_vm_cpu(0);
    o->pc = __atomic_load_n(&c->pc, __ATOMIC_RELAXED);
    o->esr = __atomic_load_n(&c->esr_el1, __ATOMIC_RELAXED);
    o->far = __atomic_load_n(&c->far_el1, __ATOMIC_RELAXED);
    o->elr = __atomic_load_n(&c->elr_el1, __ATOMIC_RELAXED);
    o->vbar = __atomic_load_n(&c->vbar_el1, __ATOMIC_RELAXED);
    o->el = __atomic_load_n(&c->el, __ATOMIC_RELAXED);
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
    HvVmapple *bd = hv_vm_board();
    if (bd && bd->vusb) { /* the USB device port (installer boots only); online/session are read under the port's own lock */
        uint32_t sess = 0;
        int on = hv_vusb_online(bd->vusb, &sess);
        printf("vmrun %s usb port: guest on the bus=%d session=%u (rom front-end owns port=%d, PCI front-end DRIVER_OK=%d; unlocked reads)\n", tag, on, sess,
               __atomic_load_n(&bd->vusb->rom_on, __ATOMIC_RELAXED), __atomic_load_n(&bd->vusb->pci_on, __ATOMIC_RELAXED));
    } else {
        printf("vmrun %s usb port: not present in this boot mode\n", tag);
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

/* A new auxiliary storage file the way Henge's iOS app makes one (ios/Henge/Native/henge-vm.c): a 24-byte header, zeros to 32 MB + 16 KB. */
static int make_aux(const char *path) {
    static const unsigned char header[24] = {0xa5, 0xd5, 0xb3, 0xfe, 0x97, 0xad, 0x1d, 0x27, 1, 0, 0, 0, 0, 0, 0, 0,
                                             1, 0, 0, 0, 0, 0, 0, 0};
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, header, sizeof header) == (ssize_t)sizeof header && ftruncate(fd, 33570816) == 0;
    close(fd);
    return ok ? 0 : -1;
}

static int make_sparse(const char *path, unsigned long long bytes) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int r = ftruncate(fd, (off_t)bytes);
    close(fd);
    return r;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HvVmConfig cfg = {.ram_mb = 2048, .ncpus = 2, .aux_offset = 16384, .no_net = 1, .no_sound = 1};
    double seconds = 30;
    const char *snap = NULL;
    int make_blank = 0, recreate_blank = 0, psci_log = 0, usb_probe = 0, irecv_probe = 0, dnload_probe = 0, dnload_a3 = 0, dnload_a4 = 0, tss_probe = 0;
    const char *pf_ipsw = NULL, *pf_work = ".", *g0_ipsw = NULL;
    int pf_stage = 0;
    unsigned long long disk_gb = 8;
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
        else if (!strcmp(a, "--make-blank")) make_blank = 1; /* create --disk and --aux when they do not exist */
        else if (!strcmp(a, "--recreate-blank")) recreate_blank = 1; /* with --make-blank: start the files named blank_* from scratch */
        else if (!strcmp(a, "--psci-log")) psci_log = 1;             /* print every PSCI call the guest makes (function, x1..x3, pc) */
        else if (!strcmp(a, "--usb-probe")) usb_probe = 1; /* log USB port events and probe the device from the host side (descriptor + strings) */
        else if (!strcmp(a, "--dnload-probe-a4")) dnload_probe = dnload_a4 = 1; /* A0-A3, then ONE ABORT and ONE GET_STATE (host/dnload_probe.h) */
        else if (!strcmp(a, "--dnload-probe-a3")) dnload_probe = dnload_a3 = 1; /* A0-A2 plus ONE GET_STATUS (see host/dnload_probe.h) */
        else if (!strcmp(a, "--dnload-probe")) dnload_probe = 1; /* stage A0-A2 of the DFU download test (see host/dnload_probe.h); a vmrun built with HV_IRECV_PROBE */
        else if (!strcmp(a, "--tss-probe")) tss_probe = 1; /* needs a vmrun built with HV_TSS_PROBE: ONE request to Apple's TSS server, see host/tss_probe.h */
        else if (!strcmp(a, "--irecv-probe")) irecv_probe = 1; /* open the USB device through libirecovery (needs a vmrun built with HV_IRECV_PROBE); sets the configuration, sends no firmware */
        else if (!strcmp(a, "--restore-preflight") && more) pf_ipsw = argv[++i]; /* needs a vmrun built with HV_RESTORE_PREFLIGHT: the real restore tool up to its no-action stop (host/restore_preflight.h); asks Apple nothing */
        else if (!strcmp(a, "--restore-preflight-stop-a")) pf_stage = 1; /* with --restore-preflight: stop A (before the root filesystem step) instead of FLAG_NOACTION */
        else if (!strcmp(a, "--install-g0") && more) g0_ipsw = argv[++i]; /* needs a vmrun built with HV_INSTALL_G0: the install entry point (host/install.c) up to the tool's stop A, storage read-only; asks Apple nothing (host/install_g0.h) */
        else if (!strcmp(a, "--workdir") && more) pf_work = argv[++i];
        else if (!strcmp(a, "--disk-gb") && more) disk_gb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--installer")) cfg.installer = 1; /* boot the ROM in DFU mode (the macOS installer's way in) */
        else {
            fprintf(stderr, "usage: vmrun --rom F --disk F --aux F [--installer] [--make-blank [--recreate-blank] [--disk-gb N]] [--psci-log] [--usb-probe] [--irecv-probe] [--tss-probe] [--restore-preflight IPSW [--restore-preflight-stop-a] [--workdir DIR]] [--install-g0 IPSW [--workdir DIR]] [--dnload-probe | --dnload-probe-a3 | --dnload-probe-a4] [--ecid N --seed N --aux-offset N --ram-mb N --cpus N --jit-mb N --log N --seconds N --snapshot out.bmp]\n");
            return 2;
        }
    }
    if (!cfg.rom || !cfg.disk || !cfg.aux) {
        fprintf(stderr, "vmrun: --rom, --disk and --aux are required\n");
        return 2;
    }
    struct stat sb;
    if (psci_log) setenv("HV_PSCI_LOG", "1", 1); /* read by the engine at its first PSCI call, so before the machine exists */
    if (usb_probe) { /* read by the VM when it creates the USB port, so before hv_vm_create */
        setenv("HV_VUSB_LOG", "1", 1);
        setenv("HV_VUSB_PROBE", "1", 1);
    }
    if (dnload_probe) {
#ifdef HV_IRECV_PROBE
        if (usb_probe || irecv_probe) { fprintf(stderr, "vmrun: --dnload-probe cannot be combined with --usb-probe or --irecv-probe\n"); return 2; }
        if (!cfg.installer) { fprintf(stderr, "vmrun: --dnload-probe needs --installer (the DFU boot)\n"); return 2; }
        setenv("HV_VUSB_LOG", "1", 1);
#else
        fprintf(stderr, "vmrun: this build has no dnload probe in it (--dnload-probe unavailable)\n");
        return 2;
#endif
    }
    if (pf_ipsw) {
#ifdef HV_RESTORE_PREFLIGHT
        if (usb_probe || irecv_probe || dnload_probe || tss_probe) { fprintf(stderr, "vmrun: --restore-preflight cannot be combined with the USB probes\n"); return 2; }
        if (!cfg.installer) { fprintf(stderr, "vmrun: --restore-preflight needs --installer (the DFU boot)\n"); return 2; }
        if (seconds < 150) seconds = 150; /* up to 60 s for the guest to come on the bus, then the tool (cap 120 s) */
#else
        fprintf(stderr, "vmrun: this build has no restore tool in it (--restore-preflight unavailable)\n");
        return 2;
#endif
    }
    if (g0_ipsw) {
#ifdef HV_INSTALL_G0
        if (usb_probe || irecv_probe || dnload_probe || tss_probe || pf_ipsw) { fprintf(stderr, "vmrun: --install-g0 cannot be combined with the probes or --restore-preflight\n"); return 2; }
        if (!cfg.installer) { fprintf(stderr, "vmrun: --install-g0 needs --installer (the DFU boot)\n"); return 2; }
        if (make_blank || recreate_blank) { fprintf(stderr, "vmrun: --install-g0 never creates or recreates storage (no --make-blank, no --recreate-blank)\n"); return 2; }
        if (getenv("HV_USB_ON_NORMAL")) { fprintf(stderr, "vmrun: --install-g0 is not combined with the normal-boot USB experiment\n"); return 2; }
        cfg.readonly_storage = 1;   /* the disk and the auxiliary storage cannot be written; every write the guest asks for is refused and counted */
        cfg.no_net = 1;             /* the guest's own network stays off (already the default of this program) */
        if (seconds < 200) seconds = 200;
#else
        fprintf(stderr, "vmrun: this build has no install entry point test in it (--install-g0 unavailable)\n");
        return 2;
#endif
    }
    if (tss_probe) {
#ifdef HV_TSS_PROBE
        if (usb_probe || irecv_probe || dnload_probe) { fprintf(stderr, "vmrun: --tss-probe cannot be combined with the other USB probes\n"); return 2; }
        if (!cfg.installer) { fprintf(stderr, "vmrun: --tss-probe needs --installer (the DFU boot)\n"); return 2; }
        if (seconds < 120) seconds = 120;
#else
        fprintf(stderr, "vmrun: this build has no TSS probe in it (--tss-probe unavailable)\n");
        return 2;
#endif
    }
    if (irecv_probe) {
#ifdef HV_IRECV_PROBE
        if (usb_probe) { fprintf(stderr, "vmrun: --irecv-probe and --usb-probe both talk to the device; use one\n"); return 2; }
        if (!cfg.installer) { fprintf(stderr, "vmrun: --irecv-probe needs --installer (the DFU boot)\n"); return 2; }
        setenv("HV_VUSB_LOG", "1", 1);
        setenv("HV_LIBUSB_LOG", "1", 1);
#else
        fprintf(stderr, "vmrun: this build has no libirecovery in it (--irecv-probe unavailable)\n");
        return 2;
#endif
    }
    if (make_blank && recreate_blank) {
        /* only files whose name starts with "blank_" are ever removed: a real installed disk is never touched */
        const char *names[2] = {cfg.disk, cfg.aux};
        for (int k = 0; k < 2; k++) {
            const char *slash = strrchr(names[k], '/');
            const char *base = slash ? slash + 1 : names[k];
            if (!strncmp(base, "blank_", 6)) { if (unlink(names[k]) == 0) printf("vmrun: removed the old %s to start fresh\n", base); }
            else printf("vmrun: --recreate-blank leaves %s alone (its name does not start with blank_)\n", base);
        }
    }
    if (make_blank) {
        if (stat(cfg.disk, &sb) != 0) printf("vmrun: new blank disk %s (%llu GB, sparse): %s\n", cfg.disk, disk_gb, make_sparse(cfg.disk, disk_gb << 30) == 0 ? "ok" : "FAILED");
        if (stat(cfg.aux, &sb) != 0) printf("vmrun: new auxiliary storage %s: %s\n", cfg.aux, make_aux(cfg.aux) == 0 ? "ok" : "FAILED");
    }
    char ecid_txt[24];
    printf("vmrun config: ram %llu MB, cpus %d, jit %zu MB (0 = the VM's default), ecid %s, seed %llx, aux_offset %llu, installer %d, psci_log %d, seconds %.0f\n",
           (unsigned long long)cfg.ram_mb, cfg.ncpus, cfg.jit_bytes >> 20, (tss_probe || pf_ipsw || g0_ipsw) ? "(hidden)" : (snprintf(ecid_txt, sizeof ecid_txt, "%llx", (unsigned long long)cfg.ecid), ecid_txt),
           (unsigned long long)cfg.seed, (unsigned long long)cfg.aux_offset, cfg.installer, psci_log, seconds);
    char err[512] = "";
#ifdef HV_INSTALL_G0
    if (g0_ipsw && hv_g0_begin(cfg.disk, cfg.aux, cfg.ecid) != 0) { printf("vmrun RESULT: install g0 FAILED before the machine was created\n"); return 1; }
#endif
    double t0 = now();
    if (hv_vm_create(&cfg, err, sizeof err) != 0) {
        printf("vmrun RESULT: CREATE FAILED: %s\n", err);
        return 1;
    }
    printf("vmrun created in %.0f ms\n", (now() - t0) * 1e3);
#ifdef HV_TSS_PROBE
    if (tss_probe) {
        HvVmapple *bd2 = hv_vm_board();
        if (bd2 && bd2->vusb) hv_tss_probe_start(bd2->vusb, cfg.ecid);
        else fprintf(stderr, "vmrun: no USB port in this boot mode; --tss-probe has nothing to talk to\n");
    }
#endif
#ifdef HV_RESTORE_PREFLIGHT
    if (pf_ipsw) {
        HvVmapple *bdp = hv_vm_board();
        if (bdp && bdp->vusb) hv_restore_preflight_start(bdp->vusb, pf_ipsw, pf_work, cfg.ecid, pf_stage);
        else fprintf(stderr, "vmrun: no USB port in this boot mode; --restore-preflight has nothing to talk to\n");
    }
#endif
#ifdef HV_IRECV_PROBE
    if (dnload_probe) {
        HvVmapple *bd1 = hv_vm_board();
        if (bd1 && bd1->vusb) hv_dnload_probe_start(bd1->vusb, dnload_a4 ? HV_DNLOAD_A4 : dnload_a3 ? HV_DNLOAD_A3 : HV_DNLOAD_A2);
        else fprintf(stderr, "vmrun: no USB port in this boot mode; --dnload-probe has nothing to talk to\n");
    }
    if (irecv_probe) {
        HvVmapple *bd0 = hv_vm_board();
        if (bd0 && bd0->vusb) hv_irecv_probe_start(bd0->vusb, cfg.ecid);
        else fprintf(stderr, "vmrun: no USB port in this boot mode; --irecv-probe has nothing to open\n");
    }
#endif
    status("start", 0);
    hv_vm_start(1, on_exit_cb);
#ifdef HV_INSTALL_G0
    if (g0_ipsw) {
        HvVmapple *bdg = hv_vm_board();
        int started = bdg && bdg->vusb && hv_g0_start(bdg->vusb, g0_ipsw, pf_work, cfg.ecid) == 0;
        int w = started ? hv_g0_wait(120000) : 1;
        int failed = hv_g0_finish(stdout, cfg.disk, cfg.aux);
        if (w == -1) { printf("vmrun RESULT: install g0 FAILED: the restore thread did not end; the process ends here without closing anything\n"); fflush(stdout); _exit(4); }
        hv_vm_stop();   /* after the table: the thread and file descriptor counts were taken with the machine still running */
        double tsg = now();
        while (!EXITED() && now() - tsg < 5.0) usleep(10000);
        printf("vmrun RESULT: install g0 %s; machine %s\n", failed ? "FAILED" : "passed", EXITED() ? "stopped" : "did not confirm stopping within 5 s");
        return failed ? 1 : EXITED() ? 0 : 3;
    }
#endif
    double t1 = now(), next = 2;
    Sample prev, cur;
    sample(&prev);
    int changes = 0;
    while (!EXITED() && now() - t1 < seconds
#ifdef HV_TSS_PROBE
           && !(tss_probe && hv_tss_probe_finished())
#endif
#ifdef HV_RESTORE_PREFLIGHT
           && !(pf_ipsw && hv_restore_preflight_finished())
#endif
           ) {
        usleep(10000);
        sample(&cur);
        if (changes < 40 && (cur.el != prev.el || cur.esr != prev.esr || cur.vbar != prev.vbar)) {
            changes++;
            printf("vmrun change#%d t=%.3fs pc=%llx el=%u esr_el1=%llx far_el1=%llx elr_el1=%llx vbar_el1=%llx (was el=%u esr_el1=%llx vbar_el1=%llx)\n",
                   changes, now() - t1, cur.pc, cur.el, cur.esr, cur.far, cur.elr, cur.vbar, prev.el, prev.esr, prev.vbar);
        }
        prev = cur;
        if (now() - t1 >= next) {
            status("run", now() - t1);
            next += next < 10 ? 2 : 10;
        }
    }
    int by_us = 0;
    if (!EXITED()) {
        by_us = 1;
        Sample last;
        sample(&last); /* the state just before we stop it */
        printf("vmrun before stop: pc=%llx el=%u esr_el1=%llx far_el1=%llx elr_el1=%llx\n", last.pc, last.el, last.esr, last.far, last.elr);
        hv_vm_stop();
        double ts = now();
        while (!EXITED() && now() - ts < 5.0) usleep(10000);
    }
    status("final", now() - t1);
    if (snap) {
        enum { W = 640, H = 400 };
        static uint8_t px[W * H * 4];
        if (hv_vm_snapshot(px, W, H) == 0 && write_bmp(snap, px, W, H) == 0) printf("vmrun snapshot written: %s\n", snap);
        else printf("vmrun snapshot: nothing to show\n");
    }
    hv_vm_flush_disks();
    if (EXITED()) {
        printf("vmrun RESULT: stopped %s after %.1fs, exit reason %d (%s)\n", by_us ? "by vmrun's time limit" : "by the guest/engine",
               now() - t1, exit_reason, hv_vm_reason_name(exit_reason));
        return 0;
    }
    printf("vmrun RESULT: FAILED: the machine did not confirm stopping within 5 s of hv_vm_stop\n");
    return 3;
}
