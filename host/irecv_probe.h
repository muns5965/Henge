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

/* One device event as the probe records it. */
typedef struct { int type; int mode; int have_ecid; uint64_t ecid; int ms; } HvIrecvEv;
#define HV_IREC_EV_ADD 1
#define HV_IREC_EV_REMOVE 2
/* Verdict on a recorded event sequence, by this contract of the virtual bus (it is NOT a rule of USB in general): the first event is an ADD, ADD and REMOVE
   strictly alternate, the last one is an ADD (the device is present at the end), every ADD carries `ecid` and a DFU-family mode, and no event was dropped.
   Returns 1 if the sequence meets it; otherwise 0, with the first violation in note. */
int hv_irecv_events_verdict(const HvIrecvEv *ev, int n, int dropped, uint64_t ecid, char *note, int cap);

/* The same, on a detached thread (vmrun --irecv-probe). */
void hv_irecv_probe_start(HvVirtioUsb *usb, uint64_t ecid);

#endif
