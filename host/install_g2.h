#ifndef HENGEVM_INSTALL_G2_H
#define HENGEVM_INSTALL_G2_H
/* G2: the Android restore entry point (host/install.c, the exact code the app's real install would use) run once, up to and including the TSS stage that a
   macOS Customer Erase Install needs -- idevicerestore.c's OWN code (get_tss_response / get_local_policy_tss_response / get_recoveryos_root_ticket_tss_response,
   reached through FLAG_SHSHONLY), not host/tss_probe_core.c's separate hand-built mirror of the same three requests. Real Apple signing server, real hosts: the
   only net_guard allowance this build adds beyond the private usbmuxd listener is the three hosts libtatsu already tries for TSS (gs.apple.com and its two CDN
   aliases, port 443, HTTP purpose). Nothing else -- no FDR, no URLAsset, no other host -- is approved; a connection attempt to anything else is refused and
   counted, which is treated as not a pass (the run went further than this stage is meant to reach).

   FLAG_SHSHONLY makes the tool return right after the three TSS responses are in hand: no root filesystem step, no firmware sent to the device, nothing but the
   nonce reads and ordinary control requests reaches the device at all. The disk and the auxiliary storage are opened read-only and must already exist (G2 creates
   neither); since FLAG_SHSHONLY never reaches a write, a hash of both is still taken before and after as a defensive, not a trusted, check -- any difference is a
   failure regardless of why. The signing ticket itself is never written to a file (hv_no_ticket_files, already the session's default) and never printed: this
   module's own log filter passes only a short list of known-harmless lines and withholds anything that looks like an ECID, a nonce or a ticket.

   A diagnostic build only: tools/vmrun-install-g2-build.sh compiles host/install.c and host/vmrun.c with -DHV_INSTALL_G2; nothing else sets it
   (tools/check-no-test-flags.sh). This is NOT a restore and does not continue past the TSS stage on its own. */
#include <stdint.h>
#include <stdio.h>

#include "virtio_usb.h"

struct idevicerestore_client_t;

/* host/install.c, G2 build, on the restore thread AFTER the session was begun (the session's begin empties the connection policy, so approving these hosts any
   earlier would be undone): approves the three real TSS hosts in net_guard and returns FLAG_SHSHONLY. */
int hv_g2_extra_flags(void);
/* host/install.c, G2 build: what the restore thread saw, recorded before it publishes the result. */
void hv_g2_record(int engine_rc, int closed, int cleanup_mask, int final_report);

/* Before the run: both storage files must already exist (G2 creates nothing), their identity is taken, the ECID is recorded for the log filter (never printed),
   and the log filter is installed. 0, or -1 with the reason printed. */
int hv_g2_begin(const char *disk, const char *aux, uint64_t ecid);
/* Waits (60 s) for the guest on the USB bus, then starts the restore through hv_install_start. 0, or -1. */
int hv_g2_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
/* Waits for the end of the run (cap_ms), asks it to stop once if it does not end, then waits 20 s more. 1: ended by itself, 0: needed the cancel, -1: did not
   end (then nothing may be freed or closed: the caller ends the process). */
int hv_g2_wait(int cap_ms);
/* The verdict table to `out`. Returns the number of FAILED rows (0 = G2 passed). The result line is one of g2_passed, g2_passed_metadata_only (the disk was
   compared by size and time only, see host/install_g0.h's content cap) or FAILED. */
int hv_g2_finish(FILE *out, const char *disk, const char *aux);

#endif
