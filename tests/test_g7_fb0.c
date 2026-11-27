/* host/g7_fb0.h on synthetic buffers (no VM): scan counts, cheap hash, change-only lines, the 3-dump rule, cost. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../host/g7_fb0.h"
static int fails;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }
static double mono(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
int main(void) {
    uint32_t w = 1920, h = 1080, stride = w * 4;
    uint8_t *px = calloc(1, (size_t)stride * h);
    HvFb0Sample a, b;
    hv_fb0_scan(px, w, h, stride, &a);
    check("black frame: nz 0, words = w*h", a.nz == 0 && a.words == (uint64_t)w * h);
    for (uint32_t i = 0; i < w * h; i++) px[(size_t)i * 4 + 3] = 0xff;
    hv_fb0_scan(px, w, h, stride, &b);
    check("alpha alone is not drawn (nz 0) but changes the hash", b.nz == 0 && b.hash != a.hash);
    for (uint32_t y = 100; y < 200; y++) for (uint32_t x = 200; x < 300; x++) px[(size_t)y * stride + x * 4 + 1] = 0x40;
    hv_fb0_scan(px, w, h, stride, &a);
    check("100x100 block: nz = 10000", a.nz == 10000);
    uint8_t *odd = calloc(1, 4 * 13 * 3 + 64); for (int y = 0; y < 3; y++) for (int x = 0; x < 13; x++) odd[y * 64 + x * 4 + 2] = 1;
    hv_fb0_scan(odd, 13, 3, 64, &b);
    check("width not a multiple of 8 (tail path), padded stride: nz = 39", b.nz == 39 && b.words == 39);
    px[(size_t)150 * stride + 250 * 4 + 1] = 0x41;
    hv_fb0_scan(px, w, h, stride, &b);
    check("one byte changed inside a pixel: same nz, different hash", b.nz == a.nz && b.hash != a.hash);
    hv_fb0_scan(px, w, h, stride, &a);
    check("same pixels: same hash", a.hash == b.hash);
    /* the state machine */
    HvFb0State st; hv_fb0_init(&st); char line[512]; int dump;
    HvFb0Sample z = {0, 1000, 111}, s1 = {10, 1000, 222}, s2 = {10, 1000, 333}, s3 = {45, 1000, 444}, s4 = {44, 1000, 555}, s5 = {12, 1000, 666}, s6 = {3, 1000, 777}, s7 = {3000, 1000, 888};
    check("first sample is logged (change=first), black = no dump", hv_fb0_step(&st, 0.25, &z, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && strstr(line, "change=first") && strstr(line, "src=raw") && dump == 0);
    check("same hash: no line", hv_fb0_step(&st, 0.5, &z, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 0 && dump == 0);
    check("nz first > 0: line, change=nz_up, dump #1 first_nz", hv_fb0_step(&st, 0.75, &s1, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && strstr(line, "change=nz_up") && dump == 1 && strstr(line, "dump=first_nz") && st.t_first_nz == 0.75);
    check("hash moved, nz equal: line, same_nz, no dump", hv_fb0_step(&st, 1.0, &s2, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && strstr(line, "change=same_nz") && dump == 0);
    check("nz 10 -> 45 (4.5x): big change dump #2", hv_fb0_step(&st, 1.25, &s3, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && dump == 2 && strstr(line, "dump=big_change") && st.dumps == 2);
    check("45 -> 44: nz_down, no dump", hv_fb0_step(&st, 1.5, &s4, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && strstr(line, "change=nz_down") && dump == 0);
    check("45 -> 12 (3.75x): not big enough, no dump", hv_fb0_step(&st, 1.75, &s5, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && dump == 0);
    check("45 -> 3 (15x down): dump #3", hv_fb0_step(&st, 2.0, &s6, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && dump == 2 && st.dumps == 3);
    check("a 4th big change: never more than 3 dumps", hv_fb0_step(&st, 2.25, &s7, 1, HV_FB0_DEFAULT_BASE, 0, 0, line, sizeof line, &dump) == 1 && dump == 0 && st.dumps == HV_FB0_MAX_DUMPS);
    HvFb0Sample bs = {5, 2000, 999};
    check("src change (boot_args found): forgotten basis, change=first, geometry in the line", hv_fb0_step(&st, 3.0, &bs, 2, 0xef540000ull, 1920, 1080, line, sizeof line, &dump) == 1 && strstr(line, "src=boot_args") && strstr(line, "win=1920x1080") && strstr(line, "change=first"));
    /* line cap */
    HvFb0State c; hv_fb0_init(&c); int n = 0;
    for (int i = 0; i < 500; i++) { HvFb0Sample q = {0, 10, (uint64_t)i + 1}; n += hv_fb0_step(&c, i * 0.25, &q, 1, 0, 0, 0, line, sizeof line, &dump); }
    check("lines capped at 200, the rest counted as suppressed, observation continues", n == HV_FB0_MAX_LINES && c.suppressed == 500 - HV_FB0_MAX_LINES && c.scans == 500);
    /* first sample already drawn: dump 1 immediately; a black-only run never dumps */
    HvFb0State d; hv_fb0_init(&d); hv_fb0_step(&d, 0.1, &s1, 1, 0, 0, 0, line, sizeof line, &dump);
    check("already drawn at t=0: first_nz dump at once", dump == 1 && d.t_first_nz == 0.1);
    HvFb0State e; hv_fb0_init(&e); for (int i = 0; i < 10; i++) { HvFb0Sample q = {0, 10, (uint64_t)i}; hv_fb0_step(&e, i * 0.25, &q, 1, 0, 0, 0, line, sizeof line, &dump); }
    hv_fb0_end_line(&e, 60.0, 12.5, 0.9, line, sizeof line);
    check("all-black run: no dump, end line says nz_first_t=none with the cost fields", e.dumps == 0 && strstr(line, "nz_first_t=none") && strstr(line, "total_ms=12.5") && strstr(line, "max_ms=0.90"));
    hv_fb0_start_line(line, sizeof line);
    check("start line states period, seconds, base, window", strstr(line, "period_ms=250 seconds=60") && strstr(line, "default_base=0xef540000") && strstr(line, "default_window=9437184"));
    /* cost: 9 MiB window = 2359296 words, as the sampler would scan it each 250 ms (informational; asserts only a loose bound) */
    uint8_t *win = calloc(1, HV_FB0_DEFAULT_WINDOW); for (size_t i = 0; i < HV_FB0_DEFAULT_WINDOW; i += 4096) win[i + 1] = 7;
    double t0 = mono(); int N = 100; uint64_t sink = 0; for (int i = 0; i < N; i++) { ((volatile uint8_t *)win)[1] = (uint8_t)i; hv_fb0_scan(win, HV_FB0_DEFAULT_WINDOW / 4, 1, HV_FB0_DEFAULT_WINDOW, &a); sink += a.hash + a.nz; }
    double ms = (mono() - t0) * 1000 / N;
    printf("INFO scan of a 9 MiB window: %.3f ms per scan on this host (240 scans = %.0f ms in 60 s; sink %llu)\n", ms, ms * 240, (unsigned long long)sink);
    check("one scan of the 9 MiB window takes under 20 ms here (loose bound; the real cost is logged on the phone)", ms < 20);
    free(px); free(odd); free(win);
    printf(fails ? "G7FB0 FAILED %d\n" : "G7FB0 OK\n", fails);
    return fails != 0;
}
