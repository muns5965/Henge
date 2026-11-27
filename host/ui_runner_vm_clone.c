/* CLONE BOOT: a sibling of host/ui_runner_vm_installed.c (same frame publishing, same stdin words, same stop idea) for ONE case that binary refuses by design: a normal, non-installer
   boot of a VERIFIED CLONE of an install pair, with the clone's disk and AUX WRITABLE (macOS writes to its Data volume, Preboot and NVRAM even on a plain boot, so a read-only boot cannot
   tell "the installation is incomplete" from "the write was refused").
   What keeps the original safe, all in this file and re-checked here by host/clone_dir.h (the Java launcher checks them too and is not trusted for it):
     - the only files this binary opens for writing are macos_disk.img and macos_aux.img of --clone-dir; there is no --disk/--aux option
     - --clone-dir must be <ecid-dir>/clones/clone-NNN (plain absolute paths) of the SAME vm folder whose vm_ecid.txt is used; the folder is opened O_NOFOLLOW and locked (a second runner is refused); it holds
       only the known files; the members are regular single-link files that are not the original pair's inodes; the marker has exactly the six lines the app writes (clone_complete, source state, disk size
       equal to the file, allocated size, member count equal to the files, ecid_tag) and BOTH the marker and g7_state.txt carry the tag computed here from the ECID read from vm_ecid.txt
     - the files that passed are KEPT OPEN and the machine is given /proc/self/fd/N paths of them: a file swapped for another after the check is not what is written
     - writable storage is set ONLY after those checks passed (clone_boot_config_ok refuses writable storage without them); everything else hv_runner_config_ok guarantees stays (no guest network, installer 0)
     - the ECID is read from <ecid-dir>/vm_ecid.txt here and is never an option (it does not travel on a command line)
   Output: the same status lines as the installed runner, plus memory lines from read-only procfs: g7-process once at start, g7-mem-stage at each big start-up allocation, g7-mem every 10 s, g7-mem-pss on the first sample and then about every 100 s, "FRAME" lines on the stdin word "frame" (a downsampled copy of what the machine shows
   and a pixel-statistics class: an estimate), and a final VERDICT line that separates "the machine stopped (confirmed)" from "macOS booted" (never claimed here).
   host/ui_runner_vm_installed.c is unchanged: it still boots read-only.
   Arguments: --shm PATH --rom F --clone-dir D --ecid-dir D  [--ram-mb N (2048) --cpus N (2) --jit-mb N (64) --seconds N --stop-wait-ms N --usb-mode 0|1|2 (0)]. Stdin: "quit" stops, "status" prints one status line, "frame" prints one FRAME line.
   Exit codes: 0 stopped and confirmed; 1 settings refused or machine not made; 2 bad arguments; 3 stop not confirmed. Output lines start with "ui_runner_vm_clone". */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clone_dir.h"
#include "frame_shm.h"
#include "input_inject.h"
#include "xhci.h"
#include "virtio_input.h"
#include "g7_mem.h"
#include "g7_diag.h"
#include "g7_nvram.h"
#include "pvg_lite.h"
#include "runner_vm_core.h"
#include "usb_host.h"
#include "../core/waitdiag.h"
#include "vm.h"

extern uint64_t hv_net_tx_frames, hv_net_rx_frames;
extern void (*hv_stage_hook)(const char *name);   /* core/machine.h: called after each big start-up allocation */
static void stage_cb(const char *name) {
    HvG7Mem m; hv_g7m_sample(&m);
    char l[200]; hv_g7m_stage_line(l, sizeof l, name, &m);
    fputs(l, stdout); fflush(stdout);
}

static const HvRunnerApi REAL_API = {
    .boot_display = hv_vm_boot_display,
    .pointer = hv_vm_pointer,
    .key = hv_vm_key,
    .mouse_move = hv_vm_mouse_move,
    .mouse_button = hv_vm_mouse_button,
    .mouse_wheel = hv_vm_mouse_wheel,
    .mouse_hwheel = hv_vm_mouse_hwheel,
};

static atomic_int g_exited, g_exit_reason = -1, g_sig;
static void on_exit_cb(int reason) { atomic_store(&g_exit_reason, reason); atomic_store(&g_exited, 1); }
static void on_signal(int s) { atomic_store(&g_sig, s); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* The config check of this binary. Writable storage is allowed only with clone_verified (clone_dir_ok passed); every other guarantee of hv_runner_config_ok stays. */
static int clone_boot_config_ok(const HvVmConfig *c, uint64_t ecid, int clone_verified, char *why, size_t cap) {
#define BAD(msg) do { if (why && cap) snprintf(why, cap, "%s", msg); return -1; } while (0)
    if (!c) BAD("no configuration");
    if (!c->readonly_storage && !clone_verified) BAD("writable storage without a verified clone");
    if (!c->no_net) BAD("the guest network is not off");
    if (c->installer) BAD("installer boot must be OFF (normal boot, not DFU)");
    if (c->usb_probe_on_normal_boot && !c->usb_on_normal_boot) BAD("usb probe requested without the usb device port");
    if (ecid == 0) BAD("no valid ECID in vm_ecid.txt");
    if (c->jit_rx || c->jit_rw) BAD("host-provided JIT memory");
    if (!c->rom || !c->disk || !c->aux || !*c->rom || !*c->disk || !*c->aux) BAD("a path is missing");
    if (c->ram_mb < 64 || c->ram_mb > 4096) BAD("RAM size out of range (64..4096 MB)");
    if (c->ncpus < 1 || c->ncpus > 4) BAD("CPU count out of range (1..4)");
    if (c->jit_bytes < ((size_t)16 << 20) || c->jit_bytes > ((size_t)768 << 20)) BAD("JIT size out of range (16..768 MB)");
    if (why && cap) why[0] = 0;
    return 0;
#undef BAD
}

static HvShm *g_shm;
static HvRunner *g_run;

/* TEMPORARY guest input test (research/g7-input): stdin lines "inject <command>" (host/input_inject.h) become ordinary hv_vm_key / hv_vm_mouse_* calls, i.e. events of the virtual USB keyboard
   and the virtual mouse. Every step is logged with the devices' own counters: xhci "delivered" = keyboard reports written into a transfer buffer the guest's USB driver posted;
   virtio-input "delivered" = events written into an event buffer the guest's driver posted. Those say the guest-side driver took the report; they do NOT say a guest application acted on it. */
static HvInject g_inj;
static int g_inj_on;
static void inj_log(void *ctx, const char *line) {
    (void)ctx;
    HvVmapple *b = hv_vm_board();
    unsigned long long kd = 0, kp = 0, md = 0, mx = 0, mq = 0;
    if (b && b->usb) { kd = __atomic_load_n(&b->usb->delivered, __ATOMIC_RELAXED); kp = (unsigned long long)(b->usb->rtail - b->usb->rhead); }
    if (b && b->mouse) { md = __atomic_load_n(&b->mouse->delivered, __ATOMIC_RELAXED); mx = __atomic_load_n(&b->mouse->dropped, __ATOMIC_RELAXED); mq = (unsigned long long)(b->mouse->tail - b->mouse->head); }
    int px = -1, py = -1, pk = hv_vm_pointer(&px, &py) == 0;
    printf("ui_runner_vm_clone INJECT: %s | kbd reports delivered-to-guest-buffer %llu pending %llu | mouse events delivered-to-guest-buffer %llu pending %llu dropped %llu | guest pointer %s", line, kd, kp, md, mq, mx, pk ? "" : "not reported");
    if (pk) printf("%d,%d", px, py);
    printf(" | guest consumption: unknown\n");
    fflush(stdout);
}
static G7Nv g_nv;   /* NVRAM observer state (host/g7_nvram.h) */
static int cursor_wake_fd = -1;           /* read end of the pointer-moved doorbell pipe (hv_vm_cursor_wake_open), -1 = none */
static unsigned long cursor_wake_loops;   /* loop passes that the doorbell woke (log only) */

/* Disk counters of the root disk and the auxiliary storage, SEPARATELY (counts, bytes, failed/refused writes, flush results, file size and mtime: metadata only, no contents are read or hashed).
   "host accepted the write / the sync returned success" is what these say; they do NOT say that the guest boot device kept anything. */
static const char *flush_name(int f) { return f == HV_FLUSH_OK ? "ok" : f == HV_FLUSH_FAILED ? "FAILED" : f == HV_FLUSH_SKIPPED ? "skipped" : "never"; }
static void disk_counters_line(const char *tag) {
    HvDiskStats r, a;
    hv_vm_disk_stats(&r, &a);
    printf("ui_runner_vm_clone disk-counters %s: root writes %llu bytes %llu refused %llu failed %llu flush %s (calls %llu failed %llu) file_size %llu mtime %llu | "
           "aux writes %llu bytes %llu refused %llu failed %llu flush %s (calls %llu failed %llu) file_size %llu mtime %llu | counts only; not a statement about persistence on the guest boot device\n", tag,
           (unsigned long long)r.write_calls, (unsigned long long)r.write_bytes, (unsigned long long)r.write_refused, (unsigned long long)r.write_failed, flush_name(r.flush_last), (unsigned long long)r.flush_calls, (unsigned long long)r.flush_failed, (unsigned long long)r.file_size, (unsigned long long)r.mtime_s,
           (unsigned long long)a.write_calls, (unsigned long long)a.write_bytes, (unsigned long long)a.write_refused, (unsigned long long)a.write_failed, flush_name(a.flush_last), (unsigned long long)a.flush_calls, (unsigned long long)a.flush_failed, (unsigned long long)a.file_size, (unsigned long long)a.mtime_s);
    fflush(stdout);
}

static void status_line(const char *tag) {
    HvRunnerStats r;
    hv_runner_stats(g_run, &r);
    HvShmStats st;
    int ok = hv_shm_stats(g_shm, &st) == 0;
    uint64_t wc = 0, wr = 0;
    hv_vm_disk_write_stats(&wc, &wr);
    printf("ui_runner_vm_clone %s: pid %d cmds popped %llu dispatched %llu skipped %llu rejected %llu | calls %llu queries %llu | display present %llu absent %llu | "
           "pointer valid %llu absent %llu | call_errors %llu shm_errors %llu | frames published %llu (pvg %llu pvg_surface %llu boot_args %llu unknown %llu) | "
           "ring pushed %llu popped %llu dropped %llu | disk writes asked %llu refused %llu | "
           "net frames tx %llu rx %llu | recovered %u%s\n",
           tag, (int)getpid(), (unsigned long long)r.cmds_popped, (unsigned long long)r.cmds_dispatched, (unsigned long long)r.cmds_skipped, (unsigned long long)r.cmds_rejected,
           (unsigned long long)r.calls_made, (unsigned long long)r.queries_made, (unsigned long long)r.display_present, (unsigned long long)r.display_absent,
           (unsigned long long)r.pointer_valid, (unsigned long long)r.pointer_absent, (unsigned long long)r.call_errors, (unsigned long long)r.shm_errors,
           (unsigned long long)r.frames_published, (unsigned long long)r.frames_from_pvg, (unsigned long long)r.frames_from_pvg_surface,
           (unsigned long long)r.frames_from_boot_args, (unsigned long long)r.frames_from_unknown_source,
           ok ? (unsigned long long)st.cmds_pushed : 0ull, ok ? (unsigned long long)st.cmds_popped : 0ull,
           ok ? (unsigned long long)st.cmds_dropped : 0ull, (unsigned long long)wc, (unsigned long long)wr, (unsigned long long)hv_net_tx_frames, (unsigned long long)hv_net_rx_frames,
           ok ? st.recovered : 0u, ok ? "" : " (stats lock not taken)");
    {   /* log only: the pointer-moved doorbell (+0x1220) and the words the display page held at the last one (x/y = -1: "no pointer"). A separate line: the line above keeps its shape. */
        uint64_t db = 0, chg = 0, wk = 0; int lx = -1, ly = -1, lf = 0;
        int seen = hv_vm_cursor_stats(&db, &chg, &wk, &lx, &ly, &lf);
        printf("ui_runner_vm_clone %s cursor: doorbells %llu changed %llu | wakes %llu loops %lu pipe %s | last page words %s x=%d y=%d flag=%d\n", tag,
               (unsigned long long)db, (unsigned long long)chg, (unsigned long long)wk, cursor_wake_loops, cursor_wake_fd >= 0 ? "open" : "none", seen ? "seen" : "none-yet", lx, ly, lf);
    }
    fflush(stdout);
}

/* One picture of what the machine shows now, read back from the shared file this process itself publishes to (read only: nothing is written anywhere). Downsampled to at most 96 columns and printed
   as hex RGB on ONE line so the app can draw it; plus a coarse class. The class is an ESTIMATE from pixel statistics (black / small bright content / content): it cannot say whether content is the Apple logo,
   a progress bar or a desktop; the picture itself is what to look at. */
static void frame_line(void) {
    size_t cap = HV_SHM_PIXEL_BYTES;
    uint8_t *buf = malloc(cap);
    if (!buf) { printf("ui_runner_vm_clone FRAME none (no memory for a copy)\n"); fflush(stdout); return; }
    uint64_t seen = 0; uint32_t w = 0, h = 0;
    int r = hv_shm_read_frame(g_shm, &seen, buf, cap, &w, &h);
    if (r != 1 || w == 0 || h == 0 || (size_t)w * h * 4 > cap) { printf("ui_runner_vm_clone FRAME none class=no-frame (read=%d)\n", r); fflush(stdout); free(buf); return; }
    uint32_t ow = w < 96 ? w : 96, oh = (uint32_t)((uint64_t)h * ow / w); if (oh < 1) oh = 1; if (oh > 64) oh = 64;
    char *hex = malloc((size_t)ow * oh * 6 + 1);
    if (!hex) { printf("ui_runner_vm_clone FRAME none (no memory for the line)\n"); fflush(stdout); free(buf); return; }
    static uint8_t bins[4096]; memset(bins, 0, sizeof bins);
    size_t k = 0; unsigned long nb = 0, total = (unsigned long)ow * oh;
    for (uint32_t y = 0; y < oh; y++) for (uint32_t x = 0; x < ow; x++) {
        uint64_t sx = (uint64_t)x * w / ow + w / (2 * ow), sy = (uint64_t)y * h / oh + h / (2 * oh);
        if (sx >= w) sx = w - 1;
        if (sy >= h) sy = h - 1;
        const uint8_t *px = buf + (sy * w + sx) * 4;   /* BGRA */
        uint8_t rr = px[2], gg = px[1], bb = px[0];
        if (rr > 24 || gg > 24 || bb > 24) nb++;
        bins[((rr >> 4) << 8) | ((gg >> 4) << 4) | (bb >> 4)] = 1;
        k += (size_t)snprintf(hex + k, 7, "%02x%02x%02x", rr, gg, bb);
    }
    unsigned colors = 0; for (size_t i = 0; i < sizeof bins; i++) colors += bins[i];
    unsigned long pct = total ? nb * 100 / total : 0;
    const char *cls = nb == 0 ? "all-black" : pct < 5 ? "mostly-black-small-content" : colors < 12 ? "few-colors-content" : "many-colors-content";
    printf("ui_runner_vm_clone FRAME src=%ux%u out=%ux%u class=%s nonblack_pct=%lu colors=%u estimate=pixel-statistics-only data=%s\n", w, h, ow, oh, cls, pct, colors, hex);
    fflush(stdout); free(hex); free(buf);
}

int main(int argc, char **argv) {
    const char *shm_path = NULL, *rom = NULL, *clone_dir = NULL, *ecid_dir = NULL; HvClone cl;
    long ram_mb = 2048, cpus = 2, jit_mb = 64, seconds = 0, stop_wait_ms = 5000, usb_mode = 0;   /* usb_mode: 0 = A none, 1 = B device port only, 2 = C port + enumeration (the same two flags host/ui_runner_vm.c sets) */
    
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--shm") && more) shm_path = argv[++i];
        else if (!strcmp(argv[i], "--rom") && more) rom = argv[++i];
        else if (!strcmp(argv[i], "--clone-dir") && more) clone_dir = argv[++i];
        else if (!strcmp(argv[i], "--ecid-dir") && more) ecid_dir = argv[++i];
        else if (!strcmp(argv[i], "--usb-mode") && more) { usb_mode = atol(argv[++i]); if (usb_mode < 0 || usb_mode > 4) { fprintf(stderr, "ui_runner_vm_clone: --usb-mode must be 0, 1, 2, 3 or 4\n"); return 2; } }
        else if (!strcmp(argv[i], "--ram-mb") && more) ram_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cpus") && more) cpus = atol(argv[++i]);
        else if (!strcmp(argv[i], "--jit-mb") && more) jit_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stop-wait-ms") && more) stop_wait_ms = atol(argv[++i]);
        else {
            fprintf(stderr, "ui_runner_vm_clone: unknown or incomplete argument '%s' (accepted: --shm --rom --clone-dir --ecid-dir --ram-mb --cpus --jit-mb --seconds --stop-wait-ms --usb-mode)\n", argv[i]);
            return 2;
        }
    }
    if (!shm_path || !rom || !clone_dir || !ecid_dir || seconds < 0 || stop_wait_ms < 0 || stop_wait_ms > 600000) {
        fprintf(stderr, "ui_runner_vm_clone: --shm --rom --clone-dir --ecid-dir are required and the numbers must be sensible\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    char names[512], left[512];
    int scrubbed = hv_runner_scrub_env(names, sizeof names, left, sizeof left);
    if (scrubbed < 0) {
        fprintf(stderr, "ui_runner_vm_clone: the environment is not clean, HV_* variable(s) left: %s\n", left[0] ? left : "(unknown)");
        printf("ui_runner_vm_clone RESULT: FAILED, HV_* variable(s) could not be removed: %s\n", left[0] ? left : "(unknown)");
        return 1;
    }

    char cwhy[200] = "";
    uint64_t ecid = hv_clone_read_ecid(ecid_dir);
    /* checked AND opened in one step: the machine below is given /proc/self/fd paths of the very files that passed, so a swap after this point changes nothing */
    if (hv_clone_open(clone_dir, ecid_dir, ecid, &cl, cwhy, sizeof cwhy) != 0) {
        fprintf(stderr, "ui_runner_vm_clone: the clone was refused: %s\n", cwhy);
        printf("ui_runner_vm_clone RESULT: FAILED, the clone was refused (%s)\n", cwhy);
        return 1;
    }
    printf("ui_runner_vm_clone: clone folder %s verified (same vm folder, ECID tag equal, marker complete, files opened and locked)\n", cl.real);
    HvVmConfig cfg;
    hv_runner_make_config(&cfg, rom, cl.disk, cl.aux, (uint64_t)(ram_mb < 0 ? 0 : ram_mb), (int)cpus, (size_t)(jit_mb < 0 ? 0 : jit_mb));
    /* [this binary's whole point, reviewed and explicitly approved] hv_runner_make_config leaves these at 0/0
       (host/runner_vm_core.c is unchanged); this binary alone sets them, after every other guarantee that
       function makes (readonly storage, no network, installer=0 -- all untouched above) is already built. */
    cfg.readonly_storage = 0;   /* the clone's disk and AUX are writable, and only because clone_dir_ok passed above; the original install pair is a different folder this binary never names */
    cfg.ecid = ecid;
    cfg.seed = 0;
    /* USB test modes (explicitly approved): 0 = none, 1 = the guest-facing virtual USB device port only, 2 = port plus hv_usb_probe_start's non-destructive enumeration -- exactly what host/ui_runner_vm.c
       sets for its normal boot. installer stays 0 in every mode; this is not host USB and not the restore (usbmuxd/idevicerestore) path. */
    if (usb_mode == 4) {
        /* Mode E (explicitly approved for ONE clone): refused unless the verified clone folder is named exactly clone-001. */
        const char *bn = strrchr(cl.real, '/'); bn = bn ? bn + 1 : cl.real;
        if (strcmp(bn, "clone-001") != 0) {
            fprintf(stderr, "ui_runner_vm_clone: usb mode 4 is approved for clone-001 only\n");
            printf("ui_runner_vm_clone RESULT: FAILED, usb mode 4 is approved for clone-001 only; not started\n");
            return 2;
        }
    }
    cfg.usb_on_normal_boot = usb_mode >= 1;
    cfg.usb_probe_on_normal_boot = usb_mode >= 2;
    /* Mode 3 = mode 2 plus a read-only "getenv" of five recovery variables (host/usb_host.c); no setenv/saveenv/reboot exists anywhere in that path. */
    hv_usb_probe_enable_getenv(usb_mode >= 3);
    /* Mode 4 = mode 3 plus the fixed auto-boot sequence (host/usb_host.c autoboot_fix); the only write this binary can make to the recovery environment. */
    hv_usb_probe_enable_autoboot_fix(usb_mode >= 4);
    /* Diagnostic, default OFF: the display test pattern (devices/pvg_lite.c) is switched on only if the plain file <ecid-dir>/pvg_test_pattern starts with '1' (read-only check; this
       runner scrubs every HV_* variable and takes no new option, so a marker file is the switch). Nothing is written by this runner. */
    {
        char marker[PATH_MAX + 32];
        snprintf(marker, sizeof marker, "%s/pvg_test_pattern", ecid_dir);
        int tp = 0;
        FILE *mf = fopen(marker, "r");   /* ON only if the file exists and its first byte is '1' */
        if (mf) { int c = fgetc(mf); tp = (c == '1'); fclose(mf); }
        hv_pvg_lite_set_test_pattern(tp);
        printf("ui_runner_vm_clone display test pattern: %s\n", tp ? "ON (marker file pvg_test_pattern says 1; the guest's swapped surface is overwritten by a diagnostic picture)" : "off");
    }
    /* Display-events experiment (default OFF). The scrub above removes every HV_* variable, including HV_PVG_DISPLAY_EVENTS, so that name can never reach the device from here
       (the 2026-11-26 21:36 run showed "1 HV_* variable(s) removed: HV_PVG_DISPLAY_EVENTS" and the device stayed off). The app passes a name outside HV_*; it is read AFTER the scrub
       and handed to the device directly. The scrub itself is unchanged. */
    {
        const char *rq = getenv("G7_PVG_DISPLAY_EVENTS");
        int want = rq && *rq && *rq != '0';
        hv_pvg_lite_set_display_events(want);
        int got = hv_pvg_lite_display_events_applied();
        printf("ui_runner_vm_clone display events: requested=%d applied=%d%s\n", want, got, want == got ? "" : "  WARNING: request and applied value differ");
    }
    char why[160], line[400];
    if (clone_boot_config_ok(&cfg, ecid, 1, why, sizeof why) != 0) {
        fprintf(stderr, "ui_runner_vm_clone: the settings were refused: %s\n", why);
        printf("ui_runner_vm_clone RESULT: FAILED, settings refused (%s)\n", why);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (hv_shm_create(shm_path, &g_shm) != 0) { fprintf(stderr, "ui_runner_vm_clone: cannot make the shared file: %s\n", strerror(errno)); return 2; }
    g_run = hv_runner_create(&REAL_API, g_shm);
    if (!g_run) { fprintf(stderr, "ui_runner_vm_clone: out of memory\n"); hv_shm_set_state(g_shm, HV_SHM_STOPPED); return 2; }

    printf("ui_runner_vm_clone: started pid %d, verified-clone virtual machine mode (normal boot, writable clone, nonzero machine identity)\n", (int)getpid());
    hv_runner_config_line(&cfg, line, sizeof line);
    printf("ui_runner_vm_clone config (the structure passed to hv_vm_create): %s\n", line);
    printf("ui_runner_vm_clone usb mode %ld (%s): usb_on_normal_boot=%d usb_probe_on_normal_boot=%d installer=%d\n", usb_mode, usb_mode == 0 ? "A none" : usb_mode == 1 ? "B port only" : usb_mode == 2 ? "C port+enumeration" : usb_mode == 3 ? "D port+enumeration+getenv(read-only)" : "E clone-001 only: getenv + fixed setenv auto-boot true + saveenv",
           cfg.usb_on_normal_boot, cfg.usb_probe_on_normal_boot, cfg.installer);
    printf("ui_runner_vm_clone env: %d HV_* variable(s) removed%s%s\n", scrubbed, scrubbed ? ": " : "", scrubbed ? names : "");
    fflush(stdout);

    printf("g7-process: runner_pid=%ld\n", (long)getpid());
    hv_stage_hook = stage_cb;   /* from here the start-up stages report their memory */
    char err[256] = "";
    double t0 = now();
    cursor_wake_fd = hv_vm_cursor_wake_open();   /* before the machine: the doorbell listener reads the fd */
    if (hv_vm_create(&cfg, err, sizeof err) != 0) {
        printf("ui_runner_vm_clone RESULT: FAILED, the machine could not be made: %s\n", err);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    hv_vm_machine()->stats_interval = 10;   /* engine's own periodic "stats" lines (stderr: WFI idle %, how long the CPU thread ran, blocks chained, retranslation, JIT use); hv_vm_create clears it, so it is set here, before the vCPUs start */
    HvVmapple *bd = hv_vm_board();
    int usb_present = bd && bd->vusb ? 1 : 0;
    printf("ui_runner_vm_clone: machine created in %.0f ms; usb device port present: %d (want %d for usb mode %ld)\n", (now() - t0) * 1e3, usb_present, cfg.usb_on_normal_boot, usb_mode);
    disk_counters_line("at process start (per-process counters begin at 0; file size/mtime are what the previous process left)");
    if (!bd || usb_present != cfg.usb_on_normal_boot) {
        printf("ui_runner_vm_clone RESULT: FAILED, usb device port present=%d, expected %d for usb mode %ld; not started\n", usb_present, cfg.usb_on_normal_boot, usb_mode);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }

    /* NVRAM diagnostics (host/g7_nvram.h). The guest's NVRAM lives inside the aux image, so there is no NVRAM device to hook: the observer below looks at the aux disk's reads, writes and flushes (read-only,
       never changes a request) and prints at most 200 "g7-nvram:" lines plus one summary at stop. Default ON (light); G7_NVRAM_LOG=0 turns it off. The opt-in pre-seed (default OFF) runs BEFORE the
       observer is installed and BEFORE the machine starts; it writes only when a valid v3 store is found in the aux image, and only to the clone's own aux disk (the same writable file the guest uses).
       Requested by G7_NVRAM_PRESEED or HENGE_NVRAM_PRESEED (neither is an HV_* name, so the scrub does not touch them) or by the marker file <ecid-dir>/nvram_preseed (the boot dialog's checkbox writes it). */
    {
        const char *lg = getenv("G7_NVRAM_LOG");
        g7nv_init(&g_nv, stdout, !(lg && lg[0] == '0'));
        char ftxt[512] = "", spec[512] = "", mp[PATH_MAX + 32];
        int have_file = 0;
        snprintf(mp, sizeof mp, "%s/nvram_preseed", ecid_dir);
        FILE *mf = fopen(mp, "r");
        if (mf) { size_t nr = fread(ftxt, 1, sizeof ftxt - 1, mf); ftxt[nr] = 0; fclose(mf); have_file = 1; }
        const char *src = g7nv_spec_source(getenv("G7_NVRAM_PRESEED"), getenv("HENGE_NVRAM_PRESEED"), have_file ? ftxt : NULL, spec, sizeof spec);
        G7NvSeed items[G7NV_SEED_MAX]; char perr[96] = ""; int ni = src ? g7nv_parse_spec(spec, items, G7NV_SEED_MAX, perr, sizeof perr) : 0;
        printf("g7-nvram: log %s (cap %d lines + 1 summary; default ON, G7_NVRAM_LOG=0 turns it off) | preseed %s\n", g_nv.on ? "ON" : "OFF", G7NV_LINE_CAP,
               !src ? "OFF (default, nothing requested)" : ni < 0 ? "REFUSED (bad request, nothing written)" : ni == 0 ? "OFF (request says off)" : "REQUESTED");
        if (src && ni < 0) printf("g7-nvram: preseed request from %s refused: %s\n", src, perr);
        if (src && ni > 0) {
            printf("g7-nvram: preseed requested via %s: %d item(s):", src, ni);
            for (int q = 0; q < ni; q++) printf(" %s=<%zu bytes>", items[q].name, items[q].vlen);   /* names and sizes only; the values are in the result lines */
            printf("\n"); fflush(stdout);
            HvDisk *ad = hv_vm_aux_disk();
            if (!ad || ad->readonly) printf("g7-nvram: preseed skipped: the aux disk is not writable here; nothing written\n");
            else (void)g7nv_preseed(ad, items, ni, stdout);
        }
        fflush(stdout);
        if (g_nv.on && hv_vm_aux_disk()) hv_disk_set_trace(hv_vm_aux_disk(), g7nv_trace, &g_nv);
    }

    hv_vm_start(1, on_exit_cb);
    if (hv_vm_start_failed()) {   /* a vCPU thread could not be created: the machine is not whole, do not report it as running */
        printf("ui_runner_vm_clone RESULT: FAILED, the machine could not start all its threads; not running\n");
        hv_vm_stop();
        double tw = now(); while (!atomic_load(&g_exited) && (now() - tw) * 1000 < (double)stop_wait_ms) usleep(2000);
        if (atomic_load(&g_exited)) (void)hv_vm_flush_disks_checked();
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    hv_runner_set_ready(g_run, 1);
    { HvInputSink isk = {hv_vm_key, hv_vm_mouse_move, hv_vm_mouse_button, hv_vm_mouse_wheel, hv_vm_mouse_hwheel}; hv_inject_init(&g_inj, &isk, inj_log, NULL); g_inj_on = 1; }
    hv_shm_set_state(g_shm, HV_SHM_RUNNING);

    const char *why_stop = "quit", *stop_kind = "requested-quit";
    char reason_txt[700];   /* outlives the loop: why_stop may point at it after the break */
    double tstart = now(), next_frame = 0, next_mem = 0; unsigned nmem = 0;
    HvG7DPrev diag_prev; hv_g7d_prev_init(&diag_prev);
    char inl[256]; size_t ll = 0; long ignored = 0;
    for (;;) {
        if (atomic_load(&g_sig)) { why_stop = "signal"; stop_kind = "signal"; break; }
        if (hv_shm_app_closed(g_shm)) { why_stop = "app closed"; stop_kind = "app-closed"; break; }
        if (atomic_load(&g_exited)) {
            int er = atomic_load(&g_exit_reason);
            uint32_t pfn = 0, pcpu = 0; uint64_t ppc = 0, plr = 0;
            /* Same read-only accessors host/ui_runner_vm.c uses: a PSCI SYSTEM_OFF/SYSTEM_RESET halt says which call, which cpu, pc and lr, and the raw stack words
               (nothing is interpreted here). Without a PSCI record the line stays as before. */
            if (hv_vm_psci_halt_info(&pfn, &pcpu, &ppc, &plr)) {
                int rn = snprintf(reason_txt, sizeof reason_txt, "the machine stopped itself, reason %d (%s), psci %s (fn=%#x) by cpu%u at pc=%#llx lr=%#llx",
                                  er, hv_vm_reason_name(er), pfn == 0x84000008 ? "SYSTEM_OFF" : "SYSTEM_RESET", pfn, pcpu, (unsigned long long)ppc, (unsigned long long)plr);
                uint64_t psp = 0, px26 = 0, pstack[HV_PSCI_HALT_STACK_WORDS]; int pn = 0;
                if (rn > 0 && (size_t)rn < sizeof reason_txt && hv_vm_psci_halt_stack(&psp, &px26, pstack, &pn)) {
                    char words[HV_PSCI_HALT_STACK_WORDS * 17 + 1] = ""; size_t wo = 0;
                    for (int i = 0; i < pn && wo + 18 < sizeof words; i++)
                        wo += (size_t)snprintf(words + wo, sizeof words - wo, "%s%016llx", i ? "," : "", (unsigned long long)pstack[i]);
                    snprintf(reason_txt + rn, sizeof reason_txt - (size_t)rn, " sp=%#llx x26=%#llx stack(sp+0..sp+%#x)=%s", (unsigned long long)psp, (unsigned long long)px26, pn * 8, words);
                }
            } else
                snprintf(reason_txt, sizeof reason_txt, "the machine stopped itself, reason %d (%s) [no PSCI record: stop address or other]", er, hv_vm_reason_name(er));
            why_stop = reason_txt; stop_kind = "guest-initiated"; break;
        }
        if (seconds && now() - tstart >= (double)seconds) { why_stop = "time limit"; stop_kind = "time-limit"; break; }
        /* fd 0 is the app's command line; the second fd (when the pipe could be made) is the pointer-moved doorbell: the loop wakes at once and the pump below asks for the pointer. */
        struct pollfd pf[2] = {{0, POLLIN, 0}, {cursor_wake_fd, POLLIN, 0}};
        struct pollfd p;
        int r = poll(pf, cursor_wake_fd >= 0 ? 2 : 1, 10);
        p = pf[0];
        if (r < 0 && errno != EINTR) { why_stop = "input error"; stop_kind = "input-error"; break; }
        if (r > 0 && cursor_wake_fd >= 0 && (pf[1].revents & POLLIN)) { hv_vm_cursor_wake_drain(); cursor_wake_loops++; r = (p.revents & (POLLIN | POLLHUP | POLLERR)) ? r : 0; }   /* the pointer is asked for at work: just below, before anything else */
        if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            char c;
            ssize_t n = read(0, &c, 1);
            if (n == 0) { why_stop = "input closed"; stop_kind = "input-closed"; break; }
            if (n < 0) { if (errno == EINTR || errno == EAGAIN) goto work; why_stop = "input error"; stop_kind = "input-error"; break; }
            if (c == '\n') {
                inl[ll] = 0; ll = 0;
                if (!strcmp(inl, "quit")) { why_stop = "quit"; break; }
                else if (!strcmp(inl, "status")) status_line("status");
                else if (!strcmp(inl, "frame")) frame_line();
                else if (!strncmp(inl, "inject ", 7)) {
                    char im[120] = "";
                    int ir = g_inj_on ? hv_inject_command(&g_inj, inl + 7, (int64_t)(now() * 1000), im, sizeof im) : -1;
                    printf("ui_runner_vm_clone INJECT-CMD: '%s' -> %s%s%s\n", inl + 7, ir == 0 ? "accepted" : "refused", im[0] ? ": " : "", g_inj_on ? im : "machine not running"); fflush(stdout);
                }
                else ignored++;
            } else if (ll + 1 < sizeof inl) inl[ll++] = c;
        }
    work:
        if (g_inj_on) { int ix = 0, iy = 0, ik = hv_vm_pointer(&ix, &iy) == 0; hv_inject_tick(&g_inj, ik, ix, iy, (int64_t)(now() * 1000)); }
        hv_runner_pump_cmds(g_run);
        hv_runner_pump_pointer(g_run);
        double t = now();
        if (t >= next_frame) { hv_runner_pump_frame(g_run); next_frame = t + 0.1; }
        hv_shm_heartbeat(g_shm);
        if (t >= next_mem) {   /* one memory line every 10 s (read-only procfs), the slower PSS on the first and every 10th */
            HvG7Mem m; hv_g7m_sample(&m); char ml[HV_G7M_LINE_MAX]; hv_g7m_line(ml, sizeof ml, (uint64_t)(t - tstart), &m); fputs(ml, stdout);
            if (nmem == 0 || nmem % 10 == 9) { char pl[100]; hv_g7m_pss_line(pl, sizeof pl, (uint64_t)(t - tstart), hv_g7m_pss_kb()); fputs(pl, stdout); }
            char d1[900], d2[1800]; hv_g7d_sample_lines(&diag_prev, (uint64_t)(t - tstart), d1, sizeof d1, d2, sizeof d2); fputs(d1, stdout); fputs(d2, stdout);   /* read-only: vmstat, PSI, swaps, one line per thread */
            {   /* read-only wait-site timing (core/waitdiag.h): why a stalled boot stalled. wd: one line per wait site, wd-cpu: one per vCPU (last pc / exit reason / which wait it is in), gfx-ch: the gfx channels' ring pointers */
                static char wd[6000], gl[1200];
                size_t wl = hv_wd_report(wd, sizeof wd, (uint64_t)(t - tstart), hv_vm_machine()); if (wl) fputs(wd, stdout);
                size_t gn = hv_vm_gfx_diag(gl, sizeof gl); if (gn) fputs(gl, stdout);
            }
            {   /* g7-jit (research/g7-hjit2): the JIT region size and the whole-flush counters of this 10 s window next to the process's rss/swap/major faults, so a JIT-size A/B reads from one line. Only numbers the loop already has (HvG7Mem above, the engine's counters) plus one getrusage(); no extra sampling. Format: docs/g7-analysis-notes.md. */
                static uint64_t pf = 0, pfull = 0, pdrop = 0, pmaj = 0; struct rusage ru; uint64_t maj = getrusage(RUSAGE_SELF, &ru) == 0 ? (uint64_t)ru.ru_majflt : 0;
                const HvStats *js = &hv_vm_machine()->stats;
                char jr[24], jw[24]; hv_g7m_mib(jr, sizeof jr, m.rss_kb); hv_g7m_mib(jw, sizeof jw, m.swap_kb);
                printf("g7-jit: t=%llus jit_mb=%zu used_mb=%.0f flushes=%llu(+%llu) full=%llu(+%llu) dropped_blocks=%llu(+%llu) emit_fail=%llu table_full=%llu rss=%s swap=%s majflt=%llu(+%llu)\n", (unsigned long long)(t - tstart), cfg.jit_bytes >> 20, (double)hv_vm_machine()->jit.used * 4 / (1 << 20),
                       (unsigned long long)js->block_flushes, (unsigned long long)(js->block_flushes - pf), (unsigned long long)js->flush_full, (unsigned long long)(js->flush_full - pfull), (unsigned long long)js->flushed_blocks, (unsigned long long)(js->flushed_blocks - pdrop),
                       (unsigned long long)js->flush_emit_fail, (unsigned long long)js->flush_table_full, jr, jw, (unsigned long long)maj, (unsigned long long)(maj - pmaj));
                pf = js->block_flushes; pfull = js->flush_full; pdrop = js->flushed_blocks; pmaj = maj;
            }
            fflush(stdout); nmem++; next_mem = t + 10.0;
        }
    }

    hv_shm_set_state(g_shm, HV_SHM_STOPPING);
    hv_runner_set_ready(g_run, 0);
    hv_runner_close_display(g_run);
    hv_vm_save_last_frame();   /* the newest PVG surface, as a file next to the GPU dump (a worker thread writes it; nothing happens without LAGFX_ARM64_DUMP_DIR) */
    hv_vm_frames_flush();
    if (!atomic_load(&g_exited)) hv_vm_stop();
    double ts = now();
    while (!atomic_load(&g_exited) && (now() - ts) * 1000 < (double)stop_wait_ms) usleep(2000);
    int confirmed = atomic_load(&g_exited);
    int flush_rc = confirmed && hv_vm_quiesced_ok() == 1 ? hv_vm_flush_disks_checked() : -2;   /* -2 also when the other threads or the disk workers did not end: the flush alone then proves nothing */   /* -2: not attempted (the machine's stop was not confirmed) */
    hv_shm_set_state(g_shm, HV_SHM_STOPPED);
    disk_counters_line(confirmed ? "at stop (after the final flush; a guest SYSTEM_RESET stop included)" : "at stop (stop NOT confirmed)");
    g7nv_summary(&g_nv);   /* one line: what the guest asked of the aux disk (where its NVRAM lives) during this run */
    status_line("final");
    printf("ui_runner_vm_clone RESULT: %s (%s), ignored input lines %ld\n", confirmed ? "stopped, confirmed" : "STOP NOT CONFIRMED within the wait; ending the process anyway", why_stop, ignored);
    /* Structured end result for the app (appended AFTER macos_boot=..., so the app's older pattern, which ends in ".*$", still matches). outcome says what the app may do next: only a
       confirmed guest SYSTEM_RESET is "restart"; everything the host ended, an unconfirmed stop, or a halt without a PSCI record is not. */
    uint32_t vfn = 0, vcpu = 0; uint64_t vpc = 0, vlr = 0;
    int have_psci = hv_vm_psci_halt_info(&vfn, &vcpu, &vpc, &vlr);
    const char *psci_name = !have_psci ? "none" : vfn == 0x84000008 ? "SYSTEM_OFF" : "SYSTEM_RESET";
    int guest_end = !strcmp(stop_kind, "guest-initiated");
    const char *outcome = !confirmed ? "not-confirmed" : !guest_end ? "host-stop" : !have_psci ? "halt-other" : vfn == 0x84000008 ? "shutdown" : "restart";
    int qok = hv_vm_quiesced_ok();
    uint64_t wcalls = 0, wrefused = 0; hv_vm_disk_write_stats(&wcalls, &wrefused);
    HvDiskStats vr, va; hv_vm_disk_stats(&vr, &va);   /* the split counters: NEW optional trailing fields of the VERDICT line, after disk_write_refused= */
    printf("ui_runner_vm_clone VERDICT: vm_stop_confirmed=%d stop_kind=%s disk_flush=%s macos_boot=not-determined outcome=%s psci=%s quiesced=%s uptime_s=%d disk_writes=%llu disk_write_refused=%llu root_writes=%llu root_write_bytes=%llu root_write_refused=%llu root_write_failed=%llu root_flush=%s root_flush_calls=%llu root_flush_failed=%llu aux_writes=%llu aux_write_bytes=%llu aux_write_refused=%llu aux_write_failed=%llu aux_flush=%s aux_flush_calls=%llu aux_flush_failed=%llu (the root_ and aux_ fields are host-side counts of the writes the guest asked for and of the sync results; they do not prove that the guest boot device or the auxiliary storage kept anything. a confirmed stop says only that the machine ended; it does not say macOS booted, and it is not a flush result; quiesced=1 means every CPU thread and the kicker were joined and the disk workers stopped before the flush)\n",
           confirmed ? 1 : 0, stop_kind, flush_rc == 0 ? "ok" : flush_rc == -2 ? "not-attempted" : "FAILED",
           outcome, psci_name, qok == 1 ? "1" : qok == 0 ? "0" : "n/a", (int)(now() - tstart), (unsigned long long)wcalls, (unsigned long long)wrefused,
           (unsigned long long)vr.write_calls, (unsigned long long)vr.write_bytes, (unsigned long long)vr.write_refused, (unsigned long long)vr.write_failed, flush_name(vr.flush_last), (unsigned long long)vr.flush_calls, (unsigned long long)vr.flush_failed,
           (unsigned long long)va.write_calls, (unsigned long long)va.write_bytes, (unsigned long long)va.write_refused, (unsigned long long)va.write_failed, flush_name(va.flush_last), (unsigned long long)va.flush_calls, (unsigned long long)va.flush_failed);
    fflush(stdout);
    /* Only a machine whose threads and disk workers all ended is torn down here. When quiesce failed a thread or a disk worker may still use the runner, the shared state and the clone's
       descriptors: they are left as they are and the process end (immediately below) releases them, so nothing is freed or unlocked under a live user. */
    if (confirmed && qok == 1) { hv_runner_destroy(g_run); hv_shm_close(g_shm); hv_clone_close(&cl); }
    return confirmed ? 0 : 3;
}
