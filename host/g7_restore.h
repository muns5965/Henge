#ifndef HENGEVM_G7_RESTORE_H
#define HENGEVM_G7_RESTORE_H
/* The G7 restore adapter and the judgment of its result. Two parts, neither of which starts a restore by itself:

   1. hv_g7_restore_run drives ONE restore through a table of operations (HvG7RestoreOps): start, poll, cancel, sleep. The table is the only thing that touches the restore tool.
      The real table (host/g7_restore_real.c: hv_install_start / hv_install_result / hv_install_cancel) exists only in the G7 restore build (tools/g7-runner-build.sh real) and in
      no shipped build; the tests use a fake table. The outcome says WHAT ARRIVED (a signal), never that the installation worked.
   2. hv_g7_conclude is the only place that may call hv_g7_pair_finish: only when the outcome is SUCCESS_SIGNAL AND the files show a restore happened (see below).
      Every other outcome, and a SUCCESS_SIGNAL whose files do not look right, leaves the pair in_progress: nothing is deleted, moved or reset, and the next run is blocked by the
      preflight until someone quarantines it on purpose.

   What the judgment can and cannot tell: the signal is the restore tool's own report; the file checks (allocated disk size, the GPT header, the AUX header) are evidence that a
   restore wrote to the disk, not proof that macOS boots. They are NOT verified against a real restore yet (none has run): if they are too strict for a real result the pair stays
   in_progress, which is the safe direction. A run that boots the installed disk is the only proof of an installation.
   Nothing here prints an ECID. */
#include <stdint.h>
#include <stddef.h>

typedef struct { const char *ipsw, *workdir; uint64_t ecid; } HvG7RestoreReq;

typedef struct {
    int (*start)(const HvG7RestoreReq *req, void *ctx);   /* 0 started, else not started */
    int (*poll)(void *ctx);                               /* 1 running, 0 the restore reported success, -1 it reported failure; anything else is treated as a failure */
    void (*cancel)(void *ctx);                            /* ask it to stop; returns at once; may be called more than once */
    void (*sleep_ms)(void *ctx, int ms);
    void *ctx;
} HvG7RestoreOps;

typedef enum {
    HV_G7R_NOT_STARTED = 0,   /* start() refused: nothing ran */
    HV_G7R_SUCCESS_SIGNAL,    /* the tool said it succeeded (necessary, not sufficient) */
    HV_G7R_FAILED,            /* the tool said it failed (or answered something that is not a result) */
    HV_G7R_CANCELLED,         /* a stop was asked for and the tool ended with a failure/stop result */
    HV_G7R_TIMEOUT,           /* the time ran out, a cancel was sent, and the tool then ended */
    HV_G7R_NO_SIGNAL          /* nothing arrived even after the cancel and the grace time: the run did not end; the caller must end the process and free nothing */
} HvG7RestoreOutcome;

typedef struct { int timeout_ms, grace_ms, step_ms; } HvG7RestoreLimits;
#define HV_G7R_DEFAULT_LIMITS {.timeout_ms = 2 * 3600 * 1000, .grace_ms = 60 * 1000, .step_ms = 200}   /* provisional: no real restore has been timed */

/* Asks the running hv_g7_restore_run to cancel (async-signal-safe, from any thread). */
void hv_g7_restore_request_stop(void);
HvG7RestoreOutcome hv_g7_restore_run(const HvG7RestoreReq *req, const HvG7RestoreOps *ops, const HvG7RestoreLimits *lim);
const char *hv_g7_restore_outcome_name(HvG7RestoreOutcome o);

/* The state of the files after a restore. disk/aux: the pair in `dir`. */
typedef struct { int disk_written, gpt_header, aux_header, aux_size_ok; unsigned long long disk_allocated; } HvG7Evidence;
#ifndef HV_G7_MIN_WRITTEN_BYTES
#define HV_G7_MIN_WRITTEN_BYTES (1ULL << 30)   /* ARBITRARY sanity floor for "a restore wrote something"; a macOS install writes far more. Not a measured value. */
#endif
int hv_g7_collect_evidence(const char *dir, HvG7Evidence *ev);   /* read-only; 0, or -1 when the files cannot be looked at */

typedef enum { HV_G7_CONCLUDED_DONE = 0, HV_G7_CONCLUDED_KEPT } HvG7Conclusion;
/* Decides and, only for DONE, calls hv_g7_pair_finish(dir). `note` says why in words (no ECID). Never deletes, moves or resets anything. */
HvG7Conclusion hv_g7_conclude(const char *dir, HvG7RestoreOutcome outcome, char *note, int cap);

#endif
