#ifndef HENGEVM_INSTALL_G4_H
#define HENGEVM_INSTALL_G4_H
/* G4: the Android restore entry point (host/install.c, the code the app's install will use) run through the REAL restore tool's own start function, with net_guard
   extended to the SAME three real Apple TSS hosts G2/G3 use (no new host: see below), and the vendored restore tool's own "stop C" (third_party/idevicerestore/
   src/hv_stop.h) used to stop it right after the device is confirmed in MODE_RESTORE -- i.e. right after recovery_enter_restore() sent AppleLogo, the
   components iBoot loads, the restore ramdisk, the device tree, (SEP firmware, if present) and the kernelcache, the device disconnected from Recovery and
   reconnected running that ramdisk (RecoveryOS), and idevicerestore's own wait for MODE_RESTORE succeeded -- but BEFORE "About to restore device..." and
   restore_device() (the actual ASR/FDR disk-writing restore). A test build only: tools/vmrun-install-g4-build.sh compiles host/install.c and host/vmrun.c
   with -DHV_INSTALL_G4 -DHV_STOP_AFTER_RECOVERYOS_ENTRY; nothing shipped sets either.

   G4 goes one stage past G3: it runs G3's whole TSS + DFU->Recovery firmware transfer first (reusing the same code paths, now proven by G3), then continues
   past stop B into recovery_enter_restore() and stops only at stop C. Every component recovery_enter_restore() sends (AppleLogo, iBoot-loaded components,
   ramdisk, device tree, SEP, kernelcache) was already extracted from the same already-downloaded IPSW and signed by the SAME TSS tickets fetched in stage 1
   -- recovery_send_ticket() sends the ApTicket already held in client->tss, it does not fetch a new one. So G4 makes NO new network request of its own and
   widens net_guard no further than G2/G3 already do. Disk and auxiliary storage stay read-only and are hash-verified unchanged before/after, exactly as in
   G0/G1/G2/G3, because recovery_enter_restore() never writes to the virtual disk -- it only gets the device running RecoveryOS off its own ramdisk; the
   actual ASR/FDR disk-writing restore happens strictly AFTER stop C and is explicitly out of this stage's scope. device_requests reuses G3's own
   classification (dnload/getstatus/getstate/reset/handshake/command_execute/enumreq/forbidden): recovery_enter_restore() sends its components through the
   exact same irecv_send_buffer()/irecv_send_command()/irecv_send_command_breq() calls G3 already analyzed, just more of them. */
#include <stdint.h>
#include <stdio.h>

#include "virtio_usb.h"

/* host/install.c, G4 build: the flag that stops the tool right after RecoveryOS entry is confirmed (and installs the hook that reports it). */
int hv_g4_extra_flags(void);
/* host/install.c, G4 build: what the restore thread saw, recorded before it publishes the result. */
void hv_g4_record(int engine_rc, int closed, int cleanup_mask, int final_report);

/* The checks before the run: both storage files must already exist (G4 creates nothing), then their identity is taken. Also makes the tool's log print
   without the ECID/nonce/ticket pattern. 0, or -1 with the reason printed. */
int hv_g4_begin(const char *disk, const char *aux, uint64_t ecid);
/* Waits (60 s) for the guest on the USB bus, then starts the restore through hv_install_start. 0, or -1. */
int hv_g4_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
/* Waits for the end of the run (cap_ms), asks it to stop once if it does not end, and waits 20 s more. 1: it ended by itself, 0: it needed the cancel, -1: it
   did not end at all (then nothing may be freed or closed: the caller ends the process). */
int hv_g4_wait(int cap_ms);
/* The verdict table to `out`. Returns the number of FAILED rows (0 = G4 passed). The result line is one of g4_passed (disk and aux content verified),
   g4_passed_metadata_only (the disk was compared by size and time only) or FAILED. */
int hv_g4_finish(FILE *out, const char *disk, const char *aux);

#endif
