#include "display_buf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct HvDispBuf {
    pthread_mutex_t mu;
    int closed;
    uint8_t *px;       /* BGRA, tightly packed (width * 4 per row) */
    size_t cap;
    uint32_t w, h;
    uint64_t seq;      /* number of the frame held; 0 = none yet */
    HvDispStats st;
};

HvDispBuf *hv_dispbuf_create(void) {
    HvDispBuf *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    pthread_mutex_init(&b->mu, NULL);
    return b;
}

void hv_dispbuf_destroy(HvDispBuf *b) {
    if (!b) return;
    pthread_mutex_destroy(&b->mu);
    free(b->px);
    free(b);
}

static int geometry_ok(uint32_t w, uint32_t h, uint32_t stride) {
    if (w == 0 || h == 0 || w > HV_DISP_MAX_DIM || h > HV_DISP_MAX_DIM) return 0;
    if ((uint64_t)stride < (uint64_t)w * 4) return 0;
    if ((uint64_t)stride * h > (uint64_t)HV_DISP_MAX_DIM * HV_DISP_MAX_DIM * 4 * 2) return 0; /* a stride far beyond the width is not a frame of ours */
    return 1;
}

/* Caller holds the lock. Copies the rows into the buffer's own memory. */
static int store(HvDispBuf *b, const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride) {
    size_t need = (size_t)w * h * 4;
    if (need > b->cap) {
        uint8_t *n = realloc(b->px, need);
        if (!n) { b->st.rejected++; return HV_DISP_REJECTED; }
        b->px = n; b->cap = need;
    }
    for (uint32_t y = 0; y < h; y++) memcpy(b->px + (size_t)y * w * 4, src + (size_t)y * stride, (size_t)w * 4);
    b->w = w; b->h = h; b->seq++;
    b->st.frames++;
    return HV_DISP_NEW;
}

int hv_dispbuf_pull(HvDispBuf *b, HvFrameFetch fetch, void *ctx) {
    if (!b || !fetch) return HV_DISP_REJECTED;
    pthread_mutex_lock(&b->mu);
    if (b->closed) { b->st.closed_refused++; pthread_mutex_unlock(&b->mu); return HV_DISP_CLOSED; }
    const uint8_t *src = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    int r;
    if (fetch(ctx, &src, &w, &h, &stride) != 0 || !src) { b->st.none++; r = HV_DISP_NONE; }
    else if (!geometry_ok(w, h, stride)) { b->st.rejected++; r = HV_DISP_REJECTED; }
    else r = store(b, src, w, h, stride); /* src is not touched after this point */
    pthread_mutex_unlock(&b->mu);
    return r;
}

int hv_dispbuf_put(HvDispBuf *b, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride) {
    if (!b || !bgra) return HV_DISP_REJECTED;
    pthread_mutex_lock(&b->mu);
    int r;
    if (b->closed) { b->st.closed_refused++; r = HV_DISP_CLOSED; }
    else if (!geometry_ok(w, h, stride)) { b->st.rejected++; r = HV_DISP_REJECTED; }
    else r = store(b, bgra, w, h, stride);
    pthread_mutex_unlock(&b->mu);
    return r;
}

int hv_dispbuf_size(HvDispBuf *b, uint32_t *w, uint32_t *h) {
    if (!b) return -1;
    pthread_mutex_lock(&b->mu);
    int r = b->seq ? 0 : -1;
    if (!r) { if (w) *w = b->w; if (h) *h = b->h; }
    pthread_mutex_unlock(&b->mu);
    return r;
}

size_t hv_disp_bgra_to_rgbx(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t sstride, uint8_t *dst, uint32_t dstride_px, uint32_t dw, uint32_t dh) {
    if (!src || !dst || !sw || !sh || !dw || !dh || (uint64_t)sstride < (uint64_t)sw * 4 || dstride_px < dw) return 0;
    uint32_t w = sw < dw ? sw : dw, h = sh < dh ? sh : dh;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * sstride;
        uint8_t *d = dst + (size_t)y * dstride_px * 4;
        for (uint32_t x = 0; x < w; x++, s += 4, d += 4) {
            uint8_t bb = s[0], gg = s[1], rr = s[2];   /* alpha is dropped: the guest's alpha byte of a desktop picture is not meaningful */
            d[0] = rr; d[1] = gg; d[2] = bb; d[3] = 0xff;
        }
    }
    return (size_t)w * h;
}

int hv_dispbuf_blit(HvDispBuf *b, uint64_t *seen, uint8_t *dst, uint32_t dst_stride_px, uint32_t dst_w, uint32_t dst_h) {
    if (!b || !seen || !dst) return -1;
    pthread_mutex_lock(&b->mu);
    int r;
    if (!b->seq) r = -1;
    else if (b->seq == *seen) r = 0;
    else {
        r = hv_disp_bgra_to_rgbx(b->px, b->w, b->h, b->w * 4, dst, dst_stride_px, dst_w, dst_h) ? 1 : -1;
        if (r == 1) { *seen = b->seq; b->st.blits++; }
    }
    pthread_mutex_unlock(&b->mu);
    return r;
}

void hv_dispbuf_close(HvDispBuf *b) {
    if (!b) return;
    pthread_mutex_lock(&b->mu);   /* waits for a pull in progress */
    b->closed = 1;
    pthread_mutex_unlock(&b->mu);
}

int hv_dispbuf_is_closed(HvDispBuf *b) {
    if (!b) return 1;
    pthread_mutex_lock(&b->mu);
    int c = b->closed;
    pthread_mutex_unlock(&b->mu);
    return c;
}

void hv_dispbuf_stats(HvDispBuf *b, HvDispStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!b) return;
    pthread_mutex_lock(&b->mu);
    *out = b->st;
    pthread_mutex_unlock(&b->mu);
}

/* ---- the synthetic picture ---- */

static void put_px(uint8_t *row, uint32_t x, uint8_t r, uint8_t g, uint8_t bl) { row[x * 4] = bl; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 0xff; }

void hv_testpattern(uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride, uint32_t frame, int cx, int cy) {
    if (!bgra || w < 128 || h < 64 || stride < w * 4) return;
    static const uint8_t bar[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{32,32,32}};
    uint32_t bars_end = h * 2 / 3;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = bgra + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            if (y < bars_end) { const uint8_t *c = bar[x * 8 / w]; put_px(row, x, c[0], c[1], c[2]); }
            else { uint8_t v = (uint8_t)(x * 255 / (w - 1)); put_px(row, x, v, v, v); }
        }
    }
    /* the moving square along the lower third */
    uint32_t sq = 32, span = w - sq, sx = (uint32_t)((frame * 7u) % (span ? span : 1)), sy = bars_end + (h - bars_end > sq ? (h - bars_end - sq) / 2 : 0);
    for (uint32_t y = sy; y < sy + sq && y < h; y++) for (uint32_t x = sx; x < sx + sq && x < w; x++) put_px(bgra + (size_t)y * stride, x, 255, 255, 255);
    /* 16 cells, 8 x 8, at the top left: the frame number, white = 1 */
    for (uint32_t bit = 0; bit < 16; bit++) {
        uint8_t v = (frame >> bit) & 1 ? 255 : 0;
        for (uint32_t y = 0; y < 8; y++) for (uint32_t x = bit * 8; x < bit * 8 + 8; x++) put_px(bgra + (size_t)y * stride, x, v, v, v);
    }
    /* the simulated pointer: a cross */
    if (cx >= 0 && cy >= 0)
        for (int d = -10; d <= 10; d++) {
            int px = cx + d, py = cy + d;
            if (px >= 0 && (uint32_t)px < w && cy >= 0 && (uint32_t)cy < h) put_px(bgra + (size_t)cy * stride, (uint32_t)px, 255, 255, 255);
            if (py >= 0 && (uint32_t)py < h && cx >= 0 && (uint32_t)cx < w) put_px(bgra + (size_t)py * stride, (uint32_t)cx, 255, 255, 255);
        }
}

int hv_testpattern_frame_rgbx(const uint8_t *rgbx, uint32_t stride_px, uint32_t w, uint32_t h) {
    if (!rgbx || w < 128 || h < 64) return -1;
    int frame = 0;
    for (uint32_t bit = 0; bit < 16; bit++) {
        const uint8_t *p = rgbx + ((size_t)4 * stride_px) * 4 + (size_t)(bit * 8 + 4) * 4; /* the middle of the cell: row 4, column 4 */
        if (p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255) frame |= 1 << bit;
        else if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255)) return -1;
    }
    return frame;
}
