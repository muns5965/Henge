#ifndef HENGEVM_RESTORE_TRANSITION_H
#define HENGEVM_RESTORE_TRANSITION_H

#include "usb_host.h"

/* The judgement of a device transition in the restore flow: the virtual Mac leaves the USB bus after an image (iBSS, iBEC, the kernelcache) and comes back as a NEW
   device. The restore tool waits for exactly this with its own timeouts; this module is the host's own check of the same thing, written so that each way it can go
   wrong has its own name and its own bound, and so that "the transfer function returned" is never taken for "the device came back".

   Observation (hv_transition_observe): the virtual port's session number, which the guest side bumps on every leave and every (re)connect. A leave is seen when the
   session differs from the one at the start or the guest is off the bus; the arrival when the guest is on the bus again. A leave and a return that both happen
   between two looks still show as both (the session differs while the port is on). Looking is cheap and does not touch the device.

   The rest (open a new client, check its mode, check that the OLD client no longer works) is done by the caller with the library it uses and filled into the
   observation; hv_transition_verdict then names the first thing that is wrong. */

typedef struct {
    int leave_ms;        /* bound for the device to leave after the transfer */
    int arrive_ms;       /* bound for it to be back, counted from the leave */
    int mode_lo, mode_hi; /* the USB product ids accepted for the new device */
} HvTrSpec;

typedef struct {
    int left, left_after_ms;         /* a leave was observed, this long after the start */
    int arrived, arrived_after_ms;   /* the device was back on the bus, this long after the leave */
    int fast;                        /* both seen in the same look */
    int opened;                      /* the caller opened a new client on the new device */
    int mode;                        /* its product id (valid when opened) */
    int old_io_failed;               /* an operation on the OLD client failed (it must) instead of working or hanging */
    int old_io_hung;                 /* an operation on the OLD client did not return in time */
} HvTrObs;

enum { HV_TR_OK = 0, HV_TR_NO_LEAVE, HV_TR_NO_ARRIVE, HV_TR_OPEN_FAILED, HV_TR_BAD_MODE, HV_TR_OLD_HANDLE_USABLE, HV_TR_OLD_HANDLE_HUNG };

/* Fills the first part of o (left/arrived/fast). */
void hv_transition_observe(HvVirtioUsb *usb, const HvTrSpec *spec, HvTrObs *o);
/* Pure. Returns HV_TR_OK or the first failure, with a sentence in note. */
int hv_transition_verdict(const HvTrObs *o, const HvTrSpec *spec, char *note, int cap);
const char *hv_transition_name(int r);

#endif
