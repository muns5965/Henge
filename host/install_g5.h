#ifndef HENGEVM_INSTALL_G5_H
#define HENGEVM_INSTALL_G5_H
/* G5: the Android restore entry point (host/install.c) run through the REAL restore tool's own start function, one stage past G4: this build also enters
   restore_device() itself (G4 never does) and runs it up through the real `restored` handshake -- restore_open_with_timeout() (opens the device's restored
   service and confirms its type is "com.apple.mobile.restored", recording the protocol version) and the HardwareInfo/SavedDebugInfo queries and the
   conditional BBTicket copy (reusing the already-fetched TSS blob, no new Apple request) -- then stops at the vendored tool's own "stop D"
   (third_party/idevicerestore/src/hv_stop.h), strictly BEFORE FDR starts and BEFORE restored_start_restore() (the actual ASR/FDR disk-writing restore). A
   test build only: tools/vmrun-install-g5-build.sh compiles host/install.c and host/vmrun.c with -DHV_INSTALL_G5 -DHV_STOP_AFTER_RESTORED_HANDSHAKE; nothing
   shipped sets either.

   G5 goes one stage past G4: it runs G4's whole TSS + DFU->Recovery + RecoveryOS-entry stage first (reusing the same code paths, now proven by G4 through a
   real device), then continues past stop C into restore_device() and stops only at stop D. No new network host and no new signature request: the
   HardwareInfo/SavedDebugInfo queries are local reads over the already-open restored/usbmuxd link, and the conditional BBTicket copy reuses
   client->tss (the SAME TSS response fetched in stage 1), it fetches nothing new. Disk and auxiliary storage stay read-only and are hash-verified unchanged
   before/after, exactly as in G0-G4, because reaching stop D never calls restored_start_restore() or starts FDR -- the host never sends the actual
   restore-start command, so storage writes continue to be refused exactly as before this point (the guest's own AUX write attempts, observed since Henge
   build 155, are unaffected and remain refused by the read-only storage policy; this is NOT "no disk-related signal is sent at all").

   Required pass conditions for G5 (deliberately narrower than "reached stop D"): the restored service type and protocol version must be confirmed (implied
   by reaching stop D at all, since restore_open_with_timeout() returns an error otherwise and restore_device() returns early before stop D is ever
   reachable); the HardwareInfo query must have independently succeeded (hwinfo_status == HV_INFO_OK; restore_device() itself tolerates this query's failure
   without aborting, so reaching stop D alone does NOT prove it succeeded); restored_start_restore() must never have been called (0 call count); FDR must
   never have been initialized (0 call count); disk and aux content must be unchanged (hash-verified, same as G0-G4). SavedDebugInfo's status is reported
   for information only (success/not-provided/failed) and is NEVER required: a device with no prior restore attempt legitimately has nothing to give there.
   The BBTicket copy is reported for information only and is NEVER required: a build/device with no baseband (this project's virtual Mac, for instance) has
   no BBTicket in its TSS response at all, and that is not a failure. USB bulk/interrupt transfer failure counts (the usbmuxd standing-read teardown pattern
   G4 root-caused and classified) are kept purely OBSERVATIONAL/informational for G5's first real run and never gate the pass/fail decision: G5 opens more
   connections before any teardown than G4 did, so the count at teardown time may legitimately differ from G4's fixed 3. */
#include <stdint.h>
#include <stdio.h>

#include "virtio_usb.h"

/* host/install.c, G5 build: the flag that stops the tool right after the restored handshake/queries/BBTicket-copy are confirmed (and installs the hook that
   reports it). */
int hv_g5_extra_flags(void);
/* host/install.c, G5 build: what the restore thread saw, recorded before it publishes the result. */
void hv_g5_record(int engine_rc, int closed, int cleanup_mask, int final_report);

/* The checks before the run: both storage files must already exist (G5 creates nothing), then their identity is taken. Also makes the tool's log print
   without the ECID/nonce/ticket pattern. 0, or -1 with the reason printed. */
int hv_g5_begin(const char *disk, const char *aux, uint64_t ecid);
/* Waits (60 s) for the guest on the USB bus, then starts the restore through hv_install_start. 0, or -1. */
int hv_g5_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
/* Waits for the end of the run (cap_ms), asks it to stop once if it does not end, and waits 20 s more. 1: it ended by itself, 0: it needed the cancel, -1: it
   did not end at all (then nothing may be freed or closed: the caller ends the process). */
int hv_g5_wait(int cap_ms);
/* The verdict table to `out`. Returns the number of FAILED rows (0 = G5 passed). The result line is one of g5_passed (disk and aux content verified),
   g5_passed_metadata_only (the disk was compared by size and time only) or FAILED. */
int hv_g5_finish(FILE *out, const char *disk, const char *aux);

#endif
