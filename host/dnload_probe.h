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
   Returns 0 when A0-A2 (and A3 if asked) all got an answer (read the printed states), the number of the stage that stopped it (1 setup, 2 A0, 3 A1, 4 A2, 5 A3), or -1 if the guest
   never came on the bus. */
typedef struct {
    int stopped_at;      /* 0 = ran to the end */
    int requests;        /* control requests sent (the limit is HV_DNLOAD_MAX_REQUESTS) */
    int a0_state;        /* -1 = no answer */
    int a1_result;       /* bytes (= wLength) or a negative HV_USB_E* code; 0 = not sent */
    int a2_state;        /* -1 = no answer / not sent */
    /* stage A3 (only with with_status): ONE DFU GET_STATUS after A2, sent only when A0 = 2, A1 answered and A2 = 5 (the same conditions as the run that was seen
       on the phone), and never repeated. */
    int a3_sent;         /* 1 once the request went out */
    int a3_len;          /* bytes the transfer returned (6 expected), or a negative HV_USB_E* code */
    uint8_t a3_raw[8];   /* the answer as received (first a3_len bytes, at most 8) */
    int a3_parsed;       /* 1 when a3_len was exactly 6 and the fields below are valid */
    int a3_status, a3_poll_ms, a3_state, a3_istring;
} HvDnloadResult;
#define HV_DNLOAD_MAX_REQUESTS 8

/* With with_status = 1 the sequence continues with A3 (see HvDnloadResult) and nothing after it: the second GET_STATUS, ABORT, CLR_STATUS, a zero-length
   DNLOAD, a second block are never sent, whatever GET_STATUS answers. Stopped-at code 5 = A3 not sent (A2 was not 5) or failed. The request limit
   is still HV_DNLOAD_MAX_REQUESTS (A0-A3 use 7). */
int hv_dnload_probe_run(HvVirtioUsb *usb, FILE *out, int wait_ms, int with_status, HvDnloadResult *res);

/* The same, on a detached thread (vmrun --dnload-probe, or --dnload-probe-a3 with with_status = 1). */
void hv_dnload_probe_start(HvVirtioUsb *usb, int with_status);

#endif
