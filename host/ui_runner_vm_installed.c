/* C1-2 INSTALLED: a third sibling of host/ui_runner_vm.c and host/ui_runner_vm_g4.c, same file layout (host/frame_shm.h, the same stdin words, the same
   stop idea), same real hv_vm_* input functions and the same frame-publishing loop (host/runner_vm_core.c) -- but for the one combination neither of those
   two binaries covers: a NORMAL boot (installer stays 0, exactly like host/ui_runner_vm.c -- this is not a DFU/restore run) of a disk that was already
   personalized for a specific machine identity (a nonzero ECID/seed, host/vm.h's "the machine identity the disk was installed with"), which
   host/ui_runner_vm.c's own hv_runner_make_config() hardcodes to 0/0 as one of its four safety guarantees (host/runner_vm_core.c) and therefore can never
   pass through.

   WHY A THIRD BINARY, NOT A FLAG ON host/ui_runner_vm.c: host/runner_vm_core.c's hv_runner_config_ok() is documented (runner_vm_core.h) as being the
   ecid=0/seed=0, installer=0 guarantee for host/ui_runner_vm.c and every other caller of it; reusing or loosening it here would make it lie for this one
   caller. tools/check-runner-vm-safe.sh additionally enforces, at the source and binary level, that host/ui_runner_vm.c (a) assigns no safety field itself,
   (b) accepts exactly its documented nine options, and (c) contains none of the strings --ecid/--seed/--installer/... anywhere in the built binary -- so
   adding those flags there is not just undesirable, it is a build-time-checked impossibility by design. This file is the separate execution mode called for
   instead, mirroring host/ui_runner_vm_g4.c's own precedent of a second binary with its own local config-check function rather than a modified shared one.

   WHAT IS DIFFERENT FROM host/ui_runner_vm.c (which this binary otherwise matches line for line in spirit):
     - a caller-supplied, REQUIRED, nonzero --ecid (and an optional --seed, default 0 -- host/vm.h's ecid/seed pair; G4's own g4ui_config_ok likewise only
       requires ecid nonzero, not seed, and this mirrors that asymmetry) -- host/ui_runner_vm.c forces both to 0 and that stays true THERE; this is a
       separate, new binary, not a change to it.
     - installer stays 0 (unlike ui_runner_vm_g4.c, which deliberately sets it to 1): this is still a normal boot, not a DFU/restore run. The disk is assumed
       to already be an installed, personalized macOS disk (however it was produced -- that question, G7 real restore vs. an externally-sourced image, is
       explicitly out of scope for this file and is tracked separately, see docs/g7-vs-external-image-2026-11-22.md).
     - its own config check (installed_boot_config_ok below), NOT host/runner_vm_core.c's hv_runner_config_ok -- every OTHER guarantee that function makes
       (read-only storage, no guest network, installer boot off, no host-provided JIT memory, sane RAM/CPU/JIT ranges, required paths) is re-checked here,
       unchanged; only the ecid/seed=0 requirement is lifted, and only in this one binary.
     - disk and aux stay read-only (installed_boot_config_ok requires it, same as hv_runner_config_ok and g4ui_config_ok): this binary cannot write the
       personalization it boots any more than host/ui_runner_vm.c can write anything. Nothing here creates, restores, or writes to a disk image --
       host/disk_policy.c's macos_disk.img creation path and restored_start_restore() (the real ASR/FDR restore) are both untouched and unlinked by this
       file, same as by host/ui_runner_vm.c.
     - no usb_on_normal_boot/usb_probe_on_normal_boot/panic-probe wiring: those exist in host/ui_runner_vm.c for an unrelated, already-concluded boot-branch
       investigation on a BLANK (no-identity) normal boot; carrying them over here would mix two separate investigations and is left for a later, explicit
       round if a real installed-disk boot ever needs them.
     - take any of that from the command line: the accepted options are --shm, --rom, --disk, --aux, --ecid, --seed, --ram-mb, --cpus, --jit-mb, --seconds,
       --stop-wait-ms (--ecid required and nonzero; --seed optional, default 0; every other option matches host/ui_runner_vm.c's own).
   tools/check-runner-vm-installed-safe.sh checks the built binary and these source lines, mirroring tools/check-runner-vm-safe.sh's approach for the other
   two binaries; tools/c1vm-test.sh does not yet run this one (tracked as a follow-up in the sequencing doc below, same stand-in-ROM/blank-disk shape as the
   existing Part A coverage would need).
   WHAT A PASSING RUN DOES NOT SHOW: with a stand-in ROM (`b .`) or a blank disk, no guest software runs and nothing about whether an actually-installed,
   personalized macOS disk boots any differently than a blank one is shown by this file at all -- that is exactly the open question item (2) of the
   sequencing doc below exists to answer (G7 real restore vs. an externally-sourced pre-installed image), not something this binary resolves by existing.

   Arguments: --shm PATH, --rom F, --disk F, --aux F, --ecid HEX (all five required, ecid must be nonzero)  --seed HEX (default 0)  --ram-mb N (default 256)
   --cpus N (default 2)  --jit-mb N (default 64)  --seconds N (stop after N s)  --stop-wait-ms N (default 5000).
   Standard input: "quit" or end of input = stop; "status" = one status line. Exit codes: 0 stopped and confirmed; 1 the settings were refused or the machine
   could not be made; 2 bad arguments or the shared file could not be made; 3 the stop was NOT confirmed within the wait.
   Output: "ui_runner_vm_installed: ..." lines and one "ui_runner_vm_installed RESULT: ..." at the end. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* This binary's own config check: every guarantee hv_runner_config_ok makes EXCEPT ecid/seed=0, which this binary
   deliberately allows nonzero (reviewed; see the top-of-file comment for why hv_runner_config_ok itself is not
   reused or changed). Unlike g4ui_config_ok (host/ui_runner_vm_g4.c), installer must stay 0 here: this is a
   normal boot, not a DFU/restore run. */
static int installed_boot_config_ok(const HvVmConfig *c, uint64_t ecid, char *why, size_t cap) {
#define BAD(msg) do { if (why && cap) snprintf(why, cap, "%s", msg); return -1; } while (0)
    if (!c) BAD("no configuration");
    if (!c->readonly_storage) BAD("storage is not read-only");
    if (!c->no_net) BAD("the guest network is not off");
    if (c->installer) BAD("this binary needs installer boot OFF (it boots an already-installed disk as a normal machine, not DFU)");
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
    printf("ui_runner_vm_installed %s: pid %d cmds popped %llu dispatched %llu skipped %llu rejected %llu | calls %llu queries %llu | display present %llu absent %llu | "
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
    const char *shm_path = NULL, *rom = NULL, *disk = NULL, *aux = NULL;
    long ram_mb = 256, cpus = 2, jit_mb = 64, seconds = 0, stop_wait_ms = 5000;
    uint64_t ecid = 0, seed = 0;
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--shm") && more) shm_path = argv[++i];
        else if (!strcmp(argv[i], "--rom") && more) rom = argv[++i];
        else if (!strcmp(argv[i], "--disk") && more) disk = argv[++i];
        else if (!strcmp(argv[i], "--aux") && more) aux = argv[++i];
        else if (!strcmp(argv[i], "--ecid") && more) ecid = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed") && more) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ram-mb") && more) ram_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cpus") && more) cpus = atol(argv[++i]);
        else if (!strcmp(argv[i], "--jit-mb") && more) jit_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stop-wait-ms") && more) stop_wait_ms = atol(argv[++i]);
        else {
            fprintf(stderr, "ui_runner_vm_installed: unknown or incomplete argument '%s' (accepted: --shm --rom --disk --aux --ecid --seed --ram-mb --cpus --jit-mb --seconds --stop-wait-ms)\n", argv[i]);
            return 2;
        }
    }
    if (!shm_path || !rom || !disk || !aux || !ecid || seconds < 0 || stop_wait_ms < 0 || stop_wait_ms > 600000) {
        fprintf(stderr, "ui_runner_vm_installed: --shm --rom --disk --aux are required, --ecid must be a nonzero value, and the numbers must be sensible\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    char names[512], left[512];
    int scrubbed = hv_runner_scrub_env(names, sizeof names, left, sizeof left);
    if (scrubbed < 0) {
        fprintf(stderr, "ui_runner_vm_installed: the environment is not clean, HV_* variable(s) left: %s\n", left[0] ? left : "(unknown)");
        printf("ui_runner_vm_installed RESULT: FAILED, HV_* variable(s) could not be removed: %s\n", left[0] ? left : "(unknown)");
        return 1;
    }

    HvVmConfig cfg;
    hv_runner_make_config(&cfg, rom, disk, aux, (uint64_t)(ram_mb < 0 ? 0 : ram_mb), (int)cpus, (size_t)(jit_mb < 0 ? 0 : jit_mb));
    /* [this binary's whole point, reviewed and explicitly approved] hv_runner_make_config leaves these at 0/0
       (host/runner_vm_core.c is unchanged); this binary alone sets them, after every other guarantee that
       function makes (readonly storage, no network, installer=0 -- all untouched above) is already built. */
    cfg.ecid = ecid;
    cfg.seed = seed;
    char why[160], line[400];
    if (installed_boot_config_ok(&cfg, ecid, why, sizeof why) != 0) {
        fprintf(stderr, "ui_runner_vm_installed: the settings were refused: %s\n", why);
        printf("ui_runner_vm_installed RESULT: FAILED, settings refused (%s)\n", why);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (hv_shm_create(shm_path, &g_shm) != 0) { fprintf(stderr, "ui_runner_vm_installed: cannot make the shared file: %s\n", strerror(errno)); return 2; }
    g_run = hv_runner_create(&REAL_API, g_shm);
    if (!g_run) { fprintf(stderr, "ui_runner_vm_installed: out of memory\n"); hv_shm_set_state(g_shm, HV_SHM_STOPPED); return 2; }

    printf("ui_runner_vm_installed: started pid %d, installed-disk virtual machine mode (normal boot, nonzero machine identity)\n", (int)getpid());
    hv_runner_config_line(&cfg, line, sizeof line);
    printf("ui_runner_vm_installed config (the structure passed to hv_vm_create): %s\n", line);
    printf("ui_runner_vm_installed env: %d HV_* variable(s) removed%s%s\n", scrubbed, scrubbed ? ": " : "", scrubbed ? names : "");
    fflush(stdout);

    char err[256] = "";
    double t0 = now();
    if (hv_vm_create(&cfg, err, sizeof err) != 0) {
        printf("ui_runner_vm_installed RESULT: FAILED, the machine could not be made: %s\n", err);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    HvVmapple *bd = hv_vm_board();
    int usb_present = bd && bd->vusb ? 1 : 0;
    printf("ui_runner_vm_installed: machine created in %.0f ms; usb device port present: %d (want 0, normal boot without the usb_on_normal_boot diagnostic)\n", (now() - t0) * 1e3, usb_present);
    if (!bd || usb_present) {
        printf("ui_runner_vm_installed RESULT: FAILED, usb device port present=%d, expected 0 for this binary; not started\n", usb_present);
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
    if (confirmed && hv_vm_quiesced_ok() != 0) hv_vm_flush_disks();   /* 0 = a thread or disk worker did not end: no flush, no teardown */
    hv_shm_set_state(g_shm, HV_SHM_STOPPED);
    status_line("final");
    printf("ui_runner_vm_installed RESULT: %s (%s), ignored input lines %ld\n", confirmed ? "stopped, confirmed" : "STOP NOT CONFIRMED within the wait; ending the process anyway", why_stop, ignored);
    fflush(stdout);
    if (confirmed && hv_vm_quiesced_ok() != 0) { hv_runner_destroy(g_run); hv_shm_close(g_shm); }
    return confirmed ? 0 : 3;
}
