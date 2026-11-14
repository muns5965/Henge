#ifndef HENGEVM_INSTALL_PLAN_H
#define HENGEVM_INSTALL_PLAN_H

#include <stddef.h>
#include <stdint.h>

/* G2-b: a MOCK orchestrator for an install, as data and a small executor. It has no connection to anything real: it includes no other project header, calls nothing but
   the callbacks it is given, and nothing outside tests/ uses it (tools/check-install-plan-isolation.sh keeps it that way). The callbacks are the plan's whole world, so a
   test can hand it a fake device, a synthetic server and a fake clock, and make any step fail, need retries, bounce the device or be cancelled.

   It is a graph of steps, not a fixed line: each step may be skipped by a condition, retried with a wait, followed by a wait for the device to come back (with its own
   bound and policy), allowed to fail without ending the plan (optional), or sent back to an earlier step (on_fail_goto). A plan-wide budget of step runs ends any loop. The
   order of events and the reason for the end are recorded; nothing is thrown away on failure. It does NOT decide whether anything real may run: there is no permission
   machinery here and no way to reach one. */

typedef enum { IP_DONE = 0, IP_AGAIN, IP_FAILED } IpResult;   /* what one run of a step says: done, transient (retry it), permanent */
typedef enum { IP_RECONNECT_RETRY_STEP = 0, IP_RECONNECT_FAIL } IpReconnectPolicy;
typedef enum { IP_PLAN_OK = 0, IP_PLAN_FAILED, IP_PLAN_CANCELLED, IP_PLAN_BUDGET, IP_PLAN_INVALID } IpVerdict;
typedef enum { IP_ST_PENDING = 0, IP_ST_DONE, IP_ST_SKIPPED, IP_ST_FAILED_OPTIONAL, IP_ST_FAILED, IP_ST_CANCELLED } IpStepState;

typedef struct IpEnv IpEnv;
struct IpEnv {
    void *user;
    volatile int *cancel;                                   /* may be NULL; looked at before every run, wait and reconnect */
    void (*wait)(IpEnv *env, int ms);                       /* required: sleep/advance the clock; should return early on cancel */
    int (*reconnect)(IpEnv *env, int step, int attempt);    /* required only by steps with await_return: 0 = the device is back, else not (yet) */
    void (*trace)(IpEnv *env, const char *line);            /* optional: every event, in order */
};

typedef struct {
    const char *name;
    IpResult (*run)(IpEnv *env, int step, int attempt);     /* required; attempt counts from 1 */
    int (*when)(IpEnv *env, int step);                      /* optional: 0 = skip this step */
    int max_attempts;                                       /* runs allowed for IP_AGAIN, 0 = 1 */
    int backoff_ms;                                         /* wait between attempts */
    int await_return;                                       /* after DONE, wait for the device to come back through env->reconnect */
    int max_reconnects;                                     /* tries of reconnect per DONE, 0 = 1 */
    IpReconnectPolicy on_reconnect_fail;                    /* retry the step (counts against max_attempts) or fail it */
    int optional;                                           /* a failure is recorded and the plan goes on */
    int on_fail_goto;                                       /* a failure (not optional) goes back to this step; -1 = end the plan */
} IpStep;

/* attempts and reconnects are reset to 0 each time this step is (re-)entered (ip_execute's
   "rc->attempts = 0; rc->reconnects = 0;"), so after on_fail_goto sends execution back to an
   earlier step, a later re-visit's counts overwrite the ones from an earlier visit: this is
   the LAST visit's counts, not a running total across every visit. The plan-wide total of
   every step run, across every step and every visit, is IpReport.runs instead. */
typedef struct { IpStepState state; int attempts, reconnects; } IpRecord;
typedef struct {
    IpVerdict verdict;
    int failed_step;       /* -1 when none */
    int runs;              /* step runs spent against the budget, summed over every step and every visit (unlike IpRecord.attempts/reconnects, which are per visit) */
    int steps_done;
    IpRecord rec[16];
    char note[160];
} IpReport;
#define IP_MAX_STEPS 16

/* Runs the plan from step 0. max_runs bounds the total step runs (including retries and goto loops); 0 = 4 x number of steps. */
IpVerdict ip_execute(const IpStep *steps, int n, IpEnv *env, int max_runs, IpReport *out);
const char *ip_verdict_name(IpVerdict v);
const char *ip_state_name(IpStepState s);

#endif
