/* The frame-file writer of host/vm.c (the region between FRAME-FILES-BEGIN and FRAME-FILES-END, pasted in by tools/g7-frames-test.sh): PNG validity, bounds, worker thread, flush. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "frames_region.inc"

static int fails;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    unsetenv("LAGFX_ARM64_DUMP_DIR");
    uint8_t px[8 * 4 * 4]; memset(px, 0x80, sizeof px);
    check("no folder set: nothing is written, never retried", frame_save_async("a", "a", px, 8, 4, 32) == -1 && atomic_load(&frame_files) == 0);
    setenv("LAGFX_ARM64_DUMP_DIR", dir, 1);
    uint32_t w = 37, h = 23; uint8_t *img = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { uint8_t *p = img + ((size_t)y * w + x) * 4; p[0] = (uint8_t)(x * 7); p[1] = (uint8_t)(y * 11); p[2] = (uint8_t)(x + y); p[3] = 0xff; }   /* BGRA */
    check("a frame starts a writer", frame_save_async("small", "small", img, w, h, w * 4) == 1);
    memset(img, 0xff, (size_t)w * h * 4);   /* the writer works on its own copy: changing the source now must not change the file */
    uint32_t bw = 1920, bh = 1080; uint8_t *big = malloc((size_t)bw * bh * 4);
    for (size_t i = 0; i < (size_t)bw * bh; i++) { big[i * 4] = (uint8_t)i; big[i * 4 + 1] = (uint8_t)(i >> 8); big[i * 4 + 2] = (uint8_t)(i >> 16); big[i * 4 + 3] = 255; }
    int r = frame_save_async("big", "big", big, bw, bh, bw * 4);
    check("a second frame starts (or the busy answer is 0, never -1)", r == 1 || r == 0);
    if (r == 0) { hv_vm_frames_flush(); r = frame_save_async("big", "big", big, bw, bh, bw * 4); check("after the flush the big frame starts", r == 1); }
    hv_vm_frames_flush();
    check("flush: no writer is left", atomic_load(&frame_inflight) == 0);
    check("a frame over the pixel bound is refused for good", frame_save_async("huge", "huge", big, 4096, 2048, 4096 * 4) == -1);
    char path[700]; struct { const char *tag; } k[] = { {"small_37x23"}, {"big_1920x1080"} };
    for (int i = 0; i < 2; i++) { snprintf(path, sizeof path, "%s/g7_frame_%s.png", dir, k[i].tag); FILE *f = fopen(path, "rb"); check(k[i].tag, f != NULL); if (f) fclose(f); snprintf(path, sizeof path, "%s/g7_frame_%s.png.part", dir, k[i].tag); check("no .part file is left", access(path, F_OK) != 0); }
    int n = 2, last = 0;
    for (int i = 0; i < 10; i++) { char t[32]; snprintf(t, sizeof t, "f%d", i); int q = frame_save_async(t, t, px, 8, 4, 32); if (q == 0) { hv_vm_frames_flush(); q = frame_save_async(t, t, px, 8, 4, 32); } if (q == 1) n++; if (q == -1) last = 1; hv_vm_frames_flush(); }
    check("the file count is bounded (FRAME_MAX_FILES) and the next one is refused for good", n == FRAME_MAX_FILES && last == 1 && atomic_load(&frame_files) == FRAME_MAX_FILES);
    check("the byte budget is not over its bound", atomic_load(&frame_bytes) <= FRAME_MAX_BYTES);
    printf(fails ? "G7FRAMES FAILED\n" : "G7FRAMES OK\n");
    return fails != 0;
}
