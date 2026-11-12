/* C0 + B0 unit tests (Linux, no VM, no Android): host/display_buf.c, host/key_map.c, host/touch_input.c.
   Build and run: tools/c0b0-test.sh <build dir>   (plain, ASan+UBSan, TSan). Exit 0 = every check passed. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../host/display_buf.h"
#include "../host/key_map.h"
#include "../host/touch_input.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void group(const char *n) { printf("-- %s\n", n); }

/* ======================= B0: display buffer ======================= */

static void test_convert(void) {
    group("bgra -> rgbx");
    uint8_t src[2 * 3 * 4]; /* 3 x 2, stride 12 */
    for (int i = 0; i < 6; i++) { src[i * 4] = (uint8_t)(10 + i); src[i * 4 + 1] = (uint8_t)(20 + i); src[i * 4 + 2] = (uint8_t)(30 + i); src[i * 4 + 3] = 0; }
    uint8_t dst[4 * 4 * 4]; memset(dst, 0xee, sizeof dst); /* 4 x 4 destination, stride 4 pixels */
    size_t n = hv_disp_bgra_to_rgbx(src, 3, 2, 12, dst, 4, 4, 4);
    CHECK(n == 6, "pixels written %zu", n);
    CHECK(dst[0] == 30 && dst[1] == 20 && dst[2] == 10 && dst[3] == 0xff, "first pixel R G B X = %u %u %u %u", dst[0], dst[1], dst[2], dst[3]);
    CHECK(dst[4 * 4] == 33 && dst[4 * 4 + 1] == 23 && dst[4 * 4 + 2] == 13, "second row starts at the destination stride");
    CHECK(dst[3 * 4] == 0xee, "outside the source stays untouched");
    memset(dst, 0xee, sizeof dst);
    n = hv_disp_bgra_to_rgbx(src, 3, 2, 12, dst, 2, 2, 1); /* destination smaller: clipped to 2 x 1 */
    CHECK(n == 2, "clipped to %zu pixels", n);
    CHECK(dst[2 * 4] == 0xee, "nothing written past the clip");
    CHECK(hv_disp_bgra_to_rgbx(NULL, 3, 2, 12, dst, 4, 4, 4) == 0, "NULL source");
    CHECK(hv_disp_bgra_to_rgbx(src, 3, 2, 8, dst, 4, 4, 4) == 0, "stride shorter than the row");
    CHECK(hv_disp_bgra_to_rgbx(src, 0, 2, 12, dst, 4, 4, 4) == 0, "zero width");
    CHECK(hv_disp_bgra_to_rgbx(src, 3, 2, 12, dst, 3, 4, 4) == 0, "destination stride smaller than its width");
}

static void test_buffer_basic(void) {
    group("display buffer: put, size, blit");
    HvDispBuf *b = hv_dispbuf_create();
    uint64_t seen = 0; uint8_t out[16 * 8 * 4]; uint32_t w, h;
    CHECK(hv_dispbuf_size(b, &w, &h) == -1, "no frame yet");
    CHECK(hv_dispbuf_blit(b, &seen, out, 16, 16, 8) == -1, "blit before the first frame");
    uint8_t px[16 * 8 * 4]; for (size_t i = 0; i < sizeof px; i += 4) { px[i] = 1; px[i + 1] = 2; px[i + 2] = 3; px[i + 3] = 0; }
    CHECK(hv_dispbuf_put(b, px, 16, 8, 64) == HV_DISP_NEW, "put");
    CHECK(hv_dispbuf_size(b, &w, &h) == 0 && w == 16 && h == 8, "size %ux%u", w, h);
    CHECK(hv_dispbuf_blit(b, &seen, out, 16, 16, 8) == 1 && out[0] == 3 && out[1] == 2 && out[2] == 1 && out[3] == 0xff, "first blit converts");
    CHECK(hv_dispbuf_blit(b, &seen, out, 16, 16, 8) == 0, "no newer frame: 0");
    px[0] = 9;
    CHECK(hv_dispbuf_put(b, px, 16, 8, 64) == HV_DISP_NEW, "second put");
    CHECK(hv_dispbuf_blit(b, &seen, out, 16, 16, 8) == 1 && out[2] == 9, "newer frame: 1, new content");
    /* the buffer holds a copy: changing the source afterwards changes nothing */
    px[0] = 77;
    uint64_t s2 = 0; CHECK(hv_dispbuf_blit(b, &s2, out, 16, 16, 8) == 1 && out[2] == 9, "the held frame is a copy");
    /* a frame of a different size */
    uint8_t big[32 * 20 * 4]; memset(big, 5, sizeof big);
    CHECK(hv_dispbuf_put(b, big, 32, 20, 128) == HV_DISP_NEW, "other size");
    CHECK(hv_dispbuf_size(b, &w, &h) == 0 && w == 32 && h == 20, "new size");
    /* a source with padding at the end of each row */
    uint8_t padded[8 * 4 * 2 + 16 * 2]; memset(padded, 0, sizeof padded);
    padded[0] = 4; padded[1] = 5; padded[2] = 6; padded[(8 * 4 + 16) + 0] = 40;
    CHECK(hv_dispbuf_put(b, padded, 8, 2, 8 * 4 + 16) == HV_DISP_NEW, "padded rows");
    uint8_t o2[8 * 2 * 4]; uint64_t s3 = 0;
    CHECK(hv_dispbuf_blit(b, &s3, o2, 8, 8, 2) == 1 && o2[0] == 6 && o2[8 * 4 + 2] == 40, "row length honoured");
    HvDispStats st; hv_dispbuf_stats(b, &st);
    CHECK(st.frames == 4, "frames counted %llu", (unsigned long long)st.frames);
    hv_dispbuf_destroy(b);
}

static void test_rejects(void) {
    group("display buffer: frames that cannot be frames");
    HvDispBuf *b = hv_dispbuf_create();
    static uint8_t px[64 * 64 * 4];
    CHECK(hv_dispbuf_put(b, px, 64, 64, 256) == HV_DISP_NEW, "good frame");
    CHECK(hv_dispbuf_put(b, px, 0, 64, 256) == HV_DISP_REJECTED, "width 0");
    CHECK(hv_dispbuf_put(b, px, 64, 0, 256) == HV_DISP_REJECTED, "height 0");
    CHECK(hv_dispbuf_put(b, px, 64, 64, 255) == HV_DISP_REJECTED, "row shorter than width * 4");
    CHECK(hv_dispbuf_put(b, px, HV_DISP_MAX_DIM + 1, 8, (HV_DISP_MAX_DIM + 1) * 4) == HV_DISP_REJECTED, "width over the limit");
    CHECK(hv_dispbuf_put(b, px, 8, HV_DISP_MAX_DIM + 1, 32) == HV_DISP_REJECTED, "height over the limit");
    CHECK(hv_dispbuf_put(b, px, 64, 64, 0xffffffffu) == HV_DISP_REJECTED, "absurd row length");
    CHECK(hv_dispbuf_put(b, NULL, 64, 64, 256) == HV_DISP_REJECTED, "NULL pixels");
    uint32_t w, h; CHECK(hv_dispbuf_size(b, &w, &h) == 0 && w == 64 && h == 64, "the good frame is still held");
    HvDispStats st; hv_dispbuf_stats(b, &st);
    CHECK(st.rejected == 6, "rejections counted: %llu", (unsigned long long)st.rejected);
    hv_dispbuf_destroy(b);
}

typedef struct { const uint8_t *px; uint32_t w, h, stride; int calls; int have; } Fetch;
static int fetch_fn(void *ctx, const uint8_t **p, uint32_t *w, uint32_t *h, uint32_t *s) {
    Fetch *f = ctx; f->calls++;
    if (!f->have) return -1;
    *p = f->px; *w = f->w; *h = f->h; *s = f->stride; return 0;
}

static void test_pull(void) {
    group("display buffer: pull, close");
    HvDispBuf *b = hv_dispbuf_create();
    static uint8_t px[32 * 16 * 4];
    Fetch f = {px, 32, 16, 128, 0, 0};
    CHECK(hv_dispbuf_pull(b, fetch_fn, &f) == HV_DISP_NONE && f.calls == 1, "no frame yet");
    f.have = 1;
    CHECK(hv_dispbuf_pull(b, fetch_fn, &f) == HV_DISP_NEW && f.calls == 2, "frame");
    f.w = 0;
    CHECK(hv_dispbuf_pull(b, fetch_fn, &f) == HV_DISP_REJECTED, "a bad size from the machine is refused");
    CHECK(hv_dispbuf_pull(b, NULL, &f) == HV_DISP_REJECTED, "no callback");
    CHECK(!hv_dispbuf_is_closed(b), "open");
    hv_dispbuf_close(b); hv_dispbuf_close(b);
    CHECK(hv_dispbuf_is_closed(b), "closed (twice is fine)");
    int calls = f.calls; f.w = 32;
    CHECK(hv_dispbuf_pull(b, fetch_fn, &f) == HV_DISP_CLOSED && f.calls == calls, "closed: the callback is not called");
    CHECK(hv_dispbuf_put(b, px, 32, 16, 128) == HV_DISP_CLOSED, "closed: put refused");
    uint64_t seen = 0; uint8_t out[32 * 16 * 4];
    CHECK(hv_dispbuf_blit(b, &seen, out, 32, 32, 16) == 1, "the last picture can still be shown after close");
    HvDispStats st; hv_dispbuf_stats(b, &st);
    CHECK(st.closed_refused == 2, "refusals counted %llu", (unsigned long long)st.closed_refused);
    hv_dispbuf_destroy(b);
}

/* close() must wait for a pull that is inside the callback */
typedef struct { Fetch f; atomic_int in_fetch, release, close_done; HvDispBuf *b; } Slow;
static int slow_fetch(void *ctx, const uint8_t **p, uint32_t *w, uint32_t *h, uint32_t *s) {
    Slow *sl = ctx;
    atomic_store(&sl->in_fetch, 1);
    for (int i = 0; i < 2000 && !atomic_load(&sl->release); i++) usleep(1000);
    *p = sl->f.px; *w = sl->f.w; *h = sl->f.h; *s = sl->f.stride; return 0;
}
static void *slow_puller(void *a) { Slow *sl = a; hv_dispbuf_pull(sl->b, slow_fetch, sl); return NULL; }
static void *closer(void *a) { Slow *sl = a; hv_dispbuf_close(sl->b); atomic_store(&sl->close_done, 1); return NULL; }

static void test_close_waits(void) {
    group("display buffer: close waits for a copy in progress");
    static uint8_t px[32 * 16 * 4];
    Slow sl; memset(&sl, 0, sizeof sl);
    sl.f = (Fetch){px, 32, 16, 128, 0, 1};
    sl.b = hv_dispbuf_create();
    pthread_t p, c;
    pthread_create(&p, NULL, slow_puller, &sl);
    for (int i = 0; i < 2000 && !atomic_load(&sl.in_fetch); i++) usleep(1000);
    CHECK(atomic_load(&sl.in_fetch), "the pull reached the callback");
    pthread_create(&c, NULL, closer, &sl);
    usleep(150 * 1000);
    CHECK(!atomic_load(&sl.close_done), "close has NOT returned while the callback still holds the frame pointer");
    atomic_store(&sl.release, 1);
    pthread_join(p, NULL); pthread_join(c, NULL);
    CHECK(atomic_load(&sl.close_done), "close returned after the callback finished");
    HvDispStats st; hv_dispbuf_stats(sl.b, &st);
    CHECK(st.frames == 1, "the frame being copied was completed, not dropped half way (frames=%llu)", (unsigned long long)st.frames);
    hv_dispbuf_destroy(sl.b);
}

/* The machine's memory is freed right after close returns. A puller that is still running must never touch it (ASan catches a use after free). */
typedef struct { HvDispBuf *b; uint8_t *mem; atomic_int stop; atomic_long pulls, closed_seen; } Machine;
static int mach_fetch(void *ctx, const uint8_t **p, uint32_t *w, uint32_t *h, uint32_t *s) {
    Machine *m = ctx; *p = m->mem; *w = 64; *h = 32; *s = 256; return 0;
}
static void *mach_puller(void *a) {
    Machine *m = a;
    while (!atomic_load(&m->stop)) {
        int r = hv_dispbuf_pull(m->b, mach_fetch, m);
        atomic_fetch_add(&m->pulls, 1);
        if (r == HV_DISP_CLOSED) { atomic_fetch_add(&m->closed_seen, 1); usleep(200); }
    }
    return NULL;
}
static void *mach_blitter(void *a) {
    Machine *m = a; uint64_t seen = 0; static uint8_t out[64 * 32 * 4];
    while (!atomic_load(&m->stop)) { hv_dispbuf_blit(m->b, &seen, out, 64, 64, 32); usleep(100); }
    return NULL;
}
static void test_free_after_close(void) {
    group("display buffer: the machine's memory is freed right after close");
    for (int round = 0; round < 20; round++) {
        Machine m; memset(&m, 0, sizeof m);
        m.b = hv_dispbuf_create();
        m.mem = malloc(256 * 32);
        memset(m.mem, round, 256 * 32);
        pthread_t p[2], bl;
        pthread_create(&p[0], NULL, mach_puller, &m); pthread_create(&p[1], NULL, mach_puller, &m); pthread_create(&bl, NULL, mach_blitter, &m);
        usleep(3000 + round * 300);
        hv_dispbuf_close(m.b);   /* 1. close */
        free(m.mem);             /* 2. then the machine's memory goes */
        m.mem = NULL;            /* a puller that got past close would now fetch NULL pixels or freed memory */
        usleep(2000);
        atomic_store(&m.stop, 1);
        pthread_join(p[0], NULL); pthread_join(p[1], NULL); pthread_join(bl, NULL);
        if (round == 0) CHECK(atomic_load(&m.closed_seen) > 0, "pullers saw the close");
        hv_dispbuf_destroy(m.b);
    }
    CHECK(1, "20 rounds without touching freed memory");
}

static void test_pattern(void) {
    group("synthetic picture");
    static uint8_t bgra[640 * 360 * 4], rgbx[640 * 360 * 4];
    uint32_t frames[] = {0, 1, 2, 255, 256, 12345, 65535};
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        hv_testpattern(bgra, 640, 360, 640 * 4, frames[i], -1, -1);
        hv_disp_bgra_to_rgbx(bgra, 640, 360, 640 * 4, rgbx, 640, 640, 360);
        int r = hv_testpattern_frame_rgbx(rgbx, 640, 640, 360);
        CHECK(r == (int)(frames[i] & 0xffff), "frame %u read back as %d", frames[i], r);
    }
    /* colour bars: the first bar is white, the second yellow (R G 255 255 0) after conversion */
    hv_testpattern(bgra, 640, 360, 640 * 4, 0, -1, -1);
    hv_disp_bgra_to_rgbx(bgra, 640, 360, 640 * 4, rgbx, 640, 640, 360);
    const uint8_t *y = rgbx + ((size_t)100 * 640 + 120) * 4;
    CHECK(y[0] == 255 && y[1] == 255 && y[2] == 0, "second bar is yellow in RGB: %u %u %u", y[0], y[1], y[2]);
    /* the pointer cross at the corners and outside must stay inside the buffer (ASan) */
    int pts[][2] = {{0, 0}, {639, 359}, {5, 355}, {700, 400}, {-5, 10}, {320, 180}};
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) hv_testpattern(bgra, 640, 360, 640 * 4, 3, pts[i][0], pts[i][1]);
    hv_disp_bgra_to_rgbx(bgra, 640, 360, 640 * 4, rgbx, 640, 640, 360);
    const uint8_t *c = rgbx + ((size_t)180 * 640 + 320) * 4;
    CHECK(c[0] == 255 && c[1] == 255 && c[2] == 255, "the cross is drawn at its centre");
    uint8_t tiny[16 * 16 * 4]; hv_testpattern(tiny, 16, 16, 64, 1, 0, 0);
    CHECK(hv_testpattern_frame_rgbx(rgbx, 640, 64, 32) == -1, "reading a frame number from a picture that is too small gives -1");
}

/* ======================= C0: key map ======================= */

static void test_keymap(void) {
    group("key map");
    int a; uint16_t l; int n = 0;
    int seen_a[1024] = {0}, seen_l[256] = {0};
    for (int i = 0; hv_keymap_entry(i, &a, &l) == 0; i++) {
        n++;
        CHECK(l != 0 && l < 256, "entry %d has a Linux code (android %d -> %u)", i, a, l);
        CHECK(a > 0 && a < 1024, "entry %d android code %d in range", i, a);
        if (a > 0 && a < 1024) { CHECK(!seen_a[a], "android code %d appears twice", a); seen_a[a] = 1; }
        if (l < 256) { CHECK(!seen_l[l], "Linux code %u is the target of two Android keys", l); seen_l[l] = 1; }
        CHECK(hv_keymap_android(a) == l, "lookup of android %d matches the table", a);
    }
    CHECK(n == 85, "entries: %d (the table changed: update this number and the check in android keycheck.c)", n);
    struct { int a; uint16_t l; const char *what; } c[] = {
        {29, 30, "A"}, {54, 44, "Z"}, {7, 11, "0"}, {8, 2, "1"}, {16, 10, "9"}, {66, 28, "Enter"}, {67, 14, "Backspace"}, {62, 57, "Space"}, {111, 1, "Esc"},
        {142, 88, "F12"}, {131, 59, "F1"}, {140, 68, "F10"}, {141, 87, "F11"}, {117, 125, "Left Meta"}, {59, 42, "Left Shift"}, {113, 29, "Left Ctrl"}, {57, 56, "Left Alt"},
        {19, 103, "Up"}, {20, 108, "Down"}, {21, 105, "Left"}, {22, 106, "Right"}, {61, 15, "Tab"}, {69, 12, "Minus"}, {45, 16, "Q (QWERTY row)"}, {41, 50, "M"},
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) CHECK(hv_keymap_android(c[i].a) == c[i].l, "%s: android %d -> %u, want %u", c[i].what, c[i].a, hv_keymap_android(c[i].a), c[i].l);
    CHECK(hv_keymap_android(0) == 0 && hv_keymap_android(1000) == 0 && hv_keymap_android(-3) == 0 && hv_keymap_android(4) == 0, "unmapped keys give 0 (4 = BACK is not forwarded)");
    CHECK(hv_keymap_entry(-1, &a, &l) == -1 && hv_keymap_entry(100000, &a, &l) == -1, "entry bounds");
}

/* ======================= C0: touch ======================= */

typedef struct { int type, a, b; } Ev;      /* 'm' move dx dy, 'b' button n down, 'w' wheel, 'h' hwheel, 'k' key */
static Ev ev[4096]; static int nev;
static void s_move(int dx, int dy) { if (nev < 4096) ev[nev++] = (Ev){'m', dx, dy}; }
static void s_btn(int b, int d) { if (nev < 4096) ev[nev++] = (Ev){'b', b, d}; }
static void s_whl(int n) { if (nev < 4096) ev[nev++] = (Ev){'w', n, 0}; }
static void s_hwl(int n) { if (nev < 4096) ev[nev++] = (Ev){'h', n, 0}; }
static void s_key(uint16_t c, int d) { if (nev < 4096) ev[nev++] = (Ev){'k', c, d}; }
static const HvInputSink SINK = {s_key, s_move, s_btn, s_whl, s_hwl};
static int count(int type, int a, int b) { int n = 0; for (int i = 0; i < nev; i++) if (ev[i].type == type && (a < -1000 || ev[i].a == a) && (b < -1000 || ev[i].b == b)) n++; return n; }
#define ANY -9999
static int sum_of(int type) { int n = 0; for (int i = 0; i < nev; i++) if (ev[i].type == type) n += ev[i].a; return n; }
static int first_idx(int type, int a, int b) { for (int i = 0; i < nev; i++) if (ev[i].type == type && (a == ANY || ev[i].a == a) && (b == ANY || ev[i].b == b)) return i; return -1; }

static void fresh(HvTouch *t) { nev = 0; hv_touch_init(t, &SINK); hv_touch_set_view(t, 0, 0, 1000, 500); hv_touch_set_display(t, 2000, 1000); }

static void test_touch_basic(void) {
    group("touch: tap, long press, move");
    HvTouch t;
    /* a tap with no pointer feedback: homing, dead reckoning, then the click once the pointer is there */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 500, 250, 0);
    hv_touch_tick(&t, 1);
    long sx = 0, sy = 0; for (int i = 0; i < nev; i++) if (ev[i].type == 'm') { sx += ev[i].a; sy += ev[i].b; }
    CHECK(ev[0].type == 'm' && ev[0].a == -2000 && ev[0].b == -1000, "the pointer is homed to the corner first: %d %d", ev[0].a, ev[0].b);
    CHECK(sx == -2000 + 1000 && sy == -1000 + 500, "dead reckoning ends at the finger (sum %ld %ld)", sx, sy);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 500, 250, 100);
    hv_touch_tick(&t, 101);
    CHECK(count('b', 0, 1) == 1 && count('b', 0, 0) == 1, "one left click");
    CHECK(first_idx('b', 0, 1) > first_idx('m', ANY, ANY), "the click comes after the moves");
    CHECK(count('b', 1, ANY) == 0, "no right click");
    /* a long press is a right click, and the lift afterwards is not also a left click */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 200, 100, 0);
    for (int ms = 1; ms < 480; ms += 16) hv_touch_tick(&t, ms);
    CHECK(count('b', 1, ANY) == 0, "not yet");
    hv_touch_tick(&t, 520);
    CHECK(count('b', 1, 1) == 1 && count('b', 1, 0) == 1, "right click at 500 ms");
    hv_touch_tick(&t, 600);
    CHECK(count('b', 1, 1) == 1, "only once");
    hv_touch_event(&t, HV_TOUCH_UP, 0, 200, 100, 700); hv_touch_tick(&t, 701);
    CHECK(count('b', 0, ANY) == 0, "no left click after a long press");
    /* a slow tap (held over the tap time, under the long press) is nothing */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 200, 100, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 200, 100, 400); hv_touch_tick(&t, 401);
    CHECK(count('b', ANY, ANY) == 0, "a touch of 400 ms is neither tap nor long press");
    /* moving past the slop: the pointer follows, nothing is clicked */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 100, 100, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_MOVE, 0, 300, 200, 30); hv_touch_tick(&t, 31);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 300, 200, 60); hv_touch_tick(&t, 61);
    sx = 0; sy = 0; for (int i = 0; i < nev; i++) if (ev[i].type == 'm') { sx += ev[i].a; sy += ev[i].b; }
    CHECK(sx == -2000 + 600, "the pointer followed to guest x=600 (sum %ld)", sx);
    CHECK(sy == -1000 + 400, "and to guest y=400 (sum %ld)", sy);
    CHECK(count('b', ANY, ANY) == 0, "a drag of the finger is not a click");
    /* a tiny wobble inside the slop is still a tap */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 100, 100, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_MOVE, 0, 106, 104, 20);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 106, 104, 80); hv_touch_tick(&t, 81); hv_touch_tick(&t, 100);
    CHECK(count('b', 0, 1) == 1, "a wobble inside the slop is a tap");
    /* mapping with an offset content rectangle and clamping */
    fresh(&t);
    hv_touch_set_view(&t, 100, 50, 800, 400);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 100, 50, 0); hv_touch_tick(&t, 1);
    sx = 0; sy = 0; for (int i = 0; i < nev; i++) if (ev[i].type == 'm') { sx += ev[i].a; sy += ev[i].b; }
    CHECK(sx == -2000 && sy == -1000, "the top-left of the content is guest (0,0): sum %ld %ld", sx, sy);
    hv_touch_event(&t, HV_TOUCH_MOVE, 0, 5000, 5000, 20); hv_touch_tick(&t, 21);
    sx = 0; sy = 0; for (int i = 0; i < nev; i++) if (ev[i].type == 'm') { sx += ev[i].a; sy += ev[i].b; }
    CHECK(sx == -2000 + 1999 && sy == -1000 + 999, "a touch far outside is clamped to the last guest pixel: sum %ld %ld", sx, sy);
}

static void test_touch_drag_scroll(void) {
    group("touch: drag, scroll, two-finger tap, cancel");
    HvTouch t;
    /* tap then touch-and-hold: left button down, drag, release */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 300, 200, 80); hv_touch_tick(&t, 81);
    CHECK(count('b', 0, 1) == 1 && count('b', 0, 0) == 1, "the first tap clicks");
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 310, 205, 200);
    CHECK(count('b', 0, 1) == 2 && count('b', 0, 0) == 1, "touching again quickly presses the left button and keeps it down");
    hv_touch_event(&t, HV_TOUCH_MOVE, 0, 600, 300, 300); hv_touch_tick(&t, 301);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 600, 300, 400);
    CHECK(count('b', 0, 0) == 2, "lifting releases it");
    hv_touch_tick(&t, 420);
    CHECK(count('b', 0, 1) == 2, "no extra click from the lift");
    /* too late for a drag: two plain clicks */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 300, 200, 80); hv_touch_tick(&t, 81);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 600); hv_touch_tick(&t, 601);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 300, 200, 680); hv_touch_tick(&t, 681);
    CHECK(count('b', 0, 1) == 2 && count('b', 0, 0) == 2, "two separate clicks");
    int order_ok = 1, held = 0; for (int i = 0; i < nev; i++) if (ev[i].type == 'b') { held += ev[i].b ? 1 : -1; if (held < 0 || held > 1) order_ok = 0; }
    CHECK(order_ok && held == 0, "press and release alternate");
    /* tap far from the first: not a drag */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 100, 100, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 100, 100, 50); hv_touch_tick(&t, 51);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 800, 400, 150);
    CHECK(count('b', 0, 1) == 1, "a second touch far away is not a drag");
    /* two-finger scroll */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_DOWN, 1, 400, 200, 10);
    for (int s = 1; s <= 9; s++) {
        hv_touch_event(&t, HV_TOUCH_MOVE, 0, 300, 200 + s * 10, 20 + s * 10);
        hv_touch_event(&t, HV_TOUCH_MOVE, 1, 400, 200 + s * 10, 20 + s * 10);
        hv_touch_tick(&t, 20 + s * 10);
    }
    CHECK(sum_of('w') == 3 && count('w', ANY, ANY) == 3, "90 px down = 3 notches away from the user, one per 30 px (sum %d, %d events)", sum_of('w'), count('w', ANY, ANY));
    for (int s = 1; s <= 6; s++) {
        hv_touch_event(&t, HV_TOUCH_MOVE, 0, 300 - s * 10, 290, 200 + s * 10);
        hv_touch_event(&t, HV_TOUCH_MOVE, 1, 400 - s * 10, 290, 200 + s * 10);
    }
    CHECK(sum_of('h') == 2, "60 px to the left = 2 horizontal notches, positive = the view moves right (sum %d)", sum_of('h'));
    hv_touch_event(&t, HV_TOUCH_UP, 1, 340, 290, 400); hv_touch_event(&t, HV_TOUCH_UP, 0, 240, 290, 410); hv_touch_tick(&t, 420);
    CHECK(count('b', ANY, ANY) == 0, "a scroll is not a click");
    /* upward */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 300, 0); hv_touch_event(&t, HV_TOUCH_DOWN, 1, 400, 300, 5);
    for (int s = 1; s <= 6; s++) { hv_touch_event(&t, HV_TOUCH_MOVE, 0, 300, 300 - s * 10, 10 + s * 10); hv_touch_event(&t, HV_TOUCH_MOVE, 1, 400, 300 - s * 10, 10 + s * 10); }
    CHECK(sum_of('w') == -2, "60 px up = two notches toward the user (sum %d)", sum_of('w'));
    /* two-finger tap */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_DOWN, 1, 400, 200, 20);
    hv_touch_event(&t, HV_TOUCH_UP, 1, 400, 200, 120); hv_touch_event(&t, HV_TOUCH_UP, 0, 300, 200, 130); hv_touch_tick(&t, 131);
    CHECK(count('b', 1, 1) == 1 && count('b', 1, 0) == 1 && count('b', 0, ANY) == 0, "a two-finger tap is one right click");
    /* a finger left behind after a two-finger gesture does not click or move the pointer */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_tick(&t, 1);
    hv_touch_event(&t, HV_TOUCH_DOWN, 1, 400, 200, 10);
    for (int s = 1; s <= 4; s++) { hv_touch_event(&t, HV_TOUCH_MOVE, 0, 300, 200 + s * 20, 10 + s * 10); hv_touch_event(&t, HV_TOUCH_MOVE, 1, 400, 200 + s * 20, 10 + s * 10); }
    hv_touch_event(&t, HV_TOUCH_UP, 1, 400, 280, 100);
    int moves_before = count('m', ANY, ANY);
    hv_touch_event(&t, HV_TOUCH_MOVE, 0, 700, 400, 120); hv_touch_tick(&t, 121);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 700, 400, 150); hv_touch_tick(&t, 151);
    CHECK(count('m', ANY, ANY) == moves_before && count('b', ANY, ANY) == 0, "the remaining finger is ignored until all are up");
    /* a third finger is ignored */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_event(&t, HV_TOUCH_DOWN, 1, 400, 200, 5); hv_touch_event(&t, HV_TOUCH_DOWN, 2, 500, 200, 8);
    hv_touch_event(&t, HV_TOUCH_UP, 2, 500, 200, 9);
    CHECK(count('b', ANY, ANY) == 0 && count('w', ANY, ANY) == 0, "a third finger does nothing");
    /* cancel during a drag releases the button; release_all too */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_tick(&t, 1); hv_touch_event(&t, HV_TOUCH_UP, 0, 300, 200, 50); hv_touch_tick(&t, 51);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 305, 200, 150);
    CHECK(count('b', 0, 1) == 2 && count('b', 0, 0) == 1, "drag button is down");
    hv_touch_event(&t, HV_TOUCH_CANCEL, 0, 0, 0, 200);
    CHECK(count('b', 0, 0) == 2, "a cancel releases the held button");
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 300, 200, 0); hv_touch_tick(&t, 1); hv_touch_event(&t, HV_TOUCH_UP, 0, 300, 200, 50); hv_touch_tick(&t, 51);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 305, 200, 150);
    hv_touch_release_all(&t); hv_touch_release_all(&t);
    CHECK(count('b', 0, 0) == 2 && count('b', 0, 1) == 2, "release_all releases once");
    /* unknown ids and events with no finger */
    fresh(&t);
    hv_touch_event(&t, HV_TOUCH_UP, 7, 1, 1, 0); hv_touch_event(&t, HV_TOUCH_MOVE, 7, 1, 1, 1); hv_touch_tick(&t, 2);
    CHECK(nev == 0, "events for a finger that is not down do nothing");
}

/* a simulated guest pointer: integer moves, acceleration that grows with the size of the move, input latency */
static struct { int x, y, lat; struct { int due, dx, dy; } q[256]; int nq; int now; int dw, dh; } G;
static int accel_move(int d) { double a = 1.0 + (abs(d) / 40.0 > 1.5 ? 1.5 : abs(d) / 40.0); return (int)(d * a + (d < 0 ? -0.5 : 0.5)); }
static void g_move(int dx, int dy) { if (G.nq < 256) { G.q[G.nq].due = G.now + G.lat; G.q[G.nq].dx = dx; G.q[G.nq].dy = dy; G.nq++; } }
static void g_apply(void) {
    int k = 0;
    for (int i = 0; i < G.nq; i++) {
        if (G.q[i].due <= G.now) {
            G.x += accel_move(G.q[i].dx); G.y += accel_move(G.q[i].dy);
            G.x = G.x < 0 ? 0 : G.x > G.dw - 1 ? G.dw - 1 : G.x;
            G.y = G.y < 0 ? 0 : G.y > G.dh - 1 ? G.dh - 1 : G.y;
        } else G.q[k++] = G.q[i];
    }
    G.nq = k;
}
static int g_buttons;
static void g_btn(int b, int d) { (void)b; g_buttons += d ? 1 : 0; s_btn(b, d); }

static void test_touch_loop(void) {
    group("touch: closed loop against a simulated accelerated pointer");
    struct { int sx, sy; float fx, fy; } cases[] = { {100, 100, 995, 497}, {1900, 1000, 3, 2}, {1000, 500, 1000, 40}, {50, 900, 400, 450}, {960, 540, 970, 448}, {0, 0, 999, 499} };
    for (int lat = 0; lat <= 3; lat += 3) {
        for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            HvTouch t; nev = 0;
            HvInputSink sk = SINK; sk.mouse_move = g_move; sk.mouse_button = g_btn;
            hv_touch_init(&t, &sk); hv_touch_set_view(&t, 0, 0, 1000, 500); hv_touch_set_display(&t, 2000, 1000);
            memset(&G, 0, sizeof G); G.x = cases[c].sx; G.y = cases[c].sy; G.dw = 2000; G.dh = 1000; G.lat = lat; g_buttons = 0;
            hv_touch_event(&t, HV_TOUCH_DOWN, 0, cases[c].fx, cases[c].fy, 0);
            double tx = cases[c].fx / 1000.0 * 1999, ty = cases[c].fy / 500.0 * 999;
            int reached = -1, flips = 0, lastsgn = 0; double worst = 0;
            for (int k = 1; k <= 100; k++) {
                G.now = k; g_apply();
                hv_touch_pointer(&t, 1, G.x, G.y);
                hv_touch_tick(&t, (int64_t)k * 16);
                double ex = tx - G.x, ey = ty - G.y, d = fabs(ex) + fabs(ey);
                if (reached < 0 && fabs(ex) <= 2 && fabs(ey) <= 2) reached = k;
                if (reached >= 0 && d > worst) worst = d;
                int sg = ex > 1.5 ? 1 : ex < -1.5 ? -1 : 0;
                if (sg && lastsgn && sg != lastsgn) flips++;
                if (sg) lastsgn = sg;
            }
            CHECK(reached > 0 && reached <= 60, "case %zu lat %d: reached the target in %d ticks", c, lat, reached);
            CHECK(fabs(tx - G.x) <= 2 && fabs(ty - G.y) <= 2, "case %zu lat %d: rests at (%d,%d), target (%.0f,%.0f)", c, lat, G.x, G.y, tx, ty);
            CHECK(flips <= 4, "case %zu lat %d: %d direction flips (no ringing)", c, lat, flips);
            CHECK(worst <= 4, "case %zu lat %d: after arriving it stays within %.0f px", c, lat, worst);
        }
    }
    /* a tap on feedback: the click waits for the pointer, then happens exactly once */
    for (int lat = 0; lat <= 3; lat += 3) {
        HvTouch t; nev = 0;
        HvInputSink sk = SINK; sk.mouse_move = g_move; sk.mouse_button = g_btn;
        hv_touch_init(&t, &sk); hv_touch_set_view(&t, 0, 0, 1000, 500); hv_touch_set_display(&t, 2000, 1000);
        memset(&G, 0, sizeof G); G.x = 100; G.y = 100; G.dw = 2000; G.dh = 1000; G.lat = lat; g_buttons = 0;
        hv_touch_event(&t, HV_TOUCH_DOWN, 0, 900, 400, 0);
        int click_tick = -1, at_x = 0, at_y = 0;
        for (int k = 1; k <= 60; k++) {
            G.now = k; g_apply(); hv_touch_pointer(&t, 1, G.x, G.y);
            if (k == 2) hv_touch_event(&t, HV_TOUCH_UP, 0, 900, 400, 60);   /* the lift is early: the pointer is still far away */
            int before = count('b', 0, 1);
            hv_touch_tick(&t, (int64_t)k * 16);
            if (count('b', 0, 1) > before && click_tick < 0) { click_tick = k; at_x = G.x; at_y = G.y; }
        }
        double tx = 900 / 1000.0 * 1999, ty = 400 / 500.0 * 999;
        CHECK(click_tick > 2, "lat %d: the click did not fire at the lift (tick %d)", lat, click_tick);
        CHECK((fabs(tx - at_x) <= 3 && fabs(ty - at_y) <= 3) || click_tick * 16 >= 60 + HV_TOUCH_CLICK_WAIT_MS, "lat %d: it fired with the pointer at (%d,%d), target (%.0f,%.0f)", lat, at_x, at_y, tx, ty);
        CHECK(count('b', 0, 1) == 1 && count('b', 0, 0) == 1, "lat %d: exactly one click", lat);
    }
    /* a pointer that never arrives (feedback frozen): the click still happens after the wait, and only once */
    {
        HvTouch t; nev = 0;
        hv_touch_init(&t, &SINK); hv_touch_set_view(&t, 0, 0, 1000, 500); hv_touch_set_display(&t, 2000, 1000);
        hv_touch_event(&t, HV_TOUCH_DOWN, 0, 900, 400, 0);
        for (int k = 1; k <= 80; k++) {
            hv_touch_pointer(&t, 1, 100, 100);   /* never changes */
            if (k == 3) hv_touch_event(&t, HV_TOUCH_UP, 0, 900, 400, 50);
            hv_touch_tick(&t, (int64_t)k * 16);
        }
        CHECK(count('b', 0, 1) == 1, "a frozen pointer: the click is sent after the wait, once (%d)", count('b', 0, 1));
        CHECK(count('m', ANY, ANY) <= 60, "and the moves stay bounded (%d)", count('m', ANY, ANY));
    }
    /* the guest stops reporting a pointer: falls back to dead reckoning and homes once */
    {
        HvTouch t; nev = 0;
        hv_touch_init(&t, &SINK); hv_touch_set_view(&t, 0, 0, 1000, 500); hv_touch_set_display(&t, 2000, 1000);
        hv_touch_pointer(&t, 1, 500, 500);
        hv_touch_event(&t, HV_TOUCH_DOWN, 0, 500, 250, 0); hv_touch_tick(&t, 1);
        hv_touch_pointer(&t, 0, 0, 0);
        hv_touch_event(&t, HV_TOUCH_MOVE, 0, 600, 250, 40); hv_touch_tick(&t, 41); hv_touch_tick(&t, 60);
        CHECK(count('m', -2000, -1000) == 1, "homing happens once when the pointer becomes unknown");
    }
}

int main(void) {
    test_convert(); test_buffer_basic(); test_rejects(); test_pull(); test_close_waits(); test_free_after_close(); test_pattern();
    test_keymap();
    test_touch_basic(); test_touch_drag_scroll(); test_touch_loop();
    printf("c0b0: %d check(s), %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
