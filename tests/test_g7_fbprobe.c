/* host/g7_fbprobe.h: the boot-framebuffer probe on synthetic buffers (no VM). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../host/g7_fbprobe.h"
static int fails;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }
int main(void) {
    uint32_t w = 640, h = 400, stride = w * 4 + 64;
    uint8_t *px = calloc(1, (size_t)stride * h);
    HvFbpSummary a, b;
    check("empty frame: samples counted, none non-empty", hv_fbp_summarize(px, w, h, stride, &a) == 0 && a.samples > 0 && a.nonempty == 0 && hv_fbp_ppm(&a) == 0);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) px[(size_t)y * stride + x * 4 + 3] = 0xff;   /* alpha only: still empty */
    check("alpha alone does not count as drawn", hv_fbp_summarize(px, w, h, stride, &b) == 0 && b.nonempty == 0);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) px[(size_t)y * stride + x * 4] = 0x80;
    check("all drawn: 1000000 ppm", hv_fbp_summarize(px, w, h, stride, &b) == 0 && hv_fbp_ppm(&b) == 1000000 && b.hash != a.hash);
    check("padding bytes outside the width are not read into the hash", (px[(size_t)stride * 3 + w * 4] = 0x55, hv_fbp_summarize(px, w, h, stride, &a) == 0 && a.hash == b.hash));
    check("bad input is refused", hv_fbp_summarize(NULL, w, h, stride, &a) == -1 && hv_fbp_summarize(px, w, h, w * 4 - 1, &a) == -1);
    uint8_t *big = calloc(1, 4096u * 2048 * 4);
    check("4096x2048 stays within 256x256 samples", hv_fbp_summarize(big, 4096, 2048, 4096 * 4, &a) == 0 && a.samples <= 65536);
    /* tick: rate, change detection, caps */
    HvFbpState st; memset(&st, 0, sizeof st);
    uint8_t *f = calloc(1, 320 * 200 * 4); char line[256]; int chg, t10;
    check("first tick logs with changed=first", hv_fbp_tick(&st, 100.0, f, 320, 200, 1280, line, sizeof line, &chg, &t10) == 1 && strstr(line, "changed=first") && strncmp(line, "g7-boot-fb: t=0.0s", 18) == 0 && !chg);
    check("a tick within 2 s logs nothing", hv_fbp_tick(&st, 101.0, f, 320, 200, 1280, line, sizeof line, &chg, &t10) == 0);
    check("same pixels after 2 s: changed=0", hv_fbp_tick(&st, 102.0, f, 320, 200, 1280, line, sizeof line, &chg, &t10) == 1 && strstr(line, "changed=0") && !chg);
    memset(f, 0xff, 320 * 100 * 4);
    check("changed pixels: changed=1 and the first-change flag", hv_fbp_tick(&st, 104.0, f, 320, 200, 1280, line, sizeof line, &chg, &t10) == 1 && strstr(line, "changed=1") && chg == 1 && strstr(line, "nonempty=500000ppm"));
    memset(f, 0x11, 320 * 200 * 4);
    check("a second change does not set the first-change flag again", hv_fbp_tick(&st, 106.0, f, 320, 200, 1280, line, sizeof line, &chg, &t10) == 1 && chg == 0 && st.nchanges == 2);
    check("t>=10 s flag fires once", hv_fbp_tick(&st, 110.0, f, 320, 200, 1280, line, sizeof line, &chg, &t10) == 1 && t10 == 1 && hv_fbp_tick(&st, 112.0, f, 320, 200, 1280, line, sizeof line, &chg, &t10) == 1 && t10 == 0);
    int n = st.lines; double t = 112.0;
    for (int i = 0; i < 100; i++) { t += 2.0; n += hv_fbp_tick(&st, t, f, 320, 200, 1280, line, sizeof line, &chg, &t10); }
    check("line cap is 30", n == HV_FBP_MAX_LINES && st.lines == HV_FBP_MAX_LINES);
    HvFbpState s2; memset(&s2, 0, sizeof s2); int c = 0;
    for (int i = 0; i < 20; i++) c += hv_fbp_src(&s2, 1.0 + i, (unsigned)(i % 3), "x", 1, 1, line, sizeof line);
    check("each source word once", c == 3);
    /* boot_args line */
    uint64_t q[16] = { 0 }; uint16_t hdr[2] = { 2, 2 };
    q[1] = 0xfffffe0007004000ull; q[2] = 0x70000000ull; q[3] = 0x40000000ull; q[5] = 0x78000000ull; q[7] = 5120; q[8] = 1280; q[9] = 800; q[10] = 32;
    check("valid boot_args video", strcmp(hv_fbp_video_check(q, 0x70000000ull, 0x10000000ull), "ok") == 0);
    hv_fbp_boot_args_line(line, sizeof line, 0x4000, hdr, q, 0x70000000ull, 0x10000000ull);
    check("boot_args line has every field", strstr(line, "v_baseAddr=0x78000000") && strstr(line, "v_rowBytes=5120") && strstr(line, "v_width=1280") && strstr(line, "v_height=800") && strstr(line, "v_depth=0x20") && strstr(line, "valid=1") && strstr(line, "v_display=0 "));
    q[8] = 0; check("zero width is reported, not hidden", strcmp(hv_fbp_video_check(q, 0x70000000ull, 0x10000000ull), "size") == 0);
    hv_fbp_boot_args_line(line, sizeof line, 0x4000, hdr, q, 0x70000000ull, 0x10000000ull);
    check("invalid line says valid=0 why=size", strstr(line, "valid=0 why=size") != NULL);
    q[8] = 1280; q[10] = 16; check("depth 16 -> depth", strcmp(hv_fbp_video_check(q, 0x70000000ull, 0x10000000ull), "depth") == 0);
    q[10] = 32; q[5] = 0x10; check("base outside RAM -> baseaddr", strcmp(hv_fbp_video_check(q, 0x70000000ull, 0x10000000ull), "baseaddr") == 0);
    printf(fails ? "G7FBPROBE FAILED\n" : "G7FBPROBE OK\n");
    return fails != 0;
}
