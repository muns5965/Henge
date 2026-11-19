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
   firmware. An OUT transfer's byte count is the request's wLength, not a count the device reported. The one exception is HV_DNLOAD_A6 (see below): if A4/A5
   ends in a session change with the guest back online, it adds exactly one read-only re-discovery (A6) on the new session; nothing else about this
   contract changes.
   Returns 0 when A0-A2 (and A3 if asked) all got an answer (read the printed states), the number of the stage that stopped it (1 setup, 2 A0, 3 A1, 4 A2, 5 A3, 6 A4/A5), or -1 if the guest
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
    /* stage A4/A5 (only with HV_DNLOAD_A4): ONE DFU ABORT, then ONE GET_STATE. ABORT is sent only when A3 was answered with exactly bStatus 0 and bState 5. */
    int a4_sent;         /* 1 once the ABORT went out */
    int a4_result;       /* the transfer result of the ABORT (0 = answered, negative HV_USB_E* code) */
    int a5_state;        /* the GET_STATE after the ABORT; -1 = no answer / not sent */
    /* A session change: the guest left the bus and came back (the session number moved on) or went away, noticed either while a request was in flight or
       just before one would have gone out. The host only learns THAT it happened between two points, never exactly when or why, so this records the bounds:
       the last request that got an answer and the request that was in flight (or stopped). Nothing is sent to the new session (no descriptor read, no
       GET_STATE, no reset): the sequence ends. */
    int gone;                 /* 1 when the sequence ended because of a session change */
    int gone_request;         /* 1-based number of the request in flight, or the one that would have been next */
    int gone_sent;            /* 1 = that request had gone out (its answer was lost); 0 = the check stopped it before it was sent */
    int last_answered;        /* 1-based number of the last request the device answered (STALL counts as an answer); 0 = none */
    int64_t session_before;   /* the session the sequence started in */
    int64_t session_after;    /* the session read right after the change was noticed; -1 = the guest was not on the bus */
    /* stage A6 (only with HV_DNLOAD_A6, and only when A4/A5 ended in a session change with the guest back online -- gone=1 and session_after>=0): ONE
       read-only re-discovery on the NEW session -- GET_CONFIGURATION, SET_CONFIGURATION(1) only if needed, GET_CONFIGURATION recheck, then ONE DFU
       GET_STATE. Nothing else is sent: no ABORT, no DNLOAD, no retry, no second GET_STATE. This does not touch gone/session_before/session_after/
       last_answered above, which still describe the original A4/A5 session change; a second session change seen while re-discovering is recorded
       separately here (a6_gone_again) and is not folded into those fields. */
    int a6_attempted;    /* 1 = re-discovery was attempted on the new session */
    int a6_setup_ok;     /* 1 = GET_CONFIGURATION ended up reporting configuration 1 */
    int a6_state;        /* the GET_STATE answer on the new session; -1 = no answer / not reached */
    int a6_gone_again;   /* 1 = the session changed yet again while re-discovering; a6_state stays -1 */
} HvDnloadResult;
#define HV_DNLOAD_MAX_REQUESTS 8
#define HV_DNLOAD_MAX_REQUESTS_A4 9 /* A0-A3 use 7, ABORT and the GET_STATE after it make 9; only the A4 mode has this limit */
/* how far the sequence goes (the with_status argument of hv_dnload_probe_run) */
#define HV_DNLOAD_A2 0 /* A0-A2 */
#define HV_DNLOAD_A3 1 /* + one GET_STATUS */
#define HV_DNLOAD_A4 2 /* + one ABORT and one GET_STATE (A5) */
#define HV_DNLOAD_A6 3 /* + if A4/A5 saw a session change and the guest is back online: one read-only re-discovery on the new session (A6, see HvDnloadResult) */

/* With with_status = 1 the sequence continues with A3 (see HvDnloadResult) and nothing after it: the second GET_STATUS, ABORT, CLR_STATUS, a zero-length
   DNLOAD, a second block are never sent, whatever GET_STATUS answers. Stopped-at code 5 = A3 not sent (A2 was not 5) or failed. The request limit
   is still HV_DNLOAD_MAX_REQUESTS (A0-A3 use 7). */
int hv_dnload_probe_run(HvVirtioUsb *usb, FILE *out, int wait_ms, int with_status, HvDnloadResult *res);

/* The same, on a detached thread (vmrun --dnload-probe, --dnload-probe-a3 = HV_DNLOAD_A3, --dnload-probe-a4 = HV_DNLOAD_A4, --dnload-probe-a6 = HV_DNLOAD_A6).
   With HV_DNLOAD_A4 the request limit is HV_DNLOAD_MAX_REQUESTS_A4; ABORT is never sent in the other modes, and nothing follows the GET_STATE after it: no
   CLR_STATUS, no retry. HV_DNLOAD_A6 runs the identical A0-A5 sequence and, only if that ends in a session change with the guest back online, adds the
   one read-only A6 re-discovery described in HvDnloadResult; it changes nothing about A0-A5 itself. */
void hv_dnload_probe_start(HvVirtioUsb *usb, int with_status);

#endif
