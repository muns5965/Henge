#include "install_plan.h"

#include <stdio.h>
#include <string.h>

const char *ip_verdict_name(IpVerdict v) {
    static const char *n[] = {"ok", "failed", "cancelled", "budget", "invalid"};
    return (unsigned)v < 5 ? n[v] : "?";
}
const char *ip_state_name(IpStepState s) {
    static const char *n[] = {"pending", "done", "skipped", "failed_optional", "failed", "cancelled"};
    return (unsigned)s < 6 ? n[s] : "?";
}

static void tr(IpEnv *e, const char *fmt, const char *name, int a, int b) {
    char line[160];
    if (!e->trace) return;
    snprintf(line, sizeof line, fmt, name, a, b);
    e->trace(e, line);
}
static int cancelled(IpEnv *e) { return e->cancel && __atomic_load_n(e->cancel, __ATOMIC_ACQUIRE); }

IpVerdict ip_execute(const IpStep *s, int n, IpEnv *e, int max_runs, IpReport *r) {
    memset(r, 0, sizeof *r);
    r->failed_step = -1;
    if (!s || !e || !e->wait || n <= 0 || n > IP_MAX_STEPS) { snprintf(r->note, sizeof r->note, "invalid plan or environment"); return r->verdict = IP_PLAN_INVALID; }
    for (int i = 0; i < n; i++) {
        if (!s[i].run || (s[i].await_return && !e->reconnect) || s[i].on_fail_goto >= n || s[i].on_fail_goto < -1) {
            snprintf(r->note, sizeof r->note, "step %d is not well formed", i);
            return r->verdict = IP_PLAN_INVALID;
        }
    }
    int budget = max_runs > 0 ? max_runs : 4 * n;
    int i = 0;
    while (i < n) {
        const IpStep *st = &s[i];
        IpRecord *rc = &r->rec[i];
        if (cancelled(e)) goto cancel;
        if (st->when && !st->when(e, i)) { rc->state = IP_ST_SKIPPED; tr(e, "%s: skipped", st->name, 0, 0); i++; continue; }
        int max_a = st->max_attempts > 0 ? st->max_attempts : 1, final = 0, failed = 0;
        rc->attempts = 0; rc->reconnects = 0;
        while (!final) {
            if (cancelled(e)) goto cancel;
            if (r->runs >= budget) { r->failed_step = i; snprintf(r->note, sizeof r->note, "step %s: the plan's run budget (%d) is spent", st->name, budget); tr(e, "%s: budget", st->name, 0, 0); return r->verdict = IP_PLAN_BUDGET; }
            r->runs++;
            rc->attempts++;
            tr(e, "%s: run %d", st->name, rc->attempts, 0);
            IpResult res = st->run(e, i, rc->attempts);
            if (cancelled(e)) goto cancel;
            if (res == IP_DONE) {
                if (!st->await_return) { final = 1; break; }
                int max_rc = st->max_reconnects > 0 ? st->max_reconnects : 1, back = 0;
                for (int k = 1; k <= max_rc && !back; k++) {
                    if (cancelled(e)) goto cancel;
                    rc->reconnects++;
                    back = e->reconnect(e, i, k) == 0;
                    tr(e, "%s: reconnect try %d %s", st->name, k, 0);
                    if (!back && k < max_rc) e->wait(e, st->backoff_ms);
                }
                if (cancelled(e)) goto cancel;
                if (back) { final = 1; break; }
                if (st->on_reconnect_fail == IP_RECONNECT_FAIL) { failed = 1; final = 1; snprintf(r->note, sizeof r->note, "step %s: the device did not come back", st->name); break; }
                res = IP_AGAIN;   /* retry the step */
            }
            if (res != IP_AGAIN) {   /* IP_FAILED, or a value the enum does not have: a callback bug is a failure, never a retry */
                if (res != IP_FAILED) tr(e, "%s: invalid result from run, treated as failed", st->name, 0, 0);
                 failed = 1; final = 1; snprintf(r->note, sizeof r->note, res == IP_FAILED ? "step %s: failed" : "step %s: run returned an invalid result", st->name); break; }
            /* IP_AGAIN */
            if (rc->attempts >= max_a) { failed = 1; final = 1; snprintf(r->note, sizeof r->note, "step %s: still not done after %d run(s)", st->name, rc->attempts); break; }
            e->wait(e, st->backoff_ms);
        }
        if (!failed) { rc->state = IP_ST_DONE; r->steps_done++; tr(e, "%s: done", st->name, 0, 0); i++; continue; }
        if (st->optional) { rc->state = IP_ST_FAILED_OPTIONAL; tr(e, "%s: failed, optional", st->name, 0, 0); r->note[0] = 0; i++; continue; }
        rc->state = IP_ST_FAILED;
        tr(e, "%s: failed", st->name, 0, 0);
        if (st->on_fail_goto >= 0) { tr(e, "%s: back to step %d", st->name, st->on_fail_goto, 0); r->note[0] = 0; i = st->on_fail_goto; continue; }
        r->failed_step = i;
        return r->verdict = IP_PLAN_FAILED;
    }
    return r->verdict = IP_PLAN_OK;
cancel:
    r->failed_step = i;
    r->rec[i].state = IP_ST_CANCELLED;
    snprintf(r->note, sizeof r->note, "cancelled at step %s (not a finding about the plan)", s[i].name);
    tr(e, "%s: cancelled", s[i].name, 0, 0);
    return r->verdict = IP_PLAN_CANCELLED;
}
