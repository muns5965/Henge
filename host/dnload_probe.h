#ifndef HENGEVM_DNLOAD_PROBE_H
#define HENGEVM_DNLOAD_PROBE_H

#include <stdint.h>
#include <stdio.h>

#include "usb_host.h"

/* Stage A0-A2 of the DFU download test, with raw control requests (hv_usb_control), not through libirecovery. It sends exactly these requests, once each,
   and then nothing more:
     setup  bus reset; GET_CONFIGURATION; SET_CONFIGURATION(1) only if the device reports 0 (what libirecovery did in the --irecv-probe run on the phone)
     A0     DFU GET_STATE                                   expected: 2 (dfuIDLE); anything else stops the test before any data is sent
     A1     DFU DNLOAD, wValue (block) 0, 16 bytes of 0x00  one request; the data is not a firmware image
     A2     DFU GET_STATE                                   the state right after A1 (GET_STATE only reads; GET_STATUS, which can advance the state, is not sent)
   Never sent: GET_STATUS, ABORT, CLR_STATUS, a zero-length DNLOAD, a second block, UPLOAD, DETACH, a reset after the setup. Any error, STALL, timeout or
   unexpected state stops the sequence at once; nothing is retried. The result says what the device answered; it does not say the ROM accepted the bytes as
   firmware. An OUT transfer's byte count is the request's wLength, not a count the device reported.
   Returns 0 when A0-A2 all got an answer (read the printed states), the number of the stage that stopped it (1 setup, 2 A0, 3 A1, 4 A2), or -1 if the guest
   never came on the bus. */
typedef struct {
    int stopped_at;      /* 0 = ran to the end */
    int requests;        /* control requests sent (the limit is HV_DNLOAD_MAX_REQUESTS) */
    int a0_state;        /* -1 = no answer */
    int a1_result;       /* bytes (= wLength) or a negative HV_USB_E* code; 0 = not sent */
    int a2_state;        /* -1 = no answer / not sent */
} HvDnloadResult;
#define HV_DNLOAD_MAX_REQUESTS 8

int hv_dnload_probe_run(HvVirtioUsb *usb, FILE *out, int wait_ms, HvDnloadResult *res);

/* The same, on a detached thread (vmrun --dnload-probe). */
void hv_dnload_probe_start(HvVirtioUsb *usb);

#endif
