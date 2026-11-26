/* A FAKE machine behind the hv_vm_* functions of host/vm.h, for tests only (never linked into a shipped binary: it lives in tests/, and tools/check-runner-vm-safe.sh looks for
   its marker string in the shipped runner). It lets host/ui_runner_vm.c run as a real process on ANY Linux (x86 too) and under ASan / UBSan / TSan, so the runner's own
   flow (start, the loop, every way of stopping, the exit codes, the shared file) is tested without the engine.
   What it does: hv_vm_create opens the three files read-only and refuses when one is missing; the board's USB port mirrors cfg.usb_on_normal_boot (host/vm.h) -- present
   (pointing at a zeroed, never-dereferenced stand-in HvVirtioUsb) when the caller asked for it, NULL otherwise -- matching host/vm.c's real hv_vm_create() contract
   closely enough for host/ui_runner_vm.c's post-create "usb device port present" check (which only tests the pointer for non-NULL, never dereferences it: devices/vmapple.c
   and host/usb_host.c, the only code that would, are not linked into this fake build at all) without needing the real USB device machinery here. hv_vm_start runs a thread
   that "stops" (calls on_exit) once hv_vm_stop was called, the boot display and the pointer are never there (-1), the input functions only count. Disk write statistics stay 0.
   What it does NOT do: run any guest code, start the USB enumeration probe, or anything else the engine does. A pass with this fake says nothing about the engine. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vm.h"

const char *const hv_test_fake_machine_marker = "HV-TEST-FAKE-MACHINE";

uint64_t hv_net_tx_frames, hv_net_rx_frames;

static HvVmapple g_board;
/* A zeroed stand-in for the real USB device port object. Never dereferenced: host/ui_runner_vm.c only tests
   g_board.vusb for non-NULL, and devices/vmapple.c / host/usb_host.c (the code that would actually read or
   write through it) are not part of this fake build (see tools/c1vm-test.sh's PART A link line). It exists
   only so g_board.vusb can point at something real instead of an arbitrary non-NULL value. */
static HvVirtioUsb g_fake_vusb;
static atomic_int g_stop, g_started;
static void (*g_on_exit)(int);
static pthread_t g_th;
static atomic_ullong g_inputs;

int hv_vm_create(const HvVmConfig *c, char *err, size_t errlen) {
    if (!c->rom || !c->disk || !c->aux) { snprintf(err, errlen, "The ROM, disk and auxiliary storage are all needed."); return -1; }
    const char *paths[3] = {c->rom, c->disk, c->aux};
    for (int i = 0; i < 3; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) { snprintf(err, errlen, "Cannot open %s.", i == 0 ? "the ROM" : i == 1 ? "the disk image" : "the auxiliary storage"); return -1; }
        close(fd);
    }
    memset(&g_board, 0, sizeof g_board);
    memset(&g_fake_vusb, 0, sizeof g_fake_vusb);
    /* Mirrors devices/vmapple.c's real board-creation condition ("if (cfg->run_installer1 || cfg->run_installer2
       || cfg->usb_on_normal_boot || getenv(\"HV_USB_ON_NORMAL\"))"), minus the getenv: every caller of this fake
       (host/ui_runner_vm.c is the only one) already strips every HV_* variable before building and passing c, so
       by the time c reaches here the environment can no longer affect the answer either way, same as it cannot
       in the real function once that stripping has happened. c->installer (host/vm.h) is what the real
       hv_vm_create() turns into run_installer1=run_installer2=1, so it stands in for both here. */
    g_board.vusb = (c->installer || c->usb_on_normal_boot) ? &g_fake_vusb : NULL;
    return 0;
}
HvVmapple *hv_vm_board(void) { return &g_board; }

static void *fake_cpu0(void *a) {
    (void)a;
    struct timespec t = {0, 2 * 1000 * 1000};
    while (!atomic_load(&g_stop)) nanosleep(&t, NULL);
    if (g_on_exit) g_on_exit(1);
    return NULL;
}
void hv_vm_start(int cpu0, void (*on_exit)(int reason)) {
    (void)cpu0;
    g_on_exit = on_exit;
    atomic_store(&g_started, 1);
    pthread_create(&g_th, NULL, fake_cpu0, NULL);
    pthread_detach(g_th);
}
void hv_vm_stop(void) { atomic_store(&g_stop, 1); }
void hv_vm_flush_disks(void) {}
int hv_vm_flush_disks_checked(void) { return 0; }
int hv_vm_quiesced_ok(void) { return 1; }
int hv_vm_quiesce(void) { return 0; }
void hv_vm_disk_write_stats(uint64_t *calls, uint64_t *refused) { *calls = 0; *refused = 0; }
const char *hv_vm_reason_name(int reason) { (void)reason; return "fake machine stopped"; }
/* The fake machine never runs guest code, so it never takes a PSCI call either: always "no halt record". */
int hv_vm_psci_halt_info(uint32_t *fn, uint32_t *cpu_index, uint64_t *pc, uint64_t *lr) {
    (void)fn; (void)cpu_index; (void)pc; (void)lr;
    return 0;
}
int hv_vm_psci_halt_stack(uint64_t *sp, uint64_t *x26, uint64_t *stack, int *n_words) {
    (void)sp; (void)x26; (void)stack; (void)n_words;
    return 0;
}
int hv_vm_panic_probe(uint32_t *hits, HvPanicHit *first, HvPanicHit *last) {
    (void)hits; (void)first; (void)last;
    return 0;
}
/* The fake machine has no HvMachine at all (g_board above is an HvVmapple, not an HvMachine), so there is nothing
   real to hand back. panic_probe_install() in host/ui_runner_vm.c only reads stop_hook off this pointer before
   deciding whether to install itself, so a zeroed-out static HvMachine is enough to let that check run safely
   under the fake machine without ever actually hooking anything (the fake machine runs no guest code to hook). */
static HvMachine g_fake_machine;
HvMachine *hv_vm_machine(void) { return &g_fake_machine; }
int hv_vm_boot_display(HvBootDisplay *d) { (void)d; return -1; }
int hv_vm_pointer(int *x, int *y) { (void)x; (void)y; return -1; }
void hv_vm_key(uint16_t code, int down) { (void)code; (void)down; atomic_fetch_add(&g_inputs, 1); }
void hv_vm_mouse_move(int dx, int dy) { (void)dx; (void)dy; atomic_fetch_add(&g_inputs, 1); }
void hv_vm_mouse_button(int button, int down) { (void)button; (void)down; atomic_fetch_add(&g_inputs, 1); }
void hv_vm_mouse_wheel(int dy) { (void)dy; atomic_fetch_add(&g_inputs, 1); }
void hv_vm_mouse_hwheel(int dx) { (void)dx; atomic_fetch_add(&g_inputs, 1); }
int hv_vm_cursor_wake_open(void) { return -1; }
void hv_vm_cursor_wake_drain(void) {}
int hv_vm_cursor_stats(uint64_t *a, uint64_t *b, uint64_t *c, int *x, int *y, int *f) { (void)a; (void)b; (void)c; (void)x; (void)y; (void)f; return 0; }
