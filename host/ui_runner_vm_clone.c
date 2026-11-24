/* CLONE BOOT: a sibling of host/ui_runner_vm_installed.c (same frame publishing, same stdin words, same stop idea) for ONE case that binary refuses by design: a normal, non-installer
   boot of a VERIFIED CLONE of an install pair, with the clone's disk and AUX WRITABLE (macOS writes to its Data volume, Preboot and NVRAM even on a plain boot, so a read-only boot cannot
   tell "the installation is incomplete" from "the write was refused").
   What keeps the original safe, all in this file and re-checked here (the Java launcher checks them too and is not trusted for it):
     - the only paths this binary opens for writing are <clone-dir>/macos_disk.img and <clone-dir>/macos_aux.img, built here from --clone-dir; there is no --disk/--aux option
     - --clone-dir must be an absolute path whose last two names are clones/clone-NNN; it is opened with O_NOFOLLOW, its members are checked with AT_SYMLINK_NOFOLLOW: regular files, one hard link,
       no *.partial left, the marker clone_state.txt starts with state=clone_complete and its logical_disk_bytes equals the disk's size, g7_state.txt carries an ecid_tag line
     - writable storage is set ONLY after those checks passed (clone_boot_config_ok refuses writable storage without them); everything else hv_runner_config_ok guarantees stays (no guest network, installer 0)
     - the ECID is read from <ecid-dir>/vm_ecid.txt here and is never an option (it does not travel on a command line); the equality with the clone's ecid_tag is checked by the launcher
   host/ui_runner_vm_installed.c is unchanged: it still boots read-only.
   Arguments: --shm PATH --rom F --clone-dir D --ecid-dir D  [--ram-mb N (2048) --cpus N (2) --jit-mb N (64) --seconds N --stop-wait-ms N]. Stdin: "quit" stops, "status" prints one status line.
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
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clone_dir.h"
#include "frame_shm.h"
#include "runner_vm_core.h"
#include "vm.h"

extern uint64_t hv_net_tx_frames, hv_net_rx_frames;

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
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *shm_path = NULL, *rom = NULL, *clone_dir = NULL, *ecid_dir = NULL; char disk[PATH_MAX], aux[PATH_MAX];
    long ram_mb = 2048, cpus = 2, jit_mb = 64, seconds = 0, stop_wait_ms = 5000;
    
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--shm") && more) shm_path = argv[++i];
        else if (!strcmp(argv[i], "--rom") && more) rom = argv[++i];
        else if (!strcmp(argv[i], "--clone-dir") && more) clone_dir = argv[++i];
        else if (!strcmp(argv[i], "--ecid-dir") && more) ecid_dir = argv[++i];
        else if (!strcmp(argv[i], "--ram-mb") && more) ram_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cpus") && more) cpus = atol(argv[++i]);
        else if (!strcmp(argv[i], "--jit-mb") && more) jit_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stop-wait-ms") && more) stop_wait_ms = atol(argv[++i]);
        else {
            fprintf(stderr, "ui_runner_vm_clone: unknown or incomplete argument '%s' (accepted: --shm --rom --clone-dir --ecid-dir --ram-mb --cpus --jit-mb --seconds --stop-wait-ms)\n", argv[i]);
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
    if (hv_clone_dir_ok(clone_dir, disk, sizeof disk, aux, sizeof aux, cwhy, sizeof cwhy) != 0) {
        fprintf(stderr, "ui_runner_vm_clone: the clone was refused: %s\n", cwhy);
        printf("ui_runner_vm_clone RESULT: FAILED, the clone was refused (%s)\n", cwhy);
        return 1;
    }
    uint64_t ecid = hv_clone_read_ecid(ecid_dir);
    HvVmConfig cfg;
    hv_runner_make_config(&cfg, rom, disk, aux, (uint64_t)(ram_mb < 0 ? 0 : ram_mb), (int)cpus, (size_t)(jit_mb < 0 ? 0 : jit_mb));
    /* [this binary's whole point, reviewed and explicitly approved] hv_runner_make_config leaves these at 0/0
       (host/runner_vm_core.c is unchanged); this binary alone sets them, after every other guarantee that
       function makes (readonly storage, no network, installer=0 -- all untouched above) is already built. */
    cfg.readonly_storage = 0;   /* the clone's disk and AUX are writable, and only because clone_dir_ok passed above; the original install pair is a different folder this binary never names */
    cfg.ecid = ecid;
    cfg.seed = 0;
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
    printf("ui_runner_vm_clone env: %d HV_* variable(s) removed%s%s\n", scrubbed, scrubbed ? ": " : "", scrubbed ? names : "");
    fflush(stdout);

    char err[256] = "";
    double t0 = now();
    if (hv_vm_create(&cfg, err, sizeof err) != 0) {
        printf("ui_runner_vm_clone RESULT: FAILED, the machine could not be made: %s\n", err);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    HvVmapple *bd = hv_vm_board();
    int usb_present = bd && bd->vusb ? 1 : 0;
    printf("ui_runner_vm_clone: machine created in %.0f ms; usb device port present: %d (want 0, normal boot without the usb_on_normal_boot diagnostic)\n", (now() - t0) * 1e3, usb_present);
    if (!bd || usb_present) {
        printf("ui_runner_vm_clone RESULT: FAILED, usb device port present=%d, expected 0 for this binary; not started\n", usb_present);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }

    hv_vm_start(1, on_exit_cb);
    hv_runner_set_ready(g_run, 1);
    hv_shm_set_state(g_shm, HV_SHM_RUNNING);

    const char *why_stop = "quit";
    double tstart = now(), next_frame = 0;
    char inl[256]; size_t ll = 0; long ignored = 0;
    for (;;) {
        if (atomic_load(&g_sig)) { why_stop = "signal"; break; }
        if (hv_shm_app_closed(g_shm)) { why_stop = "app closed"; break; }
        if (atomic_load(&g_exited)) {
            int er = atomic_load(&g_exit_reason);
            char reason_txt[160];
            snprintf(reason_txt, sizeof reason_txt, "the machine stopped itself, reason %d (%s)", er, hv_vm_reason_name(er));
            why_stop = reason_txt; break;
        }
        if (seconds && now() - tstart >= (double)seconds) { why_stop = "time limit"; break; }
        struct pollfd p = {0, POLLIN, 0};
        int r = poll(&p, 1, 10);
        if (r < 0 && errno != EINTR) { why_stop = "input error"; break; }
        if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            char c;
            ssize_t n = read(0, &c, 1);
            if (n == 0) { why_stop = "input closed"; break; }
            if (n < 0) { if (errno == EINTR || errno == EAGAIN) goto work; why_stop = "input error"; break; }
            if (c == '\n') {
                inl[ll] = 0; ll = 0;
                if (!strcmp(inl, "quit")) { why_stop = "quit"; break; }
                else if (!strcmp(inl, "status")) status_line("status");
                else ignored++;
            } else if (ll + 1 < sizeof inl) inl[ll++] = c;
        }
    work:
        hv_runner_pump_cmds(g_run);
        hv_runner_pump_pointer(g_run);
        double t = now();
        if (t >= next_frame) { hv_runner_pump_frame(g_run); next_frame = t + 0.1; }
        hv_shm_heartbeat(g_shm);
    }

    hv_shm_set_state(g_shm, HV_SHM_STOPPING);
    hv_runner_set_ready(g_run, 0);
    hv_runner_close_display(g_run);
    if (!atomic_load(&g_exited)) hv_vm_stop();
    double ts = now();
    while (!atomic_load(&g_exited) && (now() - ts) * 1000 < (double)stop_wait_ms) usleep(2000);
    int confirmed = atomic_load(&g_exited);
    if (confirmed) hv_vm_flush_disks();
    hv_shm_set_state(g_shm, HV_SHM_STOPPED);
    status_line("final");
    printf("ui_runner_vm_clone RESULT: %s (%s), ignored input lines %ld\n", confirmed ? "stopped, confirmed" : "STOP NOT CONFIRMED within the wait; ending the process anyway", why_stop, ignored);
    fflush(stdout);
    if (confirmed) { hv_runner_destroy(g_run); hv_shm_close(g_shm); }
    return confirmed ? 0 : 3;
}
