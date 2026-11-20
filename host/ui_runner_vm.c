/* C1-2: the runner process with a VIRTUAL MACHINE in it. It is host/ui_runner.c's counterpart: same shared file (host/frame_shm.h), same stdin words, same stop idea, but the
   commands go to the real hv_vm_* input functions and the picture and the pointer come from hv_vm_boot_display and hv_vm_pointer (through host/runner_vm_core.c).

   WHAT IT WILL NOT DO, and where that is enforced:
     - write to the disk or the auxiliary storage: the readonly_storage field set to 1 in hv_runner_make_config and re-checked by hv_runner_config_ok before hv_vm_create
     - use a network: no_net = 1 (same place); this program links no restore, TSS, curl, libirecovery or install code and imports no socket function. [2026-11-21 correction:
       host/usb_host.c itself IS linked (as vm_usb_host.o, from tools/devices-build.sh, same as every vmrun_g0..g6 binary) -- an earlier version of this comment said
       "USB-host" was absent too, which was never true; host/libusb_shim.c (the actual libirecovery/usbmuxd/idevicerestore passthrough) is what is genuinely absent. Until
       2026-11-21 this did not matter because nothing ever called into usb_host.c: its one entry point, hv_usb_probe_start(), is only started from inside host/vm.c's VM
       creation when HV_VUSB_PROBE is set in the environment, and this binary removes every HV_* variable regardless -- see below for why that changed.
     - boot as an installer: installer = 0 (same place), no machine identity (ECID and seed 0), and every HV_* environment variable is removed first (the engine reads some of
       them to switch an installer boot, a RAM file or the USB probe on)
     - [2026-11-20] this binary does now explicitly ask for one non-installer thing: cfg.usb_on_normal_boot = 1 (host/vm.h), so devices/vmapple.c attaches its existing,
       guest-facing-only virtual USB device port (hv_virtio_usb_create -- not host USB, not libirecovery, not restore/usbmuxd code) even though installer stays 0 and the
       ROM's own cfg region still reports run_installer1/run_installer2 as 0. This is a deliberate, explicitly-approved diagnostic: a real-device capture confirmed the
       ROM's own USB-type probe (AVPBooter 0x10C51C/0x10C590) expects USB_DEVICE_TYPE (0x1a01) and fails when a normal boot has no USB channel to answer it. After
       hv_vm_create it checks that the machine's USB device port state matches exactly what was asked for (present, now) -- not merely "absent" as before
     - [2026-11-21] and now a second, explicitly-approved thing: cfg.usb_probe_on_normal_boot = 1 (host/vm.h), so VM creation starts host/usb_host.c's hv_usb_probe_start on
       that port -- the SAME non-destructive USB enumeration (reset, GET_DESCRIPTOR device+config, GET_STATUS, GET_CONFIGURATION, conditional DFU GET_STATE/GET_STATUS; never
       SET_CONFIGURATION/DNLOAD/ABORT/CLRSTATUS) an installer boot already gets via HV_VUSB_PROBE. Real-device logs showed the ROM submitting a 2062-byte receive buffer to
       queue 0 and then waiting (164M+ REG_BUSY reads, q0.done staying 0) because nothing on the host side ever supplied a request for it to receive -- this starts the same
       supplier G0-G6 already use, reusing its code rather than designing a new one. Safe for this binary's one-VM-per-process, no-destroy-call lifecycle: the VM is only ever
       created once per process (see main() below, one hv_vm_create call), hv_vm_stop only halts CPU dispatch and never frees board/vusb (see host/vm.c), and the probe's
       detached thread, however it is blocked, is simply torn down with the whole process on exit -- host/libusb_shim.c (the real restore passthrough) stays unlinked either
       way, so this does not add any path to actual USB device passthrough or restore communication
       -- so an unexpected state (e.g. an installer boot slipping through, or the port failing to attach) still stops the machine rather than running regardless.
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

/* [C1-4] Android boot-branch probe: a read-only bring-up aid for the Android-port boot investigation. Earlier
   rounds (docs/boot-diagnostic-contract-2026-11-15.md) narrowed the SYSTEM_RESET this investigation is tracing
   to a static chain ending in 0x10D0B8 returning 0 to its caller at 0x1071C0 (tested by a tbz at 0x1071C4),
   which then falls through a second check (cbz w8 at 0x107214) into the confirmed panic call at 0x107238. A real
   device confirmed both of those branches are actually taken, with the predicted register values. Disassembly
   of 0x10D0B8 itself found it has (at least) two distinct ways to return 0 to its own caller:
     0x10D14C: bl   0x10C51C
     0x10D150: cbz  w0, 0x10D184        (w0 is 0x10C51C's return value; if NONzero, 0x10D0B8 hardcodes w19=0
                                          at 0x10D17C and returns early -- this is an early-init failure)
     ... (0x10D184 onward: further setup, then)
     0x10D2A4: bl   0x104E04
     0x10D2B8: cbz  x0, 0x10D2E0         (x0 is 0x104E04's return value; if ZERO, 0x10D0B8 takes an error path
                                          instead of the success path at 0x10D2BC, which would set w19=1, record
                                          success in a guest-RAM status byte, and return 1)
   Both paths are consistent with the observed return value 0, so which one the real device actually takes is
   still unmeasured; this round's probe watches these two addresses (instead of 0x1071C4/0x107214, already
   confirmed, so re-watching them would not add information) to find out. The engine has one stop_pc/stop_pc2/
   stop_hook slot (core/machine.h), so only one probe can be installed at a time; switching which addresses it
   watches is a source edit here, not a runtime option (see the comment on hv_runner_scrub_env() below for why
   this can't be an environment-variable switch instead). This only runs on the cpu that reached one of the two
   addresses (core/dispatch.c calls stop_hook there, then continues normal execution -- see that file's own
   comment), so it only ever writes that same cpu's own panic_first/panic_last/panic_hits fields (core/cpu.h) --
   no lock, no cross-CPU access, and no guest-visible state (registers, PC, memory, return values) is touched.
   Installing it even against some other ROM is harmless for the same reason HV_DIAG_PROBE already is: these two
   addresses, if that other ROM never executes them (the overwhelmingly likely case), simply never fire the hook,
   and if they coincidentally do, the hook still only reads registers. HvPanicHit's fields are reused generically
   here (core/cpu.h already documents them as "raw and uninterpreted... specific to the ROM build under
   investigation"): at 0x10D150 .x0 holds w0, masked to 32 bits (the cbz's own tested register, 32-bit); at
   0x10D2B8 .x0 holds the full 64-bit x0 (that cbz tests the full register, sf=1). */
/* [2026-11-20 round] Retargeted again (same single stop_pc/stop_pc2/stop_hook slot as before -- see the comment
   above): the previous round (0x10D150/0x10D2B8) already confirmed 0x10C51C returns a nonzero/failure value
   (0xFFFFFFFD observed) on this normal, non-installer boot. host/vmrun.c's own HV_DIAG_PROBE_CALLER mechanism
   (same file layout, same two-point idea) already narrowed 0x10C51C to a shared BDIF-device validation routine
   called from three known sites, compared at 0x10C590 ("cmp w0, w24"). Independent Capstone disassembly of the
   actual ROM bytes at 0x10C51C (a 512-byte dump read back via the app's own read-only "ROM 구간 읽기" feature)
   found 0x10C590's mismatch path returns -3 (0xFFFFFFFD) at 0x10C6B8 -- matching, not contradicting, the value
   observed on this boot path; an older host/vmrun.c comment's "0x40200003" for this path does not match the
   disassembly and is superseded here. This round watches 0x10C51C itself (point A: the routine's entry, x4 the
   caller's expected device type among other args) and 0x10C590 (point B: right before the compare) to get that
   trace directly from a real no-USB-channel boot (devices/vmapple.c only creates b->vusb for an installer boot
   or HV_USB_ON_NORMAL, and this binary forces installer=0 and strips every HV_* variable -- see the top-of-file
   comment -- so b->usb is NULL here, and devices/bdif.c's bdif_read() falls through to its generic REG_CFG=2
   default instead of USB_DEVICE_TYPE=0x1a01 for exactly that reason). A real-device capture (this binary, 2026-
   11-20) recorded three hits: two at point B with expected(w24)=0x2 actual(w0)=0x2 mismatch=0 (lr=0x10CBE0, an
   unrelated storage-type check that passes), and one with expected(w24)=0x1A01 actual(w0)=0x2 mismatch=1
   (lr=0x10D150 -- the same call site already tied to the observed SYSTEM_RESET above). This directly confirms
   the USB-device-type mismatch as the cause of the 0x10C51C/0x10C590/0xFFFFFFFD failure on this boot path: the
   ROM's USB-type probe expects USB_DEVICE_TYPE (0x1a01) and gets bdif.c's no-USB-channel fallback (2) instead.
   It does not by itself establish what, if anything, happens after that mismatch is fixed. Printed directly (not
   folded into the generic HvPanicHit x0-only reporting below, which can't hold the extra registers this needs)
   so the two points are fully legible without a separate accessor. */
static void panic_probe_hook(HvCpu *cpu) {
    HvPanicHit hit = { .pc = cpu->pc, .lr = cpu->x[30], .sp = cpu->sp };
    if (cpu->pc == 0x10c51cull) {
        /* Point A, 0x10C51C: the validation routine's entry. x4 is the caller's expected device type (not
           compared until point B); x5/x6 are its other options; lr says which of the three known call sites
           reached it this time. */
        hit.x0 = cpu->x[0];
        printf("ui_runner_vm: boot-branch probe A pc=10c51c lr=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx x6=%#llx\n",
               (unsigned long long)cpu->x[30], (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1],
               (unsigned long long)cpu->x[2], (unsigned long long)cpu->x[3], (unsigned long long)cpu->x[4],
               (unsigned long long)cpu->x[5], (unsigned long long)cpu->x[6]);
    } else {
        /* Point B, 0x10C590: right before "cmp w0, w24" executes, so both registers still hold exactly what the
           ROM is about to compare -- neither this hook nor the engine touches them first. 32-bit compare, so the
           mismatch test mirrors that width rather than the full 64-bit x[]. */
        uint32_t actual = (uint32_t)cpu->x[0], expected = (uint32_t)cpu->x[24];
        hit.x0 = actual;
        printf("ui_runner_vm: boot-branch probe B pc=10c590 x21=%#llx actual(w0)=%#x expected(w24)=%#x mismatch=%d\n",
               (unsigned long long)cpu->x[21], actual, expected, actual != expected);
    }
    fflush(stdout);
    if (cpu->panic_hits == 0) cpu->panic_first = hit;
    cpu->panic_last = hit;
    if (cpu->panic_hits != UINT32_MAX) cpu->panic_hits++; /* saturate, never wrap */
}

/* Installs the probe right after the machine is created and before hv_vm_start(). Refuses (prints why, changes
   nothing) rather than overwrite a stop_hook some other debug feature already set -- this binary has none of
   those, but the shared engine does, so this stays defensive the same way host/vmrun.c's diag_probe_install()
   is. Either way normal guest boot proceeds -- a refused install never blocks the boot itself, only this one
   diagnostic. */
#define HV_PANIC_PROBE_PC1 0x10c51cull /* point A: the shared BDIF-device validation routine's entry */
#define HV_PANIC_PROBE_PC2 0x10c590ull /* point B: "cmp w0, w24" right before its type-mismatch check */
static void panic_probe_install(void) {
    HvMachine *vm = hv_vm_machine();
    if (vm->stop_hook) {
        printf("ui_runner_vm: boot-branch probe not installed; a stop hook is already in place\n");
        return;
    }
    vm->stop_pc = HV_PANIC_PROBE_PC1;
    vm->stop_pc2 = HV_PANIC_PROBE_PC2;
    vm->stop_hook = panic_probe_hook;
    printf("ui_runner_vm: boot-branch probe installed at %#llx (0x10C51C entry) / %#llx (0x10C590 compare)\n",
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
    /* [2026-11-20, explicitly approved] hv_runner_make_config leaves this 0 (host/runner_vm_core.c is
       unchanged); this binary alone turns it on, after the config the runner itself guarantees (readonly
       storage, no network, installer=0 -- all untouched above) is already built. See the top-of-file
       comment and devices/vmapple.c for what this does and does not add. */
    cfg.usb_on_normal_boot = 1;
    /* [2026-11-21, explicitly approved, D2] Same reasoning as usb_on_normal_boot just above: hv_runner_make_config
       leaves this 0, this binary alone turns it on. See the top-of-file comment for what hv_usb_probe_start()
       actually does (non-destructive enumeration only) and why this binary's lifecycle makes it safe. */
    cfg.usb_probe_on_normal_boot = 1;
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
    printf("ui_runner_vm config (extra, not in hv_runner_config_line): usb_on_normal_boot=%d usb_probe_on_normal_boot=%d\n",
           cfg.usb_on_normal_boot, cfg.usb_probe_on_normal_boot);
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
    int usb_present = bd && bd->vusb ? 1 : 0;
    printf("ui_runner_vm: machine created in %.0f ms; usb device port present: %d (want %d)\n", (now() - t0) * 1e3, usb_present, cfg.usb_on_normal_boot);
    /* [2026-11-20] Was "must be 0" unconditionally (only an installer boot has the port). Now checked against
       cfg.usb_on_normal_boot instead of hardcoded 0, so the machine still refuses to run on ANY mismatch --
       an installer boot slipping through, or the explicitly-requested port failing to attach -- exactly as
       strictly as the old check did, just against the state this binary now actually asks for. */
    if (!bd || usb_present != cfg.usb_on_normal_boot) {
        printf("ui_runner_vm RESULT: FAILED, usb device port present=%d does not match the requested %d; it is not started\n",
               usb_present, cfg.usb_on_normal_boot);
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
    /* [C1-4] Android boot-branch probe result, as its own line rather than appended to reason_txt above: that
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
