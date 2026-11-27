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
    check("empty frame: samples counted, none drawn, no bbox", hv_fbp_summarize(px, w, h, stride, &a) == 0 && a.samples > 0 && a.rgb_nonzero == 0 && !a.bbox_valid && a.alpha_nonmax == a.samples && a.luma[0] == a.samples);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) px[(size_t)y * stride + x * 4 + 3] = 0xff;   /* alpha only */
    check("alpha alone is not rgb_nonzero, and alpha_nonmax drops to 0", hv_fbp_summarize(px, w, h, stride, &b) == 0 && b.rgb_nonzero == 0 && b.alpha_nonmax == 0 && !b.bbox_valid);
    for (uint32_t y = 100; y < 200; y++) for (uint32_t x = 200; x < 400; x++) { px[(size_t)y * stride + x * 4] = 0xff; px[(size_t)y * stride + x * 4 + 1] = 0xff; px[(size_t)y * stride + x * 4 + 2] = 0xff; }
    check("white block: bbox inside the block, white in the top luma bin, hash changed", hv_fbp_summarize(px, w, h, stride, &b) == 0 && b.bbox_valid && b.x0 >= 200 && b.x1 < 400 && b.y0 >= 100 && b.y1 < 200 && b.luma[7] > 0 && b.luma[7] == b.rgb_nonzero && b.hash != a.hash);
    check("luma bins add up to the samples", b.luma[0] + b.luma[1] + b.luma[2] + b.luma[3] + b.luma[4] + b.luma[5] + b.luma[6] + b.luma[7] == b.samples);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) px[(size_t)y * stride + x * 4] = 0x80;
    check("all coloured: 1000000 ppm", hv_fbp_summarize(px, w, h, stride, &b) == 0 && hv_fbp_ppm_of(b.rgb_nonzero, b.samples) == 1000000);
    check("padding bytes outside the width are not read into the hash", (px[(size_t)stride * 3 + w * 4] = 0x55, hv_fbp_summarize(px, w, h, stride, &a) == 0 && a.hash == b.hash));
    check("bad input is refused", hv_fbp_summarize(NULL, w, h, stride, &a) == -1 && hv_fbp_summarize(px, w, h, w * 4 - 1, &a) == -1);
    uint8_t *big = calloc(1, 4096u * 2048 * 4);
    check("4096x2048 stays within 256x256 samples", hv_fbp_summarize(big, 4096, 2048, 4096 * 4, &a) == 0 && a.samples <= 65536);
    /* tick: first line immediately, rate, change detection, caps that limit lines but not observation */
    HvFbpState st; memset(&st, 0, sizeof st);
    uint8_t *f = calloc(1, 1344u * 200u); char line[1024]; int chg, t10;
    check("first tick logs kind=first with rowBytes and width*4 and the INFER note", hv_fbp_tick(&st, 100.0, f, 320, 200, 1280 + 64, line, sizeof line, &chg, &t10) == 1 && strstr(line, "kind=first") && strstr(line, "rowBytes=1344 width*4=1280") && strstr(line, "INFER") && strstr(line, "sampled") && strstr(line, "bbox_rgb=none") && !chg);
    check("a tick within 2 s logs nothing", hv_fbp_tick(&st, 101.0, f, 320, 200, 1344, line, sizeof line, &chg, &t10) == 0);
    check("same pixels after 2 s: periodic, changed=0", hv_fbp_tick(&st, 102.0, f, 320, 200, 1344, line, sizeof line, &chg, &t10) == 1 && strstr(line, "kind=periodic") && strstr(line, "changed=0") && !chg);
    for (int y = 0; y < 100; y++) memset(f + (size_t)y * 1344, 0xff, 1280);
    check("changed pixels: changed=1, first-change flag, bbox", hv_fbp_tick(&st, 104.0, f, 320, 200, 1344, line, sizeof line, &chg, &t10) == 1 && strstr(line, "changed=1") && chg == 1 && strstr(line, "bbox_rgb=0,0,"));
    memset(f, 0x11, 320 * 100 * 4);
    check("a second change does not set the first-change flag again", hv_fbp_tick(&st, 106.0, f, 320, 200, 1344, line, sizeof line, &chg, &t10) == 1 && chg == 0 && st.nchanges == 2);
    check("t>=10 s flag fires once", hv_fbp_tick(&st, 110.0, f, 320, 200, 1344, line, sizeof line, &chg, &t10) == 1 && t10 == 1 && hv_fbp_tick(&st, 112.0, f, 320, 200, 1344, line, sizeof line, &chg, &t10) == 1 && t10 == 0);
    int periodic = st.lines; double t = 112.0; int extra_change = 0, extra_slow = 0;
    for (int i = 0; i < 40; i++) { t += 2.0; hv_fbp_tick(&st, t, f, 320, 200, 1344, line, sizeof line, &chg, &t10); }   /* unchanged frame, to and past the periodic cap */
    check("periodic lines stop at the cap of 30", st.lines == HV_FBP_MAX_LINES);
    int n0 = st.nchanges;
    for (int i = 0; i < 5; i++) { f[(size_t)i * 8] ^= 0x7f; t += 2.0; if (hv_fbp_tick(&st, t, f, 320, 200, 1344, line, sizeof line, &chg, &t10) && strstr(line, "kind=change")) extra_change++; }
    check("after the cap a hash change still logs (kind=change) and is counted", extra_change >= 1 && st.nchanges == n0 + 5 && st.lines == HV_FBP_MAX_LINES);
    for (int i = 0; i < 200; i++) { t += 2.0; if (hv_fbp_tick(&st, t, f, 320, 200, 1344, line, sizeof line, &chg, &t10) && strstr(line, "kind=slow")) extra_slow++; }
    check("after the cap an unchanged frame logs one slow line per 60 s, at most 30 (t spans 400 s: 6 or 7)", extra_slow >= 6 && extra_slow <= 7);
    HvFbpState s3; memset(&s3, 0, sizeof s3); t = 0.5; int chglines = 0;
    for (int i = 0; i < 400; i++) { t += 2.0; f[0] ^= 1; if (hv_fbp_tick(&s3, t, f, 320, 200, 1344, line, sizeof line, &chg, &t10)) { if (strstr(line, "kind=change")) chglines++; } }
    check("change lines are capped at 60 while observation continues", chglines == HV_FBP_MAX_CHANGE_LINES && s3.nchanges == 399);
    check("final line is formatted without touching the statistics", (periodic = st.nchanges, hv_fbp_final(&st, t, f, 320, 200, 1344, line, sizeof line) == 1 && strstr(line, "kind=final") && st.nchanges == periodic));
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
    {   /* the command-line line: synthetic boot_args page with the XNU boot.h revision-2 offsets */
        static uint8_t page[0x1000]; char cl[900]; memset(page, 0, sizeof page);
        uint16_t r = 3, v = 2; uint32_t mt = 0x20, dtl = 0x12345; uint64_t dtp = 0xfffffe0012345000ull;
        memcpy(page, &r, 2); memcpy(page + 2, &v, 2); memcpy(page + 88, &mt, 4); memcpy(page + 96, &dtp, 8); memcpy(page + 104, &dtl, 4);
        memcpy(page + HV_FBP_CMDLINE_OFF, "rd=md0 -v\x01 debug=0x14e", 22);
        hv_fbp_cmdline_line(cl, sizeof cl, page, sizeof page);
        check("cmdline line: text, length, machineType, device-tree words, rev/ver, INFER", strstr(cl, "cmdline=\"rd=md0 -v? debug=0x14e\"") && strstr(cl, "cmdline_len=22") && strstr(cl, "terminated=1") && strstr(cl, "nonprintable=1") &&
              strstr(cl, "machineType=32") && strstr(cl, "deviceTreeP=0xfffffe0012345000") && strstr(cl, "deviceTreeLength=74565") && strstr(cl, "rev=3 ver=2") && strstr(cl, "INFER"));
        memset(page + HV_FBP_CMDLINE_OFF, 'A', sizeof page - HV_FBP_CMDLINE_OFF);
        hv_fbp_cmdline_line(cl, sizeof cl, page, sizeof page);
        check("unterminated long text: bounded to 1024 (BOOT_LINE_LENGTH) and the page, shown capped, terminated=0, line fits", strstr(cl, "terminated=0") && strstr(cl, "shown=240") && strlen(cl) < 700 && strstr(cl, "cmdline_len=1024"));
        memset(page + HV_FBP_CMDLINE_OFF, 0, 64);
        hv_fbp_cmdline_line(cl, sizeof cl, page, sizeof page);
        check("empty command line is reported as empty", strstr(cl, "cmdline_len=0") && strstr(cl, "cmdline=\"\""));
        hv_fbp_cmdline_line(cl, sizeof cl, page, 100);
        check("page shorter than the offset is refused in words, nothing read past it", strstr(cl, "page too short") != NULL);
    }
    printf(fails ? "G7FBPROBE FAILED\n" : "G7FBPROBE OK\n");
    return fails != 0;
}
