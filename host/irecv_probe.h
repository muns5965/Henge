#ifndef HENGEVM_IRECV_PROBE_H
#define HENGEVM_IRECV_PROBE_H

#include <stdint.h>
#include <stdio.h>

#include "usb_host.h"

/* Opens the virtual Mac's USB device the way the restore tools do: through libirecovery's public API on top of host/libusb_shim.c. It sends no
   firmware, asks for no TSS signature and downloads nothing. It is not read-only: irecv_open_with_ecid() resets the bus when the shim lists the device
   and sends SET_CONFIGURATION(1) when the device reports configuration 0.
   Runs the stages (discover, open, device info, close) and prints a table saying for each one: ok / FAILED / not reached, with the evidence. Before and after,
   it reads GET_CONFIGURATION and the DFU GET_STATE directly. Returns the number of stages that failed (0 = all passed), or -1 if the guest never came on the bus. */
int hv_irecv_probe_run(HvVirtioUsb *usb, uint64_t ecid, FILE *out, int wait_ms);

/* The same, on a detached thread (vmrun --irecv-probe). */
void hv_irecv_probe_start(HvVirtioUsb *usb, uint64_t ecid);

#endif
