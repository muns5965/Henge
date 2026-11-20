#ifndef HENGEVM_INSTALL_G6_H
#define HENGEVM_INSTALL_G6_H
/* G6: the Android restore entry point (host/install.c) run through the REAL restore tool's own start function, one stage past G5: this build also runs
   the restore-options plist construction and resolves the Reverse Proxy attempt (this project's builds always define HAVE_REVERSE_PROXY --
   third_party/restore_config/config.h -- so that is the branch that actually compiles and runs here, never the FDR-listener-thread branch), then stops
   at the vendored tool's own "stop E" (third_party/idevicerestore/src/hv_stop.h), strictly BEFORE restored_start_restore() (the actual ASR/FDR
   disk-writing restore) is ever called. A test build only: tools/vmrun-install-g6-build.sh compiles host/install.c and host/vmrun.c with
   -DHV_INSTALL_G6 -DHV_STOP_AFTER_RESTORE_OPTIONS; nothing shipped sets either. Full design: docs/g6-stop-e-design-2026-11-20.md.

   G6 goes one stage past G5: it runs G5's whole TSS + DFU->Recovery + RecoveryOS-entry + restored-handshake stage first (reusing the same code paths,
   now proven by G5 through a real device), then continues past stop D into the Reverse Proxy attempt and the options-plist construction and stops only
   at stop E. No new network host and no new signature request: the Reverse Proxy attempt talks over the already-open restored/usbmuxd link to the SAME
   device, and the options plist is built in memory from data already fetched in stage 1. Disk and auxiliary storage stay read-only and are
   hash-verified unchanged before/after, exactly as in G0-G5, because reaching stop E never calls restored_start_restore() -- the host never sends the
   actual restore-start command, so storage writes continue to be refused exactly as before this point.

   Required pass conditions for G6 (deliberately narrower than "reached stop E"): everything G5 already requires (restored handshake, HardwareInfo
   query); restored_start_restore() must never have been called (0 call count, implied structurally by stop E's return point); the options plist's fixed
   required fields must be present with the right type (opts_required_fields_ok == 1 -- see host/install_g6.c and docs/g6-stop-e-design-2026-11-20.md
   §8); the engine's return code must be the NORMAL stop-E code, never the cleanup-uncertain one (HV_STOP_E_CLEANUP_UNCERTAIN_RC is unconditionally a
   failure for G6, regardless of anything else the table shows -- the Reverse Proxy's own cleanup left client->restore deliberately unfreed because its
   join could not be confirmed, which is never a pass); disk and aux content must be unchanged (hash-verified, same as G0-G5). The Reverse Proxy
   connection outcome (rproxy_status, from rp_started -- NOT from the pointer being non-NULL, which the source does not guarantee implies success) is
   reported as a REQUIRED row, the same way G5 treats hwinfo_query as required even though restore_device() itself tolerates its failure without
   aborting: this is a controlled, repeatable virtual-Mac target, not an unpredictable real device, so a Reverse Proxy failure here is treated as a
   regression to catch, not an expected outcome to tolerate. USB bulk/interrupt transfer failure counts are kept purely OBSERVATIONAL/informational, same
   reasoning as G5 (host/install_g5.h): this build opens still more connections before any teardown, and this project has not yet classified this
   traffic shape request by request. */
#include <stdint.h>
#include <stdio.h>

#include "virtio_usb.h"

/* host/install.c, G6 build: the flag that stops the tool right after the Reverse Proxy attempt is resolved and the restore-options plist is built (and
   installs the hook that reports it). */
int hv_g6_extra_flags(void);
/* host/install.c, G6 build: what the restore thread saw, recorded before it publishes the result. */
void hv_g6_record(int engine_rc, int closed, int cleanup_mask, int final_report);

/* The checks before the run: both storage files must already exist (G6 creates nothing), then their identity is taken. Also makes the tool's log print
   without the ECID/nonce/ticket pattern. 0, or -1 with the reason printed. */
int hv_g6_begin(const char *disk, const char *aux, uint64_t ecid);
/* Waits (60 s) for the guest on the USB bus, then starts the restore through hv_install_start. 0, or -1. */
int hv_g6_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
/* Waits for the end of the run (cap_ms), asks it to stop once if it does not end, and waits 20 s more. 1: it ended by itself, 0: it needed the cancel, -1: it
   did not end at all (then nothing may be freed or closed: the caller ends the process). */
int hv_g6_wait(int cap_ms);
/* The verdict table to `out`. Returns the number of FAILED rows (0 = G6 passed). The result line is one of g6_passed (disk and aux content verified),
   g6_passed_metadata_only (the disk was compared by size and time only) or FAILED. */
int hv_g6_finish(FILE *out, const char *disk, const char *aux);

#endif
