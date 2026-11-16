/* C1-2: the runner process with a VIRTUAL MACHINE in it. It is host/ui_runner.c's counterpart: same shared file (host/frame_shm.h), same stdin words, same stop idea, but the
   commands go to the real hv_vm_* input functions and the picture and the pointer come from hv_vm_boot_display and hv_vm_pointer (through host/runner_vm_core.c).

   WHAT IT WILL NOT DO, and where that is enforced:
     - write to the disk or the auxiliary storage: the readonly_storage field set to 1 in hv_runner_make_config and re-checked by hv_runner_config_ok before hv_vm_create
     - use a network: no_net = 1 (same place); this program links no restore, TSS, curl, USB-host, libirecovery or install code and imports no socket function
     - boot as an installer: installer = 0 (same place), no machine identity (ECID and seed 0), and every HV_* environment variable is removed first (the engine reads some of
       them to switch an installer boot, a RAM file or the USB probe on); after hv_vm_create it also checks that the machine has no USB device port (installer boots only)
     - take any of that from the command line: the only options are --shm, --rom, --disk, --aux, --ram-mb, --cpus, --jit-mb, --seconds, --stop-wait-ms
   tools/check-runner-vm-safe.sh checks the built binary and these source lines; tools/c1vm-test.sh runs it (a stand-in ROM and blank disks, Linux arm64 only: the engine runs arm64
   code natively).
   WHAT A PASSING RUN DOES NOT SHOW: with the stand-in ROM (`b .`) no guest software runs, so there is no boot picture and no pointer. That the shared file carries a real picture
   and a real pointer is shown with a recorder in place of the machine (tests/test_c1vm.c), not with this program. A real macOS boot is NOT shown by anything here.

   Arguments: --shm PATH, --rom F, --disk F, --aux F (all four required)  --ram-mb N (default 256)  --cpus N (default 2)  --jit-mb N (default 64)  --seconds N (stop after N s)
   --stop-wait-ms N (how long the stop waits for the machine's confirmation, default 5000).
   Standard input: "quit" or end of input = stop; "status" = one status line. Exit codes: 0 stopped and confirmed; 1 the settings were refused or the machine could not be made;
   2 bad arguments or the shared file could not be made; 3 the stop was NOT confirmed within the wait.
   Output: "ui_runner_vm: ..." lines and one "ui_runner_vm RESULT: ..." at the end. */
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
#include "machine.h" /* HV_PSCI_HALT_STACK_WORDS, for sizing the local buffer hv_vm_psci_halt_stack() fills */

extern uint64_t hv_net_tx_frames, hv_net_rx_frames;

/* The one table the program ever uses. */
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

/* [C1-4] Android panic-entry probe: a read-only bring-up aid for the Android-port boot investigation, narrowing
   WHICH of ~163 static BL references to two known generic-panic-helper functions (empty-message and "%llx:%d"
   assertion helpers in the AVPBooter ROM under investigation, found by ROM disassembly -- see
   docs/boot-diagnostic-contract-2026-11-15.md) is the one actually reached before the SYSTEM_RESET this
   investigation already traced to 0x106270/0x105FC0/0x103db0/0x104530/0x1044d4. This only runs on the cpu that
   reached one of the two addresses (core/dispatch.c calls stop_hook there, then continues normal execution --
   see that file's own comment), so it only ever writes that same cpu's own panic_first/panic_last/panic_hits
   fields (core/cpu.h) -- no lock, no cross-CPU access, and no guest-visible state (registers, PC, memory, return
   values) is touched. Unlike host/vmrun.c's HV_DIAG_PROBE/_CALLER this cannot be gated by an environment
   variable: hv_runner_scrub_env() below deliberately removes every HV_* variable before the machine is ever
   created, precisely so a debug-only switch can never quietly affect a real boot through this binary, which
   is why this installs unconditionally in code instead. Installing it even against some other ROM is harmless
   for the same reason HV_DIAG_PROBE already is: these two addresses, if that other ROM never executes them (the
   overwhelmingly likely case), simply never fire the hook, and if they coincidentally do, the hook still only
   reads registers. */
static void panic_probe_hook(HvCpu *cpu) {
    HvPanicHit hit = { .pc = cpu->pc, .lr = cpu->x[30], .x0 = cpu->x[0], .x1 = cpu->x[1],
                        .x2 = cpu->x[2], .x3 = cpu->x[3], .sp = cpu->sp };
    if (cpu->panic_hits == 0) cpu->panic_first = hit;
    cpu->panic_last = hit;
    if (cpu->panic_hits != UINT32_MAX) cpu->panic_hits++; /* saturate, never wrap */
}

/* Installs the probe right after the machine is created and before hv_vm_start(). Refuses (prints why, changes
   nothing) rather than overwrite a stop_hook some other debug feature already set -- this binary has none of
   those, but the shared engine does, so this stays defensive the same way host/vmrun.c's diag_probe_install()
   is. Either way normal guest boot proceeds -- a refused install never blocks the boot itself, only this one
   diagnostic. */
#define HV_PANIC_PROBE_PC1 0x105f2cull
#define HV_PANIC_PROBE_PC2 0x105f9cull
static void panic_probe_install(void) {
    HvMachine *vm = hv_vm_machine();
    if (vm->stop_hook) {
        printf("ui_runner_vm: panic-entry probe not installed; a stop hook is already in place\n");
        return;
    }
    vm->stop_pc = HV_PANIC_PROBE_PC1;
    vm->stop_pc2 = HV_PANIC_PROBE_PC2;
    vm->stop_hook = panic_probe_hook;
    printf("ui_runner_vm: panic-entry probe installed at %#llx / %#llx\n",
           (unsigned long long)HV_PANIC_PROBE_PC1, (unsigned long long)HV_PANIC_PROBE_PC2);
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
    printf("ui_runner_vm %s: pid %d cmds popped %llu dispatched %llu skipped %llu rejected %llu | calls %llu queries %llu | display present %llu absent %llu | "
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

int main(int argc, char **argv) {
    const char *shm_path = NULL, *rom = NULL, *disk = NULL, *aux = NULL;
    long ram_mb = 256, cpus = 2, jit_mb = 64, seconds = 0, stop_wait_ms = 5000;
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--shm") && more) shm_path = argv[++i];
        else if (!strcmp(argv[i], "--rom") && more) rom = argv[++i];
        else if (!strcmp(argv[i], "--disk") && more) disk = argv[++i];
        else if (!strcmp(argv[i], "--aux") && more) aux = argv[++i];
        else if (!strcmp(argv[i], "--ram-mb") && more) ram_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cpus") && more) cpus = atol(argv[++i]);
        else if (!strcmp(argv[i], "--jit-mb") && more) jit_mb = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stop-wait-ms") && more) stop_wait_ms = atol(argv[++i]);
        else {
            fprintf(stderr, "ui_runner_vm: unknown or incomplete argument '%s' (accepted: --shm --rom --disk --aux --ram-mb --cpus --jit-mb --seconds --stop-wait-ms)\n", argv[i]);
            return 2;
        }
    }
    if (!shm_path || !rom || !disk || !aux || seconds < 0 || stop_wait_ms < 0 || stop_wait_ms > 600000) {
        fprintf(stderr, "ui_runner_vm: --shm, --rom, --disk and --aux are required; the numbers must be sensible\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    char names[512], left[512];
    int scrubbed = hv_runner_scrub_env(names, sizeof names, left, sizeof left);
    if (scrubbed < 0) {   /* an HV_* variable could not be removed or is still there: the engine might read it (installer boot, RAM file, USB probe): no machine */
        fprintf(stderr, "ui_runner_vm: the environment is not clean, HV_* variable(s) left: %s\n", left[0] ? left : "(unknown)");
        printf("ui_runner_vm RESULT: FAILED, HV_* variable(s) could not be removed: %s\n", left[0] ? left : "(unknown)");
        return 1;
    }

    HvVmConfig cfg;
    hv_runner_make_config(&cfg, rom, disk, aux, (uint64_t)(ram_mb < 0 ? 0 : ram_mb), (int)cpus, (size_t)(jit_mb < 0 ? 0 : jit_mb));
    char why[160], line[400];
    if (hv_runner_config_ok(&cfg, why, sizeof why) != 0) {
        fprintf(stderr, "ui_runner_vm: the settings were refused: %s\n", why);
        printf("ui_runner_vm RESULT: FAILED, settings refused (%s)\n", why);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (hv_shm_create(shm_path, &g_shm) != 0) { fprintf(stderr, "ui_runner_vm: cannot make the shared file: %s\n", strerror(errno)); return 2; }
    g_run = hv_runner_create(&REAL_API, g_shm);
    if (!g_run) { fprintf(stderr, "ui_runner_vm: out of memory\n"); hv_shm_set_state(g_shm, HV_SHM_STOPPED); return 2; }

    printf("ui_runner_vm: started pid %d, virtual machine mode\n", (int)getpid());
    hv_runner_config_line(&cfg, line, sizeof line);
    printf("ui_runner_vm config (the structure passed to hv_vm_create): %s\n", line);
    printf("ui_runner_vm env: %d HV_* variable(s) removed%s%s\n", scrubbed, scrubbed ? ": " : "", scrubbed ? names : "");
    fflush(stdout);

    char err[256] = "";
    double t0 = now();
    if (hv_vm_create(&cfg, err, sizeof err) != 0) {
        printf("ui_runner_vm RESULT: FAILED, the machine could not be made: %s\n", err);
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    HvVmapple *bd = hv_vm_board();
    printf("ui_runner_vm: machine created in %.0f ms; usb device port present: %d (must be 0)\n", (now() - t0) * 1e3, bd && bd->vusb ? 1 : 0);
    if (!bd || bd->vusb) {   /* only an installer boot has the port: the machine is not what the settings said */
        printf("ui_runner_vm RESULT: FAILED, the machine has a USB device port; it is not started\n");
        hv_shm_set_state(g_shm, HV_SHM_STOPPED);
        return 1;
    }
    panic_probe_install();

    hv_vm_start(1, on_exit_cb);
    hv_runner_set_ready(g_run, 1);
    hv_shm_set_state(g_shm, HV_SHM_RUNNING);

    const char *why_stop = "quit";
    char reason_txt[700] = "";
    double tstart = now(), next_frame = 0;
    char inl[256]; size_t ll = 0; long ignored = 0;
    for (;;) {
        if (atomic_load(&g_sig)) { why_stop = "signal"; break; }
        if (hv_shm_app_closed(g_shm)) { why_stop = "app closed"; break; }
        if (atomic_load(&g_exited)) {
            int er = atomic_load(&g_exit_reason);
            uint32_t pfn = 0, pcpu = 0; uint64_t ppc = 0, plr = 0;
            /* hv_vm_psci_halt_info() only ever returns non-zero for a PSCI-triggered halt (SYSTEM_OFF/
               SYSTEM_RESET -- see core/exception.c's psci()), so its presence alone is the check; no need to
               separately compare er against HV_EXIT_HALT (whose enum this file does not otherwise need). */
            if (hv_vm_psci_halt_info(&pfn, &pcpu, &ppc, &plr)) {
                int rn = snprintf(reason_txt, sizeof reason_txt,
                         "the machine stopped itself, reason %d (%s), psci %s (fn=%#x) by cpu%u at pc=%#llx lr=%#llx",
                         er, hv_vm_reason_name(er), pfn == 0x84000008 ? "SYSTEM_OFF" : "SYSTEM_RESET", pfn, pcpu,
                         (unsigned long long)ppc, (unsigned long long)plr);
                /* Same snapshot, a second accessor (host/vm.h's hv_vm_psci_halt_stack): raw sp/x26/stack
                   bytes for working out which of several ROM call sites reached this PSCI call, appended
                   here rather than interpreted -- see core/machine.h's psci_halt_stack[] comment. */
                uint64_t psp = 0, px26 = 0, pstack[HV_PSCI_HALT_STACK_WORDS]; int pn = 0;
                if (rn > 0 && (size_t)rn < sizeof reason_txt && hv_vm_psci_halt_stack(&psp, &px26, pstack, &pn)) {
                    char words[HV_PSCI_HALT_STACK_WORDS * 17 + 1] = ""; size_t wo = 0;
                    for (int i = 0; i < pn && wo + 18 < sizeof words; i++)
                        wo += (size_t)snprintf(words + wo, sizeof words - wo, "%s%016llx", i ? "," : "", (unsigned long long)pstack[i]);
                    snprintf(reason_txt + rn, sizeof reason_txt - (size_t)rn, " sp=%#llx x26=%#llx stack(sp+0..sp+%#x)=%s",
                             (unsigned long long)psp, (unsigned long long)px26, pn * 8, words);
                }
            } else
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

    /* The stop, in order: no more input to the machine, no more pictures from it (display_buf's contract), then the machine, then its confirmation. */
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
    printf("ui_runner_vm RESULT: %s (%s), ignored input lines %ld\n", confirmed ? "stopped, confirmed" : "STOP NOT CONFIRMED within the wait; ending the process anyway", why_stop, ignored);
    /* [C1-4] Android panic-entry probe result, as its own line rather than appended to reason_txt above: that
       buffer is already sized for the PSCI stack dump, and a second, independently-truncatable payload is
       clearer kept separate. Only printed when hv_vm_panic_probe() has anything to say at all (the same PSCI-
       halt gate as hv_vm_psci_halt_info/_stack above -- see their own comments for why a non-PSCI stop never
       gets here); within that, hits == 0 is printed too, plainly, rather than omitting the line, since "the
       probe never fired" is itself the answer to the question this probe exists to ask. */
    uint32_t phits = 0; HvPanicHit pfirst = {0}, plast = {0};
    if (hv_vm_panic_probe(&phits, &pfirst, &plast)) {
        printf("ui_runner_vm RESULT: PANIC_DIAG hits=%u", (unsigned)phits);
        if (phits) {
            printf(" first.pc=%#llx first.lr=%#llx first.x0=%#llx first.x1=%#llx first.x2=%#llx first.x3=%#llx first.sp=%#llx"
                   " last.pc=%#llx last.lr=%#llx last.x0=%#llx last.x1=%#llx last.x2=%#llx last.x3=%#llx last.sp=%#llx\n",
                   (unsigned long long)pfirst.pc, (unsigned long long)pfirst.lr, (unsigned long long)pfirst.x0,
                   (unsigned long long)pfirst.x1, (unsigned long long)pfirst.x2, (unsigned long long)pfirst.x3, (unsigned long long)pfirst.sp,
                   (unsigned long long)plast.pc, (unsigned long long)plast.lr, (unsigned long long)plast.x0,
                   (unsigned long long)plast.x1, (unsigned long long)plast.x2, (unsigned long long)plast.x3, (unsigned long long)plast.sp);
        } else printf(" (neither probe address was reached by the cpu that made the PSCI call)\n");
    }
    fflush(stdout);
    if (confirmed) { hv_runner_destroy(g_run); hv_shm_close(g_shm); }   /* with the machine still running both are left for the process end */
    return confirmed ? 0 : 3;
}
