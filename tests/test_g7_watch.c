/* host/g7_watch.h: the watchdog line logic with synthetic samples. Nothing is run, nothing is connected. */
#include <stdio.h>
#include <string.h>
#include "../host/g7_watch.h"

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static HvG7WatchSample S(uint64_t t, uint64_t b0, uint64_t b1, uint64_t done, uint64_t infl, uint64_t age, int type, uint64_t usb) {
    HvG7WatchSample s = {t, {b0, b1}, done, infl, age, type, usb}; return s;
}

int main(void) {
    char l[512]; HvG7WatchState st; memset(&st, 0, sizeof st);
    HvG7WatchSample a = S(30, 100, 10, 5, 0, 0, 0, 3);
    CHECK(hv_g7w_step(&st, &a, l, sizeof l) == 0 && !strncmp(l, "g7-alive: t=30s ", 16) && strstr(l, "progress=yes") && l[strlen(l) - 1] == '\n', "the first sample is an alive line with progress=yes");
    HvG7WatchSample b = S(60, 100, 10, 5, 0, 0, 0, 3);
    CHECK(hv_g7w_step(&st, &b, l, sizeof l) == 0 && strstr(l, "progress=no") && !strncmp(l, "g7-alive", 8), "one quiet sample: still an alive line, progress=no");
    HvG7WatchSample c = S(90, 100, 10, 5, 0, 0, 0, 3);
    CHECK(hv_g7w_step(&st, &c, l, sizeof l) == 1 && !strncmp(l, "g7-stall: quiet=60s ", 20), "two quiet samples: a stall line, quiet=60s (counted from the last movement)");
    CHECK(strstr(l, "disk_inflight=0") && strstr(l, "no disk request in flight"), "nothing in flight: the hint names an idle or waiting guest");
    HvG7WatchSample d = S(120, 100, 10, 5, 1, 118, 4, 3);
    CHECK(hv_g7w_step(&st, &d, l, sizeof l) == 1 && strstr(l, "quiet=90s") && strstr(l, "disk_last=flush@118s") && strstr(l, "the host may be stuck in storage"), "a flush that started before the quiet period and never finished: the hint names storage");
    HvG7WatchSample e = S(150, 100, 10, 5, 1, 3, 1, 3);
    CHECK(hv_g7w_step(&st, &e, l, sizeof l) == 1 && strstr(l, "disk_last=write@3s") && strstr(l, "started recently"), "a recent request in flight is not blamed on storage");
    HvG7WatchSample f = S(180, 100, 11, 5, 0, 0, 0, 3);
    CHECK(hv_g7w_step(&st, &f, l, sizeof l) == 0 && strstr(l, "progress=yes") && st.quiet_ticks == 0, "any counter moving ends the stall");
    HvG7WatchSample g = S(210, 100, 11, 5, 0, 0, 0, 4);
    CHECK(hv_g7w_step(&st, &g, l, sizeof l) == 0 && strstr(l, "progress=yes"), "a USB command counts as progress");
    HvG7WatchSample h = S(240, 100, 11, 6, 0, 0, 0, 4);
    CHECK(hv_g7w_step(&st, &h, l, sizeof l) == 0 && strstr(l, "progress=yes"), "a finished disk request counts as progress");
    HvG7WatchSample z = S(300, 7, 8, 9, 0, 0, 77, 1);
    memset(&st, 0, sizeof st); hv_g7w_step(&st, &z, l, sizeof l); z.t_s = 330; hv_g7w_step(&st, &z, l, sizeof l); z.t_s = 360; hv_g7w_step(&st, &z, l, 40);
    CHECK(strlen(l) < 40 && l[strlen(l) - 1] != '\0', "a small buffer is cut off, never overrun");
    { int ok = 1; memset(&st, 0, sizeof st); z.t_s = 1; hv_g7w_step(&st, &z, l, sizeof l); z.t_s = 31; hv_g7w_step(&st, &z, l, sizeof l); z.t_s = 61; int r = hv_g7w_step(&st, &z, l, sizeof l);
      ok = r == 1 && strstr(l, "disk_last=other@") != NULL; CHECK(ok, "an unknown virtio type is named 'other'"); }
    { int bad = 0; for (const char *q = l; *q; q++) if (*q == '/') bad++; CHECK(!bad, "no path separators in a line"); }
    printf(g_fail ? "G7WATCH FAILED (%d)\n" : "G7WATCH OK (%d failed)\n", g_fail);
    return g_fail != 0;
}
