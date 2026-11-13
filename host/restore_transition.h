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
    int check_ms;        /* bound for the caller's check (open the new client, look at the old one); 0 = HV_TR_CHECK_DEFAULT_MS */
} HvTrSpec;
#define HV_TR_CHECK_DEFAULT_MS 5000

typedef struct {
    int left, left_after_ms;         /* a leave was observed, this long after the start */
    int arrived, arrived_after_ms;   /* the device was back on the bus, this long after the leave */
    int fast;                        /* both seen in the same look */
    int opened;                      /* the caller opened a new client on the new device */
    int mode;                        /* its product id (valid when opened) */
    int old_io_failed;               /* an operation on the OLD client failed (it must) instead of working or hanging */
    int old_io_hung;                 /* an operation on the OLD client did not return in time */
    int check_took_ms;               /* how long the caller's check ran (hv_transition_run measures it) */
    int cancelled;                   /* the wait was ended by the caller's cancel flag (not by a bound): nothing else in this observation is a finding */
} HvTrObs;

enum { HV_TR_OK = 0, HV_TR_NO_LEAVE, HV_TR_NO_ARRIVE, HV_TR_OPEN_FAILED, HV_TR_BAD_MODE, HV_TR_OLD_HANDLE_USABLE, HV_TR_OLD_HANDLE_HUNG, HV_TR_CANCELLED, HV_TR_BEGIN_FAILED, HV_TR_CHECK_TIMEOUT };

/* Fills the first part of o (left/arrived/fast). */
void hv_transition_observe(HvVirtioUsb *usb, const HvTrSpec *spec, HvTrObs *o);
/* The same, ended early (o->cancelled = 1, within a few ms) when *cancel turns non-zero. cancel may be NULL. A cancel is not a failure of the device and is never
   reported as NO_LEAVE / NO_ARRIVE. */
void hv_transition_observe_c(HvVirtioUsb *usb, const HvTrSpec *spec, volatile int *cancel, HvTrObs *o);

/* The same, against a session number the CALLER took before it started the transfer. Looking at the session only after the transfer was started can miss a leave and a
   return that both happened inside the start (the changed number would be taken for the starting one). */
void hv_transition_observe_b(HvVirtioUsb *usb, const HvTrSpec *spec, volatile int *cancel, uint32_t base_session, HvTrObs *o);

/* A run of consecutive transitions (iBSS, iBEC, the restore ramdisk ... each leaves the bus and comes back as a new device). The transfers themselves and the new
   client are the caller's: begin() starts stage i's transfer (0 = started), check() runs after the device is back and fills opened / mode / old_io_*. The run stops
   at the first stage that is not OK, with its number; later stages are NOT started. *cancel is looked at before each stage and during each wait. */
/* What a check is bound by. It must stop by deadline_ms from its start and when *cancel turns non-zero (cancel may be NULL): the run cannot interrupt it. If it
   overruns, hv_transition_run reports HV_TR_CHECK_TIMEOUT (what it filled in is not trusted); if the cancel flag is set when it returns, the stage is CANCELLED. */
typedef struct { volatile int *cancel; int deadline_ms; } HvTrCheckCtx;
typedef struct {
    int (*begin)(int stage, void *user);                                           /* the session number is taken BEFORE this is called */
    void (*check)(int stage, HvTrObs *o, const HvTrCheckCtx *ctx, void *user);
} HvTrSteps;
typedef struct { int stages_done; int failed_stage; /* -1 when all passed */ int verdict; } HvTrRun;
/* Returns the verdict of the run (HV_TR_OK when every stage passed), the sentence for the failing stage in note. */
int hv_transition_run(HvVirtioUsb *usb, const HvTrSpec *specs, int n, volatile int *cancel, const HvTrSteps *steps, void *user, HvTrRun *out, char *note, int cap);
/* Pure. Returns HV_TR_OK or the first failure, with a sentence in note. */
int hv_transition_verdict(const HvTrObs *o, const HvTrSpec *spec, char *note, int cap);
const char *hv_transition_name(int r);

#endif
