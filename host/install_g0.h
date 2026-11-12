#ifndef HENGEVM_INSTALL_G0_H
#define HENGEVM_INSTALL_G0_H
/* G0: the Android restore entry point (host/install.c, the code the app's install will use) run once, up to the restore tool's own stop A -- after the
   component check, before the root filesystem step, before any signing request and before the device is touched again. A test build only:
   tools/vmrun-install-g0-build.sh compiles host/install.c and host/vmrun.c with -DHV_INSTALL_G0 -DHV_STOP_BEFORE_ROOTFS; nothing shipped sets either.

   What G0 decides is kept apart from what the restore session decides. For the session a stop-A return (217) is "the restore failed" (final report 2) and
   that classification is not changed; G0 reads the session's report, the tool's return value and the stop-A hook separately and passes only when ALL of
   these hold: the hook was called, the tool returned 217, the session closed with cleanup_mask 0 and report 2, no connection attempt was made or refused, no
   firmware or vendor/download request went to the device, no write was asked of the disk or the auxiliary storage, their size, modification time and content
   did not change, and the restore thread ended (after a cancel request if it had to be asked). A refused connection is NOT a pass: it means the run went
   past where it should have stopped. */
#include <stdint.h>
#include <stdio.h>

#include "virtio_usb.h"

/* host/install.c, G0 build: the flag that stops the tool (and installing the hook that reports it). */
int hv_g0_extra_flags(void);
/* host/install.c, G0 build: what the restore thread saw, recorded before it publishes the result. */
void hv_g0_record(int engine_rc, int closed, int cleanup_mask, int final_report);

/* The checks before the run: both storage files must already exist (G0 creates nothing), then their identity is taken. Also turns the curl stand-in's deny
   mode on, takes the counters and makes the tool's log print without the ECID. 0, or -1 with the reason printed. */
int hv_g0_begin(const char *disk, const char *aux, uint64_t ecid);
/* Waits (60 s) for the guest on the USB bus, then starts the restore through hv_install_start. 0, or -1. */
int hv_g0_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
/* Waits for the end of the run (cap_ms), asks it to stop once if it does not end, and waits 20 s more. 1: it ended by itself, 0: it needed the cancel, -1: it did
   not end at all (then nothing may be freed or closed: the caller ends the process). */
int hv_g0_wait(int cap_ms);
/* Test only: the amount of data above which a storage file is compared by size and modification time instead of content (default 2 GiB). */
void hv_g0_set_content_cap(uint64_t bytes);
/* The verdict table to `out`. Returns the number of FAILED rows (0 = G0 passed). The result line is one of g0_passed (disk and aux content verified), g0_passed_metadata_only (the disk was compared by size and time only) or FAILED. */
int hv_g0_finish(FILE *out, const char *disk, const char *aux);

#endif
