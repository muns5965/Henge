#ifndef HENGEVM_VM_H
#define HENGEVM_VM_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h" /* HvPanicHit, for hv_vm_panic_probe() below */
#include "disk.h"
#include "vmapple.h"

/* The virtual Mac as a host app drives it: build it from files, run it, feed
   it input, stop it. The macOS command-line tool and the iOS app both use this;
   graphics are the host's own business (Mac: hv_pvg_attach before starting). */
typedef struct HvVmConfig {
    const char *rom;          /* AVPBooter.vmapple2.bin */
    const char *disk, *aux;   /* the root disk image and the auxiliary storage */
    uint64_t aux_offset;      /* where the aux image's data starts (16 KB) */
    uint64_t ecid, seed;      /* the machine identity the disk was installed with */
    uint64_t ram_mb;
    int ncpus;
    int log;                  /* m->log */
    int no_net, no_sound;
    int installer;            /* boot the ROM in DFU mode with the USB device port (installing macOS) */
    int readonly_storage;     /* open the disk and the auxiliary storage read-only (the G0 test: any write the guest asks for is refused and counted) */
    /* JIT memory the host already has (iOS: granted by a debugger), or NULL to map it here. */
    void *jit_rx, *jit_rw;
    size_t jit_bytes;         /* 0: 768 MB */
} HvVmConfig;

/* Returns 0, or -1 with a reason in err. */
int hv_vm_create(const HvVmConfig *cfg, char *err, size_t errlen);
HvMachine *hv_vm_machine(void);
HvVmapple *hv_vm_board(void);
HvCpu *hv_vm_cpu(int index);
extern void (*hv_prof_register)(void); /* debugging: each thread that will run guest code calls this */
/* Frames go to this CAMetalLayer, drawn by the GPU, instead of hv_vm_boot_display. */
void hv_vm_set_display_layer(void *layer);
uint64_t hv_vm_gpu_frames(void);
int hv_vm_ncpus(void);

/* The boot framebuffer iBoot hands the kernel (XNU's boot_args video info):
   the Apple logo and progress bar while booting or installing, before a GPU
   driver takes the display. Host pointer to the pixels (32-bit BGRA), or NULL
   until the kernel has its boot_args. */
typedef struct HvBootDisplay {
    const uint8_t *pixels;
    uint32_t width, height, stride;
} HvBootDisplay;
int hv_vm_boot_display(HvBootDisplay *d);

/* Extra device work on CPU 0 each tick (Mac graphics). Set before hv_vm_start. */
void hv_vm_set_poll(void (*poll)(void));

/* Runs CPU 0 on a new thread (unless the caller runs it: cpu0 = 0), the other
   CPUs and the 1 ms kicker. on_exit is called on CPU 0's thread when the
   machine stops (powered off, a fatal error, or hv_vm_stop), after the disks
   are flushed; reason is an HV_EXIT_* value. */
void hv_vm_start(int cpu0, void (*on_exit)(int reason));
/* Call on a thread before it runs guest code (hv_vm_start's threads do): see vm.c. */
void hv_vm_prepare_cpu_thread(void);
/* That, and the scheduling class the guest's CPUs run at. */
void hv_vm_cpu_qos(void);
/* Run CPU 0 on the calling thread (what hv_vm_start does on its own thread); returns its exit reason. */
int hv_vm_run_cpu0(void);
/* Stop at the next block; the CPUs' threads end. */
void hv_vm_stop(void);
void hv_vm_flush_disks(void);
/* Writes the devices asked for on the root disk and the auxiliary storage (calls, and those refused). */
void hv_vm_disk_write_stats(uint64_t *calls, uint64_t *refused);
const char *hv_vm_reason_name(int reason);
/* Which PSCI call halted the machine (SYSTEM_OFF 0x84000008 or SYSTEM_RESET 0x84000009), and the CPU/PC/LR
   captured at that call (core/exception.c's psci(), core/machine.h's psci_halt_* fields) -- not reconstructed
   from CPU state after the fact. Returns 0 (every out-param left untouched) when the machine has not halted
   via PSCI at all (still running, or stopped some other way: wfi, a fault, hv_vm_stop, a test's own stop_pc),
   OR when a writer has claimed the record but not yet finished filling it in (core/machine.h's psci_halt_valid
   comment: this is a three-state publish precisely so that half-written fields are never handed back, even
   when the CPU that fills them is not the one whose thread later tells host code the machine halted).
   Any out-param may be NULL. Independent of HV_PSCI_LOG and every other HV_* environment variable. */
int hv_vm_psci_halt_info(uint32_t *fn, uint32_t *cpu_index, uint64_t *pc, uint64_t *lr);

/* A second accessor for the same captured halt (0 under the exact same conditions as hv_vm_psci_halt_info,
   and always 1 at the same time that one is, since both read the one psci_halt_valid publish) -- kept
   separate rather than widening hv_vm_psci_halt_info's signature, so existing callers of that one are
   untouched. sp and x26 are live-register reads, always meaningful once this returns 1. stack is filled
   with up to HV_PSCI_HALT_STACK_WORDS (core/machine.h; 16 as of this writing) uint64_t words of RAW guest
   memory read from [sp, sp+HV_PSCI_HALT_STACK_WORDS*8) with no guest-visible side effect (core/exception.c's
   psci_halt_peek_u64); *n_words says how many of those the caller actually wrote (fewer only if an early
   word's guest-virtual address did not resolve to plain, readable guest RAM). Interpreting any of those
   words (e.g. "offset 0x38 is an outer caller's saved return address") is specific to how the ROM being
   investigated happens to lay out its stack frames at the point captured; this call does not assume or
   compute that -- it hands back the bytes. stack must point at space for HV_PSCI_HALT_STACK_WORDS
   uint64_t; any of sp/x26/stack/n_words may be NULL. */
int hv_vm_psci_halt_stack(uint64_t *sp, uint64_t *x26, uint64_t *stack, int *n_words);

/* [C1-4] A third accessor for the same published halt (0 under the exact same conditions as the two above, and
   always 1 at the same time they are): host/ui_runner_vm.c's own panic-entry probe, a separate mechanism from
   HV_DIAG_PROBE/_CALLER above (those are host/vmrun.c only, and gated by HV_* environment variables that
   host/ui_runner_vm.c deliberately scrubs before creating the machine -- see hv_runner_scrub_env). When
   ui_runner_vm.c installs the engine's two stop_pc slots at a pair of known generic-panic-helper entry points in
   the AVPBooter ROM under investigation, this reports the first and the last time the cpu that made the PSCI
   call itself hit either one, and how many times in total (core/cpu.h's HvPanicHit/HvCpu.panic_first/panic_last/
   panic_hits, core/machine.h's published copy). *hits == 0 means that cpu never hit either installed address
   before this halt -- a real, reportable outcome (the panic path, if any, did not go through those two
   addresses), not a sign the probe failed to install or run; *first and *last are then left unset (zero) and
   should not be printed as if they were a hit. This is read-only on every path: no guest register, PC or memory
   is touched by the probe itself, only this cpu's own panic_first/panic_last/panic_hits (and only on the cpu
   that hit the probe address) are written, and this accessor only ever copies that CPU's own committed snapshot
   out after the machine has already halted via PSCI. Known gap, accepted for this investigation's scope: a panic
   that does not end in a PSCI halt (recovered from, or the machine stops some other way) never publishes here,
   even if the probe did record hits on some cpu -- see docs/boot-diagnostic-contract-2026-11-15.md. */
int hv_vm_panic_probe(uint32_t *hits, HvPanicHit *first, HvPanicHit *last);

/* Input from the host UI, any thread. Keys are Linux input codes (KEY_A = 30). */
void hv_vm_key(uint16_t code, int down);
void hv_vm_key_hid(uint8_t usage, int down); /* HID keyboard usage, which is what iOS reports for a hardware keyboard */
void hv_vm_mouse_move(int dx, int dy);
void hv_vm_mouse_button(int button, int down); /* 0 left, 1 right, 2 middle */
void hv_vm_mouse_wheel(int dy);
void hv_vm_mouse_hwheel(int dx);
/* Where the guest has its pointer, in display pixels. 0, or -1 before it has one. */
int hv_vm_pointer(int *x, int *y);
/* A small picture of the display, BGRA, width * height * 4 bytes. 0, or -1 before anything shows. */
int hv_vm_snapshot(uint8_t *bgra, int width, int height);

/* Which of the two independent sources (if either) produced the frame hv_vm_capture_screen below copied out:
   HV_SCREEN_NONE neither has a frame yet; HV_SCREEN_PVG the GPU's own snapshot (hv_vm_snapshot's path, a
   Metal-rendered frame); HV_SCREEN_BOOT_ARGS the raw framebuffer XNU's boot_args points at in guest RAM (the
   Apple logo and progress bar while booting/installing, before any GPU driver takes the display -- what
   hv_vm_boot_display's own fallback finds). Diagnostic only: this classification is informational and gates
   nothing on its own. */
enum { HV_SCREEN_NONE = 0, HV_SCREEN_PVG = 1, HV_SCREEN_BOOT_ARGS = 2 };
/* The boot_args source frame's own size, before hv_vm_capture_screen scales it to the caller's width x height.
   Left all-zero when the frame came from hv_vm_snapshot (PVG) instead: that accessor already renders at exactly
   the requested size, so it has no separate "original" size of its own to report, and when *source comes back
   HV_SCREEN_NONE there was no frame to measure. */
typedef struct { uint32_t orig_width, orig_height, orig_stride; } HvScreenCaptureInfo;
/* Tries hv_vm_snapshot (PVG) first; only if that has nothing yet, falls back to hv_vm_boot_display's raw
   framebuffer, SCALED (nearest-neighbor on both axes, independently) to exactly fill width x height -- not
   cropped to its top-left corner: a boot logo is typically centered, so on a source larger than width x height a
   crop could miss content that a scale would still show, and this diagnostic snapshot only needs the content to
   be recognizable, not pixel-accurate or aspect-correct (there is no "correct" aspect ratio to preserve against a
   destination size that has nothing to do with the guest's actual resolution). *source reports which of the two
   sources (if either) actually supplied the frame; info, if non-NULL, reports the boot_args source frame's own
   width/height/stride (see HvScreenCaptureInfo above). Changes neither hv_vm_snapshot's nor hv_vm_boot_display's
   own behavior -- this only combines and scales their existing results. */
int hv_vm_capture_screen(uint8_t *bgra, int width, int height, int *source, HvScreenCaptureInfo *info);

#endif
