#ifndef HENGEVM_RESTORE_PREFLIGHT_H
#define HENGEVM_RESTORE_PREFLIGHT_H

#include <stdint.h>
#include <stdio.h>

#include "usb_host.h"

/* Build 69-a. Starts the REAL restore tool (idevicerestore_start, with libirecovery, the private usbmuxd and the libusb stand-in) against the virtual Mac's USB port,
   with FLAG_ERASE | FLAG_NOACTION, which is an existing stop of the tool: right after it has detected the device and opened the IPSW it returns 0, before it reads the
   BuildManifest. Nothing in the vendored sources is changed.
   What it does NOT do: ask Apple's signing server (the curl stand-in is in deny mode: every request is refused before any lookup, connection or TLS), send any image to the
   device, read the root filesystem out of the IPSW, or write a disk. FLAG_LATEST (the one network path before the signing step) is not set.
   Result words: "preflight_completed" and "stopped=noaction". It is never a restore result.
   cap_ms bounds the whole run. Returns 0 when every row of the table passed, otherwise the number of rows that failed; -1 if the guest never came on the bus. */
int hv_restore_preflight_run(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, FILE *out, int cap_ms);

/* The same on a detached thread (vmrun --restore-preflight); hv_restore_preflight_finished() is 1 when the thread is done. */
void hv_restore_preflight_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid);
int hv_restore_preflight_finished(void);

/* Pure helper, tested alone: the verdict on the progress the tool reported. last_step/last_p: the last callback; max_step: the highest step seen; calls: how many callbacks.
   Returns 1 when it is what a stop at FLAG_NOACTION looks like (last callback is DETECT 0.6 and no later step ever came), else 0 with the reason in note. */
int hv_restore_preflight_progress_verdict(int calls, int max_step, int last_step, double last_p, char *note, int cap);

#endif
