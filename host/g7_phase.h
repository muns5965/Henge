#ifndef HENGEVM_G7_PHASE_H
#define HENGEVM_G7_PHASE_H
/* Where the time of a G7 run goes: timing lines for the app's progress banner and for the next log. Header-only (tests/test_g7_phase.c compiles the same code).
   Lines (all start-of-line, one write each; numbers only, nothing from the guest or the network):
     g7-phase: name=<n> secs=<s>                       a start-up phase that has ended (e.g. ipsw-sha256 with MB/s when a size is given)
     g7-step: step=<k> name=<N> pct=<p> t=<s> [prev=<N> prev_secs=<s>]   the restore tool's own step / step-percent report (RESTORE_STEP_*); pct is the percent of THAT step only, there is no overall percent
     g7-steptimes: <N>=<s> ...                          seconds spent in each step seen, printed once when the restore ends
   t is seconds since g7ph_reset(). Printing is throttled: a step change always; the same step only when pct moved >= 5 points or 30 s passed. A clock and an output can be injected for tests. */
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define G7PH_STEPS 8
static const char *const g7ph_step_names[G7PH_STEPS] = {"DETECT", "PREPARE", "UPLOAD_FS", "VERIFY_FS", "FLASH_FW", "FLASH_BB", "FUD", "UPLOAD_IMG"};

typedef struct {
    int64_t t0_ms, step_t0_ms[G7PH_STEPS], last_print_ms; /* t0 = reset; per-step first-seen time (-1: not seen) */
    int cur_step, last_pct, last_print_pct;
    FILE *out;                                      /* NULL = stdout */
    int64_t (*clock_ms)(void);                      /* NULL = CLOCK_MONOTONIC */
} G7Ph;

static inline int64_t g7ph_mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static inline int64_t g7ph_now(const G7Ph *p) { return p->clock_ms ? p->clock_ms() : g7ph_mono_ms(); }
static inline const char *g7ph_name(int step) { return step >= 0 && step < G7PH_STEPS ? g7ph_step_names[step] : "?"; }
static inline void g7ph_reset(G7Ph *p) {
    for (int i = 0; i < G7PH_STEPS; i++) p->step_t0_ms[i] = -1;
    p->t0_ms = g7ph_now(p); p->cur_step = -1; p->last_pct = p->last_print_pct = -1; p->last_print_ms = p->t0_ms;
}
static inline void g7ph_emit(const G7Ph *p, const char *line) { FILE *o = p->out ? p->out : stdout; fputs(line, o); fflush(o); }

/* The restore tool's progress callback. Returns 1 when a line was printed. */
static inline int g7ph_step(G7Ph *p, int step, double progress) {
    if (step < 0 || step >= G7PH_STEPS) return 0;
    int64_t now = g7ph_now(p);
    int pct = progress < 0 ? -1 : progress > 1 ? 100 : (int)(progress * 100.0 + 0.5);
    int change = step != p->cur_step;
    char line[256]; int n;
    if (change) {
        if (p->step_t0_ms[step] < 0) p->step_t0_ms[step] = now;
        if (p->cur_step >= 0) n = snprintf(line, sizeof line, "g7-step: step=%d name=%s pct=%d t=%.1f prev=%s prev_secs=%.1f\n", step, g7ph_name(step), pct, (now - p->t0_ms) / 1000.0, g7ph_name(p->cur_step), (now - p->step_t0_ms[p->cur_step]) / 1000.0);
        else n = snprintf(line, sizeof line, "g7-step: step=%d name=%s pct=%d t=%.1f\n", step, g7ph_name(step), pct, (now - p->t0_ms) / 1000.0);
        p->cur_step = step;
    } else {
        if (pct == p->last_print_pct) { p->last_pct = pct; return 0; }
        if (!(pct >= p->last_print_pct + 5 || pct < p->last_print_pct || now - p->last_print_ms >= 30000)) { p->last_pct = pct; return 0; }
        n = snprintf(line, sizeof line, "g7-step: step=%d name=%s pct=%d t=%.1f\n", step, g7ph_name(step), pct, (now - p->t0_ms) / 1000.0);
    }
    (void)n; p->last_pct = p->last_print_pct = pct; p->last_print_ms = now;
    g7ph_emit(p, line);
    return 1;
}

/* Seconds in each step seen (a step lasts until the next one's first report; the last one until `now`). */
static inline void g7ph_times(const G7Ph *p, char *buf, size_t cap) {
    size_t at = 0; int64_t now = g7ph_now(p); int first = 1;
    if (cap) buf[0] = 0;
    for (int i = 0; i < G7PH_STEPS && at + 1 < cap; i++) {
        if (p->step_t0_ms[i] < 0) continue;
        int64_t end = now;
        for (int j = 0; j < G7PH_STEPS; j++) if (p->step_t0_ms[j] > p->step_t0_ms[i] && p->step_t0_ms[j] < end) end = p->step_t0_ms[j];
        int w = snprintf(buf + at, cap - at, "%s%s=%.1f", first ? "" : " ", g7ph_name(i), (end - p->step_t0_ms[i]) / 1000.0);
        if (w < 0 || (size_t)w >= cap - at) break;
        at += (size_t)w; first = 0;
    }
}

static inline void g7ph_phase_line(const G7Ph *p, const char *name, int64_t start_ms, int64_t bytes) {
    char line[200]; double secs = (g7ph_now(p) - start_ms) / 1000.0;
    if (bytes > 0 && secs > 0.001) snprintf(line, sizeof line, "g7-phase: name=%s secs=%.1f bytes=%lld mb_per_s=%.1f\n", name, secs, (long long)bytes, bytes / 1048576.0 / secs);
    else snprintf(line, sizeof line, "g7-phase: name=%s secs=%.1f\n", name, secs);
    g7ph_emit(p, line);
}
#endif
