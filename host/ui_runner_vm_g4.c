/* A TEST BUILD ONLY (tools/ui-runner-vm-g4-install-build.sh, -DHV_INSTALL_G4; nothing shipped sets that flag): host/ui_runner_vm.c's frame-sharing
   runner, combined with the already-proven G4 restore path (host/install_g4.c/.h) instead of host/usb_host.c's bounded enumeration probe.

   This exists to answer one question: if G4's real-device-proven DFU->Recovery->RecoveryOS sequence (TSS personalization, firmware transfer,
   AppleLogo + boot components, RecoveryOS entry, stop C -- see host/install_g4.h for exactly what that does and does not do) is driven against the
   SAME virtual machine object this binary's frame-publishing loop (host/runner_vm_core.c's hv_runner_pump_frame, the same one host/ui_runner_vm.c
   uses) is reading from, do real PVG frames reach the shared-memory file this binary's caller displays? host/vmrun.c's own G4 build already proved
   the restore sequence itself works through a real device's DFU port; it never answered this display question because it has no frame-sharing loop
   at all -- it blocks in hv_g4_wait(), then takes exactly one post-hoc snapshot (g_capture_screen) and exits.

   WHAT IS DIFFERENT FROM host/ui_runner_vm.c (which this binary otherwise matches line for line in spirit):
     - installer=1 and a caller-supplied, nonzero ecid (host/vm.h's "the machine identity the disk was installed with") -- host/ui_runner_vm.c forces
       both to 0 and that is still true THERE; this is a second, separate binary, not a change to it.
     - its own config check (g4ui_config_ok below), NOT host/runner_vm_core.c's hv_runner_config_ok -- that function's job is to BE the installer=0/
       ecid=0 guarantee for ui_runner_vm.c and every other caller of it; reusing it here would make it lie for this one caller, so this binary does not
       call it, and nothing about hv_runner_config_ok itself is touched. Every OTHER guarantee it makes (read-only storage, no guest network, no
       host-provided JIT memory, sane RAM/CPU/JIT ranges, required paths) is re-checked here, unchanged.
     - no usb_on_normal_boot/usb_probe_on_normal_boot/panic-probe wiring: those exist in host/ui_runner_vm.c for a NON-installer boot; this binary is
       an installer boot (the normal case devices/vmapple.c already handles), so none of that applies and none of it is carried over.
     - a default RAM of 2048 MiB (host/vmrun.c's own proven G4 setting; host/ui_runner_vm.c's 256 MiB default is for a non-installer boot's much
       smaller footprint and was never run against G4).
     - the G4 run and the frame-publishing loop run concurrently (host/vmrun.c's G4 runs them strictly in sequence: start -> block in hv_g4_wait() ->
       one snapshot -> stop). hv_install_start (host/install.c, what hv_g4_start calls into after its own up-to-60s USB-online wait) already runs the
       restore on its own pthread and publishes completion through hv_install_result(), polled by hv_g4_wait(); this binary keeps that exact contract
       (reviewed and confirmed: hv_g4_finish's pass row requires g_waited==1, which ONLY hv_g4_wait sets -- polling hv_install_result() directly from
       this binary's own loop instead of calling hv_g4_wait would leave g_waited at its initial -2 and make hv_g4_finish report a false FAILED even
       after a real success) by running hv_g4_wait() on its OWN dedicated thread (g4_control_thread below), so it can block for its full budget
       without blocking this binary's 100ms frame-pump cadence or its ability to notice a user "quit"/signal/time-limit promptly.
     - shutdown ownership: this binary's main loop is the ONLY caller of hv_install_cancel() for an early stop (guarded by g_cancel_sent so it is
       sent at most once, never racing hv_g4_wait()'s own internal "ask it to stop once if it does not end" after its cap_ms -- that happens only
       AFTER cap_ms, by which point this binary's own 450s --seconds default has already triggered its own early-stop attempt or the run finished).
       Shutdown order (reviewed): stop requested (quit/signal/app-closed/time-limit/G4-thread-done) -> hv_install_cancel() at most once -> pthread_join
       the control thread (it has already called hv_g4_wait() and, per the next point, hv_g4_finish() internally) -> stop frame supply -> hv_vm_stop()
       -> publish STOPPED. hv_g4_finish() is called from INSIDE g4_control_thread, strictly after hv_g4_wait() returns on that same thread -- never
       from the main thread while the control thread might still be running -- so the disk/aux hash check and the usb/network counters it reports are
       never read mid-run.
     - restored_start_restore() (third_party/idevicerestore/src/restore.c) IS linked into this binary, exactly as it already is into vmrun_g4: G4's
       protection is NOT link-time absence (reviewed and corrected from an earlier draft of this comment) but two independent control-flow stops --
       install_g4.c's own FLAG_STOP_AFTER_RECOVERYOS_ENTRY and the vendored tool's separate, independent HV_STOP_AFTER_RECOVERYOS_ENTRY compiled into
       it -- both already proven by host/vmrun.c's own G4 build. hv_g4_finish()'s verdict table is a POST-HOC check of where the run actually stopped,
       not a preventive one; this binary adds no new preventive mechanism of its own and relies on exactly the same two G4 already has.
     - disk/aux stay read-only (g4ui_config_ok requires it, same as hv_runner_config_ok); hv_g4_begin()/hv_g4_finish() additionally hash-verify them
       unchanged before and after, same as every G0-G6 diagnostic build.

   This binary makes a REAL request to Apple's signing server (the same three TSS hosts G2-G6 already use) every time it actually runs with a real
   IPSW and ECID -- that is G4's whole point, already reviewed and approved per the project's standing rule that this needs fresh confirmation each
   time it is actually invoked, not just compiled. Building this file does not invoke anything; running it does.

   Arguments: --shm PATH --rom F --disk F --aux F --ipsw F --ecid HEX (all required, ecid must be nonzero) --workdir DIR (default ".")
   --ram-mb N (default 2048) --cpus N (default 2) --jit-mb N (default 64) --seconds N (overall process budget; default 450, i.e. G4's own 380s+20s
   margin plus 50s for this binary's own startup/shutdown) --stop-wait-ms N (default 5000, same meaning as host/ui_runner_vm.c's).
   Standard input: "quit" or end of input = stop; "status" = one status line. Exit codes: 0 stopped and confirmed, G4 passed; 1 settings refused, the
   machine could not be made, or G4 failed; 2 bad arguments or the shared file could not be made; 3 the stop was NOT confirmed within the wait.
   Output: "ui_runner_vm_g4: ..." lines, the same "ui_runner_vm_g4 <tag>: ..." status line host/ui_runner_vm.c prints (same stats), and one
   "ui_runner_vm_g4 RESULT: ..." at the end. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "frame_shm.h"
#include "install_g4.h"
#include "runner_vm_core.h"
#include "vm.h"

extern uint64_t hv_net_tx_frames, hv_net_rx_frames;
void hv_install_cancel(void); /* host/install.h; declared again here rather than pulling that whole header in for one call */

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

/* This binary's own config check: every guarantee hv_runner_config_ok makes EXCEPT installer=0/ecid=0, which this
   binary deliberately sets the other way (reviewed; see the top-of-file comment for why hv_runner_config_ok itself
   is not reused or changed). */
static int g4ui_config_ok(const HvVmConfig *c, uint64_t ecid, char *why, size_t cap) {
#define BAD(msg) do { if (why && cap) snprintf(why, cap, "%s", msg); return -1; } while (0)
    if (!c) BAD("no configuration");
    if (!c->readonly_storage) BAD("storage is not read-only");
    if (!c->no_net) BAD("the guest network is not off");
    if (!c->installer) BAD("this binary needs installer boot on (it is the DFU/G4 build)");
    if (ecid == 0) BAD("--ecid is required and must be nonzero for this binary");
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
    printf("ui_runner_vm_g4 %s: pid %d cmds popped %llu dispatched %llu skipped %llu rejected %llu | calls %llu queries %llu | display present %llu absent %llu | "
           "pointer valid %llu absent %llu | call_errors %llu shm_errors %llu | frames published %llu | ring pushed %llu popped %llu dropped %llu | disk writes asked %llu refused %llu | "
           "net frames tx %llu rx %llu | recovered %u%s\n",
           tag, (int)getpid(), (unsigned long long)r.cmds_popped, (unsigned long long)r.cmds_dispatched, (unsigned long long)r.cmds_skipped, (unsigned long long)r.cmds_rejected,
           (unsigned long long)r.calls_made, (unsigned long long)r.queries_made, (unsigned long long)r.display_present, (unsigned long long)r.display_absent,
           (unsigned long long)r.pointer_valid, (unsigned long long)r.pointer_absent, (unsigned long long)r.call_errors, (unsigned long long)r.shm_errors,
           (unsigned long long)r.frames_published, ok ? (unsigned long long)st.cmds_pushed : 0ull, ok ? (unsigned long long)st.cmds_popped : 0ull,
           ok ? (unsigned long long)st.cmds_dropped : 0ull, (unsigned long long)wc, (unsigned long long)wr, (unsigned long long)hv_net_tx_frames, (unsigned long long)hv_net_rx_frames,
           ok ? st.recovered : 0u, ok ? "" : " (stats lock not taken)");
    fflush(stdout);
}

/* Runs on its own thread so hv_g4_wait()'s own blocking contract (reviewed: must be called, and called exactly
   once, for hv_g4_finish()'s g_waited==1 pass condition to ever be true) never stalls the main loop's frame pump.
   Calls hv_g4_finish() itself, on this same thread, strictly after hv_g4_wait() returns -- never from main while
   this thread might still be running. */
static int g_g4_finish_failed = -1; /* -1 not yet run, 0 passed, >0 that many FAILED rows */
static atomic_int g_g4_done;
static const char *g_disk_path, *g_aux_path;
static void *g4_control_thread(void *arg) {
    (void)arg;
    hv_g4_wait(380000); /* G4's own proven budget (host/vmrun.c): recovery_enter_restore()'s ~30s internal wait plus idevicerestore's own up-to-300s
                            wait for MODE_RESTORE, on top of the TSS+DFU->Recovery stage. Its return value only distinguishes ended-by-itself from
                            needed-the-cancel from never-ended; hv_g4_finish()'s table below is the actual verdict this binary reports. */
    g_g4_finish_failed = hv_g4_finish(stdout, g_disk_path, g_aux_path);
    atomic_store(&g_g4_done, 1);
    return NULL;
}

int main(int argc, char **argv) {
    const char *shm_path = NULL, *rom = NULL, *disk = NULL, *aux = NULL, *ipsw = NULL, *workdir = ".";
    long ram_mb = 2048, cpus = 2, jit_mb = 64, seconds = 450, stop_wait_ms = 5000;
    uint64_t ecid = 0;
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--shm") && more) shm_path = argv[++i];
        else if (!strcmp(argv[i], "--rom") && more) rom = argv[++i];
        else if (!strcmp(argv[i], "--disk") && more) disk = argv[++i];
        else if (!strcmp(argv[i], "--aux") && more) aux = argv[++i];
        else if (!strcmp(argv[i], "--ipsw") && more) ipsw = argv[++i];
        else if (!strcmp(argv[i], "--workdir") && more) workdir = argv[++i];
        else if (!strcmp(argv[i], "--ecid") && more) ecid = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ram-mb") && more) ram_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cpus") && more) cpus = atol(argv[++i]);
        else if (!strcmp(argv[i], "--jit-mb") && more) jit_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stop-wait-ms") && more) stop_wait_ms = atol(argv[++i]);
        else {
            fprintf(stderr, "ui_runner_vm_g4: unknown or incomplete argument '%s' (accepted: --shm --rom --disk --aux --ipsw --ecid --workdir --ram-mb --cpus --jit-mb --seconds --stop-wait-ms)\n", argv[i]);
            return 2;
        }
    }
    if (!shm_path || !rom || !disk || !aux || !ipsw || !ecid || seconds < 0 || stop_wait_ms < 0 || stop_wait_ms > 600000) {
        fprintf(stderr, "ui_runner_vm_g4: --shm --rom --disk --aux --ipsw are required, --ecid must be a nonzero value, and the numbers must be sensible\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    char names[512], left[512];
    int scrubbed = hv_runner_scrub_env(names, sizeof names, left, sizeof left);
    if (scrubbed < 0) {
        fprintf(stderr, "ui_runner_vm_g4: the environment is not clean, HV_* variable(s) left: %s\n", left[0] ? left : "(unknown)");
        printf("ui_runner_vm_g4 RESULT: FAILED, HV_* variable(s) could not be removed: %s\n", left[0] ? left : "(unknown)");
        return 1;
    }

    HvVmConfig cfg;
    hv_runner_make_config(&cfg, rom, disk, aux, (uint64_t)(ram_mb < 0 ? 0 : ram_mb), (int)cpus, (size_t)(jit_mb < 0 ? 0 : jit_mb));
    cfg.installer = 1;   /* this binary's whole point; host/ui_runner_vm.c is untouched and still forces 0 */
    cfg.ecid = ecid;      /* the machine identity G4's TSS personalization request is signed for */
    char why[160], line[400];
    if (g4ui_config_ok(&cfg, ecid, why, sizeof why) != 0) {
        fprintf(stderr, "ui_runner_vm_g4: the settings were refused: %s\n", why);
        printf("ui_runner_vm_g4 RESULT: FAILED, settings refused (%s)\n", why);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (hv_shm_create(shm_path, &g_shm) != 0) { fprintf(stderr, "ui_runner_vm_g4: cannot make the shared file: %s\n", strerror(errno)); return 2; }
    g_run = hv_runner_create(&REAL_API, g_shm);
    if (!g_run) { fprintf(stderr, "ui_runner_vm_g4: out of memory\n"); hv_shm_set_state(g_shm, HV_SHM_STOPPED); return 2; }

    printf("ui_runner_vm_g4: started pid %d, G4 install-diagnostic virtual machine mode\n", (int)getpid());
    hv_runner_config_line(&cfg, line, sizeof line);
    printf("ui_runner_vm_g4 config (the structure passed to hv_vm_create): %s\n", line);
    printf("ui_runner_vm_g4 env: %d HV_* variable(s) removed%s%s\n", scrubbed, scrubbed ? ": " : "", scrubbed ? names : "");
    fflush(stdout);

    /* Before the machine is created, same order host/vmrun.c's own proven G4 path uses: both storage files must
       already exist (this binary creates neither), their identity is hashed now for the later unchanged check. */
    if (hv_g4_begin(disk, aux, ecid) != 0) {
        printf("ui_runner_vm_g4 RESULT: FAILED, install g4 begin failed before the machine was created\n");
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    g_disk_path = disk; g_aux_path = aux;

    char err[256] = "";
    double t0 = now();
    if (hv_vm_create(&cfg, err, sizeof err) != 0) {
        printf("ui_runner_vm_g4 RESULT: FAILED, the machine could not be made: %s\n", err);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    HvVmapple *bd = hv_vm_board();
    int usb_present = bd && bd->vusb ? 1 : 0;
    printf("ui_runner_vm_g4: machine created in %.0f ms; usb device port present: %d (want 1, installer boot)\n", (now() - t0) * 1e3, usb_present);
    if (!bd || !usb_present) {
        printf("ui_runner_vm_g4 RESULT: FAILED, usb device port present=%d, expected 1 for an installer boot; not started\n", usb_present);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }

    hv_vm_start(1, on_exit_cb);
    hv_runner_set_ready(g_run, 1);
    hv_shm_set_state(g_shm, HV_SHM_RUNNING);

    /* hv_g4_start() itself waits up to 60s for the guest to come online on the bus, then makes the REAL TSS
       request and starts the restore thread (host/install.c's hv_install_start, already async: it pthread_creates
       its own worker and returns). That up-to-60s wait runs on THIS (main) thread, same as host/vmrun.c -- it is
       a one-time startup cost, not part of the steady-state frame-pump loop below. */
    printf("ui_runner_vm_g4: starting G4 (TSS personalization + DFU->Recovery + RecoveryOS entry, stop C) -- this makes a REAL request to Apple's signing server\n");
    fflush(stdout);
    if (bd->vusb ? hv_g4_start(bd->vusb, ipsw, workdir, ecid) != 0 : 1) {
        printf("ui_runner_vm_g4 RESULT: FAILED, install g4 did not start\n");
        hv_vm_stop();
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }

    pthread_t ctrl_th;
    if (pthread_create(&ctrl_th, NULL, g4_control_thread, NULL) != 0) {
        fprintf(stderr, "ui_runner_vm_g4: could not start the G4 control thread\n");
        printf("ui_runner_vm_g4 RESULT: FAILED, could not start the G4 control thread\n");
        hv_vm_stop();
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }

    const char *why_stop = "quit";
    double tstart = now(), next_frame = 0;
    char inl[256]; size_t ll = 0; long ignored = 0;
    int cancel_sent = 0;
    for (;;) {
        int stop_now = 0;
        if (atomic_load(&g_sig)) { why_stop = "signal"; stop_now = 1; }
        else if (hv_shm_app_closed(g_shm)) { why_stop = "app closed"; stop_now = 1; }
        else if (atomic_load(&g_exited)) { why_stop = "the machine stopped itself"; stop_now = 1; }
        else if (seconds && now() - tstart >= (double)seconds) { why_stop = "time limit"; stop_now = 1; }
        else if (atomic_load(&g_g4_done)) { why_stop = "g4 ended"; stop_now = 1; }
        if (stop_now) {
            /* This loop is the only caller of hv_install_cancel(): sent at most once (cancel_sent), so it never
               races hv_g4_wait()'s own internal cancel-after-380s on the control thread -- by the time that could
               fire, this binary's own 450s default has already requested a stop here first. A g4-ended stop skips
               the cancel (nothing left to cancel). */
            if (!cancel_sent && !atomic_load(&g_g4_done)) { hv_install_cancel(); cancel_sent = 1; }
            break;
        }
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
                if (!strcmp(inl, "quit")) {
                    if (!cancel_sent && !atomic_load(&g_g4_done)) { hv_install_cancel(); cancel_sent = 1; }
                    why_stop = "quit"; break;
                } else if (!strcmp(inl, "status")) status_line("status");
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

    /* Join the control thread BEFORE anything else: hv_g4_finish() (called on that thread, right after
       hv_g4_wait() returns there) must see the machine still running and the restore thread's own state settled
       -- reading disk/aux hashes or usb/network counters while either might still be changing was the exact
       ordering mistake an earlier draft of this design made and a review caught. This can block for a while
       (hv_g4_wait()'s own up to 20s past its cap, or up to the remainder of its 380s budget if the cancel above
       does not land quickly) -- that wait is hv_g4_wait()'s own documented contract, not something this binary
       can shorten further without changing that function. */
    pthread_join(ctrl_th, NULL);

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
    printf("ui_runner_vm_g4 RESULT: %s (%s), g4 %s (FAILED rows=%d), ignored input lines %ld\n",
           confirmed ? "stopped, confirmed" : "STOP NOT CONFIRMED within the wait; ending the process anyway", why_stop,
           g_g4_finish_failed == 0 ? "passed" : g_g4_finish_failed > 0 ? "FAILED" : "did not run to a verdict", g_g4_finish_failed, ignored);
    fflush(stdout);
    if (confirmed) { hv_runner_destroy(g_run); hv_shm_close(g_shm); }
    return (!confirmed) ? 3 : (g_g4_finish_failed == 0) ? 0 : 1;
}
