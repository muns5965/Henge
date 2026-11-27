/* host/input_inject.c with a recorder where the machine would be and a simulated guest pointer (1:1, clamped). Nothing here starts a VM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../host/input_inject.h"

static int bad;
#define EXPECT(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

static int gw = 1920, gh = 1080, gx, gy; static int64_t T;
static char ev[4096]; static int nev;
static char lg[8192];
static void rec(const char *s) { nev += snprintf(ev + nev, sizeof ev - (size_t)nev, "%s;", s); }
static void k(uint16_t c, int d) { char b[32]; snprintf(b, sizeof b, "K%u%c@%lld", c, d ? 'v' : '^', (long long)T); rec(b); }
static void mv(int dx, int dy) { gx += dx; gy += dy; if (gx < 0) gx = 0; if (gy < 0) gy = 0; if (gx > gw - 1) gx = gw - 1; if (gy > gh - 1) gy = gh - 1; }
static void bt(int b, int d) { char s[48]; snprintf(s, sizeof s, "B%d%c(%d,%d)@%lld", b, d ? 'v' : '^', gx, gy, (long long)T); rec(s); }
static void lgf(void *c, const char *l) { (void)c; strncat(lg, l, sizeof lg - strlen(lg) - 2); strcat(lg, "\n"); }
static void run(HvInject *j, int feedback, int ms) { for (int i = 0; i < ms / 5; i++) { hv_inject_tick(j, feedback, gx, gy, T); T += 5; } }
static void fresh(HvInject *j) { HvInputSink s = {k, mv, bt, NULL, NULL}; hv_inject_init(j, &s, lgf, NULL); nev = 0; ev[0] = 0; lg[0] = 0; gx = gy = 10; T = 1000; }
static int count(const char *needle) { int n = 0; for (const char *p = ev; (p = strstr(p, needle)); p++) n++; return n; }

int main(void) {
    HvInject j; char m[100];
    fresh(&j);
    EXPECT(hv_inject_command(&j, "key enter", T, m, sizeof m) == 0, "key enter accepted");
    run(&j, 1, 200);
    EXPECT(!strncmp(ev, "K28v@", 5) && strstr(ev, ";K28^@") && count("K") == 2, "enter = code 28 down then up: %s", ev);
    fresh(&j); hv_inject_command(&j, "key shift+tab", T, m, sizeof m); run(&j, 1, 300);
    EXPECT(strstr(ev, "K42v") && strstr(ev, "K15v") && strstr(ev, "K15^") && strstr(ev, "K42^") && strstr(ev, "K42v") < strstr(ev, "K15v") && strstr(ev, "K15^") < strstr(ev, "K42^"), "shift+tab: shift down, tab down, tab up, shift up: %s", ev);
    const char *names[] = {"enter", "tab", "space", "esc", "up", "down", "left", "right"}; unsigned want[] = {28, 15, 57, 1, 103, 108, 105, 106};
    for (int i = 0; i < 8; i++) { char c[32]; fresh(&j); snprintf(c, sizeof c, "key %s", names[i]); hv_inject_command(&j, c, T, m, sizeof m); run(&j, 1, 120); char w[24]; snprintf(w, sizeof w, "K%uv", want[i]); EXPECT(strstr(ev, w) != NULL && count("K") == 2, "%s -> code %u down+up", names[i], want[i]); }
    fresh(&j);
    EXPECT(hv_inject_command(&j, "key bogus", T, m, sizeof m) == -1 && hv_inject_command(&j, "click 1920 5", T, m, sizeof m) == -1 && hv_inject_command(&j, "click 5", T, m, sizeof m) == -1 && hv_inject_command(&j, "nope", T, m, sizeof m) == -1, "bad names / out of picture / half coordinates / unknown command are refused (%s)", m);
    /* click at the Macintosh HD icon with a pointer that reports its place */
    fresh(&j); hv_inject_command(&j, "click 960 397", T, m, sizeof m); run(&j, 1, 3000);
    EXPECT(count("B0v") == 1 && count("B0^") == 1, "click: exactly one left down and one up: %s", ev);
    { int x = -1, y = -1; const char *p = strstr(ev, "B0v("); if (p) sscanf(p, "B0v(%d,%d)", &x, &y); EXPECT(abs(x - 960) <= 2 && abs(y - 397) <= 2, "the button went down with the pointer at %d,%d (wanted 960,397)", x, y); }
    EXPECT(strstr(lg, "arrived") && strstr(lg, "button left DOWN") && strstr(lg, "button left UP") && !strstr(lg, "consumed"), "log has the steps and claims no consumption");
    /* Restart preset, double click, with no pointer feedback (dead reckoning) */
    fresh(&j); hv_inject_command(&j, "dclick 1190 550", T, m, sizeof m); run(&j, 0, 3000);
    { int x = -1, y = -1; const char *p = strstr(ev, "B0v("); if (p) sscanf(p, "B0v(%d,%d)", &x, &y); EXPECT(count("B0v") == 2 && count("B0^") == 2 && abs(x - 1190) <= 2 && abs(y - 550) <= 2, "dclick without feedback: two down/up pairs at %d,%d: %s", x, y, ev); }
    EXPECT(strstr(lg, "NOT available, dead reckoning") != NULL, "the log says the pointer feedback was missing");
    EXPECT(strstr(lg, "ESTIMATE ONLY") != NULL && strstr(lg, "NOT proven") != NULL, "without guest feedback the arrival line says ESTIMATE ONLY (guest arrival not proven)");
    /* a pointer that never moves: the click goes ahead after the timeout and the log says it did not arrive */
    fresh(&j); hv_inject_command(&j, "click 960 397", T, m, sizeof m);
    for (int i = 0; i < 3000 / 5; i++) { hv_inject_tick(&j, 1, 10, 10, T); T += 5; }
    EXPECT(count("B0v") == 1 && strstr(lg, "did NOT arrive") != NULL, "a stuck pointer: clicked after the timeout, logged as not arrived");
    /* click here: no move */
    fresh(&j); gx = 300; gy = 200; hv_inject_command(&j, "click", T, m, sizeof m); run(&j, 1, 300);
    EXPECT(count("B0v(300,200)") == 1 && gx == 300 && gy == 200, "click at the current pointer moves nothing");
    /* two jobs in order, then abort releases a held button */
    fresh(&j); hv_inject_command(&j, "key tab", T, m, sizeof m); hv_inject_command(&j, "key enter", T, m, sizeof m); run(&j, 1, 400);
    EXPECT(strstr(ev, "K15v") < strstr(ev, "K28v") && strstr(ev, "K15^") < strstr(ev, "K28v"), "jobs run one after the other");
    fresh(&j); hv_inject_command(&j, "click", T, m, sizeof m); run(&j, 1, 20); EXPECT(count("B0v") == 1, "button held"); hv_inject_command(&j, "abort", T, m, sizeof m); EXPECT(count("B0^") == 1 && !hv_inject_busy(&j), "abort released the button and emptied the queue");
    /* display size change */
    fresh(&j); gw = 1280; gh = 720; EXPECT(hv_inject_command(&j, "display 1280 720", T, m, sizeof m) == 0 && hv_inject_command(&j, "click 1279 719", T, m, sizeof m) == 0, "display 1280x720 then a click at its corner"); run(&j, 1, 3000);
    EXPECT(count("B0v(1278,719)") + count("B0v(1279,719)") == 1, "corner click lands within 1 px of the corner (%s)", ev);
    /* no input device: key / click / dclick are REJECTED (-2), nothing is queued, nothing reaches the sink; with the device present the same commands are accepted as before */
    fresh(&j); hv_inject_set_paths(&j, 0, "no virtual xhci HID keyboard device", 0, "no virtual mouse device");
    { int r1 = hv_inject_command(&j, "key enter", T, m, sizeof m); int bad_msg = !strstr(m, "no virtual xhci HID keyboard device") || !strstr(m, "input cannot reach the guest");
      int r2 = hv_inject_command(&j, "click 100 100", T, m, sizeof m); int r3 = hv_inject_command(&j, "dclick", T, m, sizeof m);
      run(&j, 1, 500);
      EXPECT(r1 == -2 && r2 == -2 && r3 == -2 && !bad_msg, "no device: key/click/dclick answer REJECTED (-2) with the reason (%s)", m);
      EXPECT(nev == 0 && ev[0] == 0 && !hv_inject_busy(&j) && j.keys_down == 0 && j.btn_down == 0 && j.jobs_done == 0 && j.rejected_no_path == 3 && lg[0] == 0, "no device: no event reached the sink, nothing queued, delivered-type counters unchanged (rejected_no_path=3): %s", ev);
      EXPECT(hv_inject_command(&j, "display 1280 720", T, m, sizeof m) == 0 && hv_inject_command(&j, "abort", T, m, sizeof m) == 0, "display/abort are not input and still work"); }
    fresh(&j); hv_inject_set_paths(&j, 1, "x", 0, "no virtual mouse device");
    EXPECT(hv_inject_command(&j, "key enter", T, m, sizeof m) == 0 && hv_inject_command(&j, "click", T, m, sizeof m) == -2, "keyboard present / mouse missing: key accepted, click REJECTED");
    run(&j, 1, 200); EXPECT(count("K28v") == 1 && count("B0") == 0, "only the key reached the sink");
    fresh(&j); hv_inject_set_paths(&j, 1, NULL, 1, NULL);
    EXPECT(hv_inject_command(&j, "key enter", T, m, sizeof m) == 0 && hv_inject_command(&j, "click 960 397", T, m, sizeof m) == 0 && j.rejected_no_path == 0, "devices present: accepted as before");
    run(&j, 1, 3000); EXPECT(count("K28v") == 1 && count("B0v") == 1, "devices present: both reached the sink");
    printf("%s test_input_inject\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
