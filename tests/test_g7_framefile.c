#define _GNU_SOURCE
/* integ7b T9 host test: a synthetic 1920x1080 BGRA picture goes through the REAL shared-file writer (hv_shm_publish, with a padded source stride), is read back the way the runner does
   (hv_shm_read_frame), written by the runner's file writer (g7ff_publish) and then read by the app's Java reader (tests/java/G7FrameFileTest.java, run by tools/g7-frameview-test.sh).
   This program writes <dir>/g7_frame.bin and <dir>/expected.txt: "meta W H SEQ" and "px X Y B G R A" lines the Java test compares exactly.
   The picture has: a 1 px grid of tiny text-like strokes, a ring (circle) with an anti-aliased-free edge, eight colour bars, four distinct corner markers, and a gradient behind them.
   Usage: test_g7_framefile <dir> */
#include "../host/frame_shm.h"
#include "../host/g7_frame_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1920u
#define H 1080u

static void put(uint8_t *img, uint32_t stride, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= (int)W || y >= (int)H) return;
    uint8_t *p = img + (size_t)y * stride + (size_t)x * 4;
    p[0] = b, p[1] = g, p[2] = r, p[3] = 255;
}

static void draw(uint8_t *img, uint32_t stride) {
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) put(img, stride, (int)x, (int)y, (uint8_t)(x * 255 / (W - 1) / 4), (uint8_t)(y * 255 / (H - 1) / 4), 40);   /* dark gradient */
    static const uint8_t bars[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{0,0,0}};
    for (int b = 0; b < 8; b++) for (int y = 100; y < 160; y++) for (int x = b * 240; x < b * 240 + 240; x++) put(img, stride, x, y, bars[b][0], bars[b][1], bars[b][2]);
    /* ring: centre (960,540), radius 200..206, white */
    for (int y = 300; y < 780; y++) for (int x = 720; x < 1200; x++) { int dx = x - 960, dy = y - 540, d2 = dx * dx + dy * dy; if (d2 >= 200 * 200 && d2 < 207 * 207) put(img, stride, x, y, 250, 250, 250); }
    /* text-like strokes: 5x7 "glyph" cells of 1 px lines with 1 px gaps, at (300..,850..) */
    for (int c = 0; c < 40; c++) for (int k = 0; k < 7; k++) { int x0 = 300 + c * 8, y0 = 850 + k * 2; if (((c * 7 + k * 3) % 5) < 3) for (int i = 0; i < 5; i++) put(img, stride, x0 + i, y0, 230, 230, 230); put(img, stride, x0, y0 + 1, 230, 230, 230); }
    /* corner markers 16x16, each different */
    static const uint8_t cm[4][3] = {{255,0,0},{0,255,0},{0,0,255},{255,128,0}};
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) { put(img, stride, i, j, cm[0][0], cm[0][1], cm[0][2]); put(img, stride, (int)W - 1 - i, j, cm[1][0], cm[1][1], cm[1][2]); put(img, stride, i, (int)H - 1 - j, cm[2][0], cm[2][1], cm[2][2]); put(img, stride, (int)W - 1 - i, (int)H - 1 - j, cm[3][0], cm[3][1], cm[3][2]); }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dir>\n", argv[0]); return 2; }
    char shm[1024], exp[1024];
    snprintf(shm, sizeof shm, "%s/g7_cloneboot.shm", argv[1]); snprintf(exp, sizeof exp, "%s/expected.txt", argv[1]);
    uint32_t stride = W * 4 + 64;   /* a padded source row: the writer must pack it tightly */
    uint8_t *img = calloc(1, (size_t)stride * H);
    if (!img) return 2;
    draw(img, stride);
    /* a poisoned padding byte: if the packing were wrong, pixels would shift */
    for (uint32_t y = 0; y < H; y++) memset(img + (size_t)y * stride + W * 4, 0xEE, 64);
    HvShm *s = NULL, *app = NULL;
    if (hv_shm_create(shm, &s) != 0) { perror("hv_shm_create"); return 1; }
    if (hv_shm_open(shm, &app) != 0) { perror("hv_shm_open"); return 1; }
    int fails = 0;
    if (hv_shm_publish(s, img, W, H, stride) != 1) { printf("FAIL publish\n"); return 1; }
    if (hv_shm_publish(s, img, W, H, stride) != 1) { printf("FAIL publish 2\n"); return 1; }   /* seq 2 */
    uint8_t *buf = malloc(HV_SHM_PIXEL_BYTES);
    uint64_t seen = 0; uint32_t w = 0, h = 0;
    if (!buf || hv_shm_read_frame(app, &seen, buf, HV_SHM_PIXEL_BYTES, &w, &h) != 1 || w != W || h != H || seen != 2) { printf("FAIL read_frame (w=%u h=%u seq=%llu)\n", w, h, (unsigned long long)seen); return 1; }
    if (hv_shm_read_frame(app, &seen, buf, HV_SHM_PIXEL_BYTES, &w, &h) != 0) { printf("FAIL: an unchanged picture must read as 'nothing newer'\n"); fails++; }
    uint64_t seq = seen;
    for (uint32_t y = 0; y < H; y += 97) if (memcmp(buf + (size_t)y * W * 4, img + (size_t)y * stride, W * 4) != 0) { printf("FAIL row %u differs after the shared-file round trip\n", y); fails++; }
    if (g7ff_publish(argv[1], buf, w, h, seq) != 0) { perror("g7ff_publish"); return 1; }
    /* a second publish must replace the file whole (and leave no temp file) */
    if (g7ff_publish(argv[1], buf, w, h, seq) != 0) { perror("g7ff_publish 2"); return 1; }
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s/%s", argv[1], G7FF_TMP);
    if (access(tmp, F_OK) == 0) { printf("FAIL: temp file left behind\n"); fails++; }
    /* expected pixels: fixed points + a deterministic pseudo-random set; values come from the ORIGINAL image (not from what was read back) */
    FILE *f = fopen(exp, "w");
    if (!f) return 2;
    fprintf(f, "meta %u %u %llu\n", W, H, (unsigned long long)seq);
    int fx[] = {0, 1, 15, 16, 1919, 1904, 0, 1919, 120, 360, 600, 840, 1080, 1320, 1560, 1800, 960, 960, 1160, 1166, 1167, 760, 300, 301, 308, 309, 316};
    int fy[] = {0, 0, 15, 16, 0, 15, 1079, 1079, 130, 130, 130, 130, 130, 130, 130, 130, 340, 740, 540, 540, 540, 540, 850, 851, 852, 853, 854};
    for (size_t i = 0; i < sizeof fx / sizeof fx[0]; i++) { const uint8_t *p = img + (size_t)fy[i] * stride + (size_t)fx[i] * 4; fprintf(f, "px %d %d %u %u %u %u\n", fx[i], fy[i], p[0], p[1], p[2], p[3]); }
    uint32_t r = 12345;
    for (int i = 0; i < 400; i++) { r = r * 1664525u + 1013904223u; int x = (int)((r >> 8) % W); r = r * 1664525u + 1013904223u; int y = (int)((r >> 8) % H); const uint8_t *p = img + (size_t)y * stride + (size_t)x * 4; fprintf(f, "px %d %d %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]); }
    fclose(f);
    hv_shm_close(app); hv_shm_close(s); free(buf); free(img);
    printf("test_g7_framefile: wrote %s/%s (%ux%u, seq %llu) and expected.txt, %d failed\n", argv[1], G7FF_NAME, W, H, (unsigned long long)seq, fails);
    return fails != 0;
}
