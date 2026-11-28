/* host/g7_phase.h with an injected clock and a temporary output: the progress/timing lines of a G7 run. No VM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../host/g7_phase.h"
static int64_t T; static int64_t clk(void) { return T; }
static int bad;
#define EXPECT(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)
static char *slurp(FILE *f) { long n; fflush(f); n = ftell(f); rewind(f); char *b = calloc(1, (size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) {} return b; }
static int count(const char *h, const char *n) { int c = 0; for (const char *p = h; (p = strstr(p, n)); p++) c++; return c; }
int main(void) {
    FILE *o = tmpfile(); G7Ph p; memset(&p, 0, sizeof p); p.out = o; p.clock_ms = clk; T = 100000; g7ph_reset(&p);
    T += 2000; EXPECT(g7ph_step(&p, 1, 0.0) == 1, "first report prints (step change)");
    T += 1000; EXPECT(g7ph_step(&p, 1, 0.02) == 0, "same step, +2 points, 1 s: throttled");
    T += 1000; EXPECT(g7ph_step(&p, 1, 0.06) == 1, "same step, +6 points: printed");
    T += 5000; EXPECT(g7ph_step(&p, 2, 0.0) == 1, "step change prints and names the previous step with its seconds");
    for (int i = 1; i <= 100; i++) { T += 4000; g7ph_step(&p, 2, i / 100.0); }
    T += 1000; g7ph_step(&p, 3, 0.5);
    EXPECT(g7ph_step(&p, 9, 0.5) == 0 && g7ph_step(&p, -1, 0.5) == 0, "steps outside 0..7 are ignored");
    T += 20000; char tm[300]; g7ph_times(&p, tm, sizeof tm);
    char *t = slurp(o);
    EXPECT(strstr(t, "g7-step: step=1 name=PREPARE pct=0 t=2.0\n") != NULL, "first line: PREPARE at t=2.0 s, no prev");
    EXPECT(strstr(t, "step=2 name=UPLOAD_FS pct=0 t=9.0 prev=PREPARE prev_secs=7.0") != NULL, "UPLOAD_FS line carries prev=PREPARE prev_secs=7.0");
    EXPECT(count(t, "step=2 name=UPLOAD_FS") <= 25, "a 400 s step with 100 percent reports prints at most 25 lines (%d)", count(t, "step=2 name=UPLOAD_FS"));
    EXPECT(strstr(tm, "PREPARE=7.0 UPLOAD_FS=401.0 VERIFY_FS=20.0") != NULL, "times: each step until the next one's first report, the last one until now: %s", tm);
    char big[8]; g7ph_times(&p, big, sizeof big); EXPECT(strlen(big) < sizeof big, "a small buffer is never overrun");
    free(t); fclose(o);
    o = tmpfile(); p.out = o; T = 5000; int64_t s = T; T += 16000; g7ph_phase_line(&p, "ipsw-sha256", s, 16800000000LL); T += 1500; g7ph_phase_line(&p, "pair", T - 1500, 0);
    t = slurp(o);
    EXPECT(strstr(t, "g7-phase: name=ipsw-sha256 secs=16.0 bytes=16800000000 mb_per_s=1001.4") != NULL && strstr(t, "g7-phase: name=pair secs=1.5\n") != NULL, "phase lines: seconds, and MB/s only when a size is given: %s", t);
    free(t); fclose(o);
    printf(bad ? "FAIL: %d\n" : "ALL PASS\n", bad);
    return bad != 0;
}
