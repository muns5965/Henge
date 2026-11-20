#ifndef HENGEVM_INSTALL_G3_H
#define HENGEVM_INSTALL_G3_H
/* G3: the Android restore entry point (host/install.c, the code the app's install will use) run through the REAL restore tool's own start function, with net_guard
   extended to the same three real Apple TSS hosts G2 uses, and the vendored restore tool's own "stop B" (third_party/idevicerestore/src/hv_stop.h) used to
   stop it right after the device left DFU mode -- i.e. right after the personalized firmware (iBSS/iBEC, signed by the same three TSS tickets G2 verifies)
   was sent over USB and the device disconnected and reconnected in Recovery mode -- but BEFORE the post-mode-change nonce recheck and BEFORE
   recovery_enter_restore()/restore_device() (the actual disk-writing restore). A test build only: tools/vmrun-install-g3-build.sh compiles host/install.c and
   host/vmrun.c with -DHV_INSTALL_G3 -DHV_STOP_AFTER_DFU_RECOVERY; nothing shipped sets either.

   G3 is the opposite of G0/G1/G2 on one specific point: those three builds all required that NO firmware/vendor transfer reached the device (0 forbidden
   requests). G3's whole purpose is to confirm that it DID -- so its device_requests row requires at least one DFU DNLOAD transfer to have happened, while
   still requiring 0 vendor-class requests, 0 UPLOAD/DETACH/CLRSTATUS/ABORT requests, and 0 bulk/interrupt transfers (the DFU download stage uses the control
   endpoint only). Disk and auxiliary storage stay read-only and are hash-verified unchanged before/after, exactly as in G0/G1/G2, because this stage never
   touches the virtual disk. net_guard is not widened beyond the three TSS hosts G2 already allows: this stage makes no new network request of its own, it
   only consumes TSS tickets fetched earlier in the same call. */
#include <stdint.h>
#include <stdio.h>

#include "virtio_usb.h"

/* host/install.c, G3 build: the flag that stops the tool right after the DFU->Recovery transition (and installs the hook that reports it). */
int hv_g3_extra_flags(void);
/* host/install.c, G3 build: what the restore thread saw, recorded before it publishes the result. */
void hv_g3_record(int engine_rc, int closed, int cleanup_mask, int final_report);

/* The checks before the run: both storage files must already exist (G3 creates nothing), then their identity is taken. Also makes the tool's log print
   without the ECID/nonce/ticket pattern. 0, or -1 with the reason printed. */
int hv_g3_begin(const char *disk, const char *aux, uint64_t ecid);
/* Waits (60 s) for the guest on the USB bus, then starts the restore through hv_install_start. 0, or -1. */
int hv_g3_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
/* Waits for the end of the run (cap_ms), asks it to stop once if it does not end, and waits 20 s more. 1: it ended by itself, 0: it needed the cancel, -1: it
   did not end at all (then nothing may be freed or closed: the caller ends the process). */
int hv_g3_wait(int cap_ms);
/* The verdict table to `out`. Returns the number of FAILED rows (0 = G3 passed). The result line is one of g3_passed (disk and aux content verified),
   g3_passed_metadata_only (the disk was compared by size and time only) or FAILED. */
int hv_g3_finish(FILE *out, const char *disk, const char *aux);

#endif
