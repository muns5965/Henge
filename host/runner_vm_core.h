#ifndef HENGEVM_RUNNER_VM_CORE_H
#define HENGEVM_RUNNER_VM_CORE_H
/* C1-2: what the VM runner (host/ui_runner_vm.c) does between the shared file (host/frame_shm.h) and the virtual machine (host/vm.h), kept apart from the machine so a
   test can put a recorder where the machine would be. Nothing in here starts or stops a machine and nothing links host/vm.c.

   THE API TABLE. Every call into the machine goes through one HvRunnerApi. ui_runner_vm.c passes a `static const` table of the real hv_vm_* functions and has no other way
   to get a table (no environment variable, no argument, no hook); tests/test_c1vm.c has its own main that passes a recorder. So the substitution exists only in the
   test binary; the shipped runner cannot be given another table.

   THE SAFETY SETTINGS. hv_runner_make_config fills the HvVmConfig that is passed to hv_vm_create; read-only storage, no network, no installer boot, no identity (ECID and seed
   0) and no host-provided JIT memory are set there and nowhere else. hv_runner_config_ok re-checks the finished structure and the runner refuses to create the machine when
   it fails; hv_runner_config_line prints the fields of that same structure. hv_runner_scrub_env removes every HV_* variable first: the engine reads some of them to turn on an
   installer boot (HV_RUN_INSTALLER, HV_USB_ON_NORMAL), a RAM file (HV_RAM_FILE) or the USB probe (HV_VUSB_PROBE), none of which this runner may ever do.

   THE COUNTERS. They say what the runner OBSERVED, not what the guest did:
     cmds_popped     commands taken out of the shared ring
     cmds_dispatched popped commands that made at least one call into the machine
     cmds_skipped    valid commands dropped because the machine was not ready (before it started or after the stop began)
     cmds_rejected   popped commands that were not valid (unknown type, argument out of range): no call was made
       popped = dispatched + skipped + rejected (one command is exactly one of the three); calls_made is counted on its own and is NOT popped: one command may become more
       than one call some day. A call returning is not the guest having seen the input: hv_vm_key and the mouse calls return nothing.
     calls_made      calls to the machine's input functions
     queries_made    calls to hv_vm_boot_display / hv_vm_pointer
     display_present / display_absent   the answer to "is there a boot picture" (0 / -1)
     pointer_valid / pointer_absent     the answer to "does the guest have a pointer" (0 / -1)
     call_errors     answers that broke the function's own contract (a picture with no pixels, a size that cannot be, a pointer outside 0..65535)
     shm_errors      the shared file's lock could not be taken, or a picture was refused, in a call this module made
     frames_published  pictures copied into the shared file
   Single-threaded: one thread owns an HvRunner and makes every call on it. */
#include <stddef.h>
#include <stdint.h>

#include "display_buf.h"
#include "frame_shm.h"
#include "vm.h"

typedef struct {
    int (*boot_display)(HvBootDisplay *d);
    int (*pointer)(int *x, int *y);
    void (*key)(uint16_t code, int down);
    void (*mouse_move)(int dx, int dy);
    void (*mouse_button)(int button, int down);
    void (*mouse_wheel)(int dy);
    void (*mouse_hwheel)(int dx);
} HvRunnerApi;

typedef struct {
    uint64_t cmds_popped, cmds_dispatched, cmds_skipped, cmds_rejected;
    uint64_t calls_made, queries_made;
    uint64_t display_present, display_absent, pointer_valid, pointer_absent;
    uint64_t call_errors, shm_errors, frames_published;
} HvRunnerStats;

typedef struct HvRunner HvRunner;

/* api must outlive the runner. shm is not owned. NULL when memory runs out. */
HvRunner *hv_runner_create(const HvRunnerApi *api, HvShm *shm);
/* Only after hv_runner_close_display and with no thread inside any call. */
void hv_runner_destroy(HvRunner *r);

/* Commands popped while not ready are counted as skipped and thrown away (stale input is not replayed later). Starts not ready. */
void hv_runner_set_ready(HvRunner *r, int ready);
/* Pops every waiting command and applies it. Returns how many were popped, or -1 when the shared file's lock could not be taken (nothing popped in that call). */
int hv_runner_pump_cmds(HvRunner *r);
/* Asks the machine for the pointer and writes the answer to the shared file. 1 valid, 0 not there yet, -1 the answer broke the contract (treated as not there). */
int hv_runner_pump_pointer(HvRunner *r);
/* Asks the machine for the boot picture through display_buf's pull (the guest pointer lives only inside the fetch), and publishes a new picture to the shared file.
   Returns display_buf's answer: HV_DISP_NEW, HV_DISP_NONE (no picture, or one that broke the contract: call_errors), HV_DISP_CLOSED (closed: the machine was not asked). */
int hv_runner_pump_frame(HvRunner *r);
/* After this the machine is never asked for a picture again; call it BEFORE stopping the machine (display_buf's contract). Idempotent. */
void hv_runner_close_display(HvRunner *r);

void hv_runner_stats(const HvRunner *r, HvRunnerStats *out);

/* ---- safety ---- */
/* The only place the machine's settings are made. rom, disk, aux are paths the caller names; everything else is fixed here except the sizes. */
void hv_runner_make_config(HvVmConfig *c, const char *rom, const char *disk, const char *aux, uint64_t ram_mb, int ncpus, size_t jit_mb);
/* 0 when the structure is exactly what a read-only, offline, non-installer run needs (storage read-only, no network, no installer, ECID and seed 0, no host JIT memory,
   sizes in range, the three paths set); -1 with the first thing wrong in why. */
int hv_runner_config_ok(const HvVmConfig *c, char *why, size_t cap);
/* The fields of the structure that is about to be passed to hv_vm_create, as one line "readonly_storage=1 no_net=1 ..."; paths are not included. The ECID and the seed are
   printed only as "identity=none" (they are 0 here, and an identity is never printed). */
void hv_runner_config_line(const HvVmConfig *c, char *out, size_t cap);
/* Removes every environment variable whose name starts with HV_. Returns how many; their names (not values) go into names, comma separated, if not NULL. */
int hv_runner_scrub_env(char *names, size_t cap);

/* The command rules, for tests: 1 valid, 0 not. */
int hv_runner_cmd_valid(const HvShmCmd *c);

#endif
