#include "vm.h"
#include "pvg_lite.h"
#include "g7_fbprobe.h"
#include "g7_fb0.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <os/lock.h>
#include <pthread/qos.h>
#include <sys/ucontext.h>
#else
#include "platform.h" /* Linux, Android: stand-ins for the Apple lock and reference types */
#endif
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>

#include "usb_host.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "audio_mac.h"
#include "dispatch.h"
#include "translate.h" /* hv_translate_err_name (integ7b engine-fatal record) */
#include "net_slirp.h"

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS]; /* the board takes them as an array */
static int ncpus = 1;
static HvVmapple board;
/* UI display changes may arrive before or during the background machine creation. */
static os_unfair_lock display_lock = OS_UNFAIR_LOCK_INIT;
static CFTypeRef display_layer;
static int display_layer_set;
static _Atomic(HvPvgLite *) display_pvg; /* published only when the board is ready */
/* The pointer-moved doorbell as a wake-up (hv_vm_cursor_wake_open). The listener runs on the vCPU that wrote the doorbell: one atomic exchange and, at most once per
   drain, a one-byte write to a non-blocking pipe. Nothing else. */
static _Atomic int cursor_wake_fd = -1, cursor_wake_rd = -1, cursor_wake_pending;
static _Atomic uint64_t cursor_wakes;
static void vm_cursor_listener(void *ctx, int shown, int x, int y) {
    (void)ctx; (void)shown; (void)x; (void)y;
    int fd = atomic_load_explicit(&cursor_wake_fd, memory_order_relaxed);
    if (fd < 0 || atomic_exchange(&cursor_wake_pending, 1)) return;
    char b = 1;
    if (write(fd, &b, 1) == 1) atomic_fetch_add_explicit(&cursor_wakes, 1, memory_order_relaxed);
    else atomic_store(&cursor_wake_pending, 0);   /* full or failed: the next doorbell tries again */
}
int hv_vm_cursor_stats(uint64_t *doorbells, uint64_t *changed, uint64_t *wakes, int *last_x, int *last_y, int *last_flag) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    HvPvgCursorStats s;
    hv_pvg_lite_cursor_stats(pvg, &s);
    if (doorbells) *doorbells = s.doorbells;
    if (changed) *changed = s.changed;
    if (wakes) *wakes = atomic_load_explicit(&cursor_wakes, memory_order_relaxed);
    if (last_x) *last_x = s.last_x;
    if (last_y) *last_y = s.last_y;
    if (last_flag) *last_flag = s.last_flag;
    return pvg && s.seen ? 1 : 0;
}
int hv_vm_cursor_wake_open(void) {
    if (atomic_load(&cursor_wake_rd) >= 0) return atomic_load(&cursor_wake_rd);
    int fds[2];
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) { fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK); fcntl(fds[i], F_SETFD, FD_CLOEXEC); }
    atomic_store(&cursor_wake_rd, fds[0]);
    atomic_store(&cursor_wake_fd, fds[1]);
    return fds[0];
}
void hv_vm_cursor_wake_drain(void) {
    int rd = atomic_load(&cursor_wake_rd);
    if (rd < 0) return;
    atomic_store(&cursor_wake_pending, 0);   /* first, so a doorbell after this point writes again; the caller asks the pointer after the drain */
    char buf[64];
    while (read(rd, buf, sizeof buf) > 0) {}
}
static HvDisk disk, aux;
static int net_on;
static void (*extra_poll)(void);
static void (*exit_cb)(int);

HvMachine *hv_vm_machine(void) { return &m; }
HvVmapple *hv_vm_board(void) { return &board; }
HvCpu *hv_vm_cpu(int index) { return index >= 0 && index < ncpus ? &cpus[index] : NULL; }
int hv_vm_ncpus(void) { return ncpus; }

/* arm64 boot_args: {u16 Revision, u16 Version, u64 virtBase, physBase, memSize,
   topOfKernelData, Video {baseAddr, display, rowBytes, width, height, depth}, ...}.
   Found by scanning page starts for one whose physBase is the RAM base. */
/* [2026-11-21, explicitly approved, D3] hv_vm_boot_display()'s second-priority source (after a real GPU
   frame, before the boot_args RAM scan): the guest's own last-swapped PVG surface, copied into a buffer this
   function owns and reuses across calls -- grown once, lazily, only the first time a surface too big for the
   current capacity actually shows up, never malloc/free per call. 1920x1080 BGRA (~8.3 MB) is the one real
   size seen on this device so far (builds 187/188), so that is the starting capacity; devices/pvg_lite.c's
   decode_surface() refuses anything over 8192x8192 either way. */
static uint8_t *pvg_live_buf;
static size_t pvg_live_cap;
static void frame_boot_args_first(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride);
static void frame_pvg_series(const char *kind, const uint8_t *px, uint32_t w, uint32_t h);
static uint32_t pvg_last_w, pvg_last_h;
static void g7_fbp_src(unsigned id, const char *word, uint32_t w, uint32_t h);

/* The guest's own boot framebuffer (boot_args Video), found by scanning RAM. Split out of hv_vm_boot_display() so the PVG-surface branch can fall back to it. 0 on success, -1 if there is none. */
static unsigned h0(const uint8_t *p, int i) { uint16_t v; memcpy(&v, p + 2 * i, 2); return v; }   /* a 16-bit header field of boot_args (revision, version) */
static int boot_args_display(HvBootDisplay *d) {
    static uint64_t found; /* RAM offset of boot_args */
    uint8_t *ram = board.ram;
    uint64_t size = board.ram_size;
    if (!ram) return -1;
    static double last_scan;
    for (int pass = 0; pass < 2; pass++) {
        if (!found || pass) {
            /* A full scan at most every 2 s; boot_args sits low in RAM
               (iBoot puts it near 0x78ed0000), so the first GB is enough. */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = ts.tv_sec + ts.tv_nsec * 1e-9;
            if (now - last_scan < 2) return -1;
            last_scan = now;
            if (pass && found) found = 0;
            uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
            for (uint64_t off = 0; off + 0x1000 <= limit && !found; off += 0x4000) {
                const uint64_t *q = (const uint64_t *)(ram + off);
                const uint16_t *h = (const uint16_t *)(ram + off);
                if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue; /* boot_args: revision, version */
                if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size || q[3] == 0 || q[3] > size) continue;
                uint64_t base = q[5], row = q[7], w = q[8], hh = q[9], depth = q[10];
                if (w < 320 || w > 8192 || hh < 200 || hh > 8192 || row < w * 4 || (depth & 0xff) != 32) continue;
                if (base < VMAPPLE_MEM || base - VMAPPLE_MEM + row * hh > size) continue;
                found = off;
            }
        }
        if (!found) return -1;
        const uint64_t *q = (const uint64_t *)(ram + found);
        {   /* once per boot_args found: the kernel's runtime virtual base, so a kernel address in the pc samples can be turned into a file address (slide = virtBase - the kernel file's __TEXT vmaddr) */
            static uint64_t logged;
            if (logged != found) { logged = found; fprintf(stderr, "hengevm: boot_args at ram+%#llx: virtBase=%#llx physBase=%#llx (kernel slide = virtBase minus the kernel file's __TEXT address; the file is not here)\n", (unsigned long long)found, (unsigned long long)q[1], (unsigned long long)q[2]);
                fprintf(stderr, "hengevm: boot_args video (log only; raw words, 8-byte slots 5..10 of boot_args): revision=%u version=%u baseAddr=%#llx display=%#llx rowBytes=%llu width=%llu height=%llu depth=%#llx (depth low byte = bits per pixel; width*4=%llu; baseAddr-RAM base=%#llx)\n",
                        (unsigned)h0(ram + found, 0), (unsigned)h0(ram + found, 1), (unsigned long long)q[5], (unsigned long long)q[6], (unsigned long long)q[7], (unsigned long long)q[8], (unsigned long long)q[9], (unsigned long long)q[10], (unsigned long long)q[8] * 4ull, (unsigned long long)(q[5] - VMAPPLE_MEM)); }
        }
        uint64_t base = q[5], row = q[7], w = q[8], hh = q[9];
        /* the same check the first search and g7_fb0 use (size, rowbytes >= width*4, 32-bit depth, base inside RAM): a boot_args page the guest rewrote must not be shown on a weaker test */
        if (q[1] >> 40 == 0xfffffe && strcmp(hv_fbp_video_check(q, VMAPPLE_MEM, size), "ok") == 0) {
            d->pixels = ram + (base - VMAPPLE_MEM);
            d->width = (uint32_t)w, d->height = (uint32_t)hh, d->stride = (uint32_t)row, d->source = HV_SCREEN_BOOT_ARGS;
            g7_fbp_src(5, "boot_args_used", d->width, d->height);
            frame_boot_args_first(d->pixels, d->width, d->height, d->stride);   /* file only, bounded, written by a worker thread */
            return 0;
        }
    }
    return -1;
}

/* True if every pixel's colour bytes are zero (alpha ignored: a black pixel may carry alpha 0xff). A surface the guest GPU never drew into stays all-zero. */
static int bgra_all_black(const uint8_t *px, uint32_t w, uint32_t h) {
    const uint64_t *q = (const uint64_t *)(const void *)px;   /* pvg_live_buf comes from malloc/realloc: 8-byte aligned */
    size_t n = (size_t)w * h / 2;                              /* two pixels per word */
    for (size_t i = 0; i < n; i++) if (q[i] & 0x00ffffff00ffffffull) return 0;
    if (((size_t)w * h) & 1) { uint32_t last = ((const uint32_t *)(const void *)px)[(size_t)w * h - 1]; if (last & 0x00ffffffu) return 0; }
    return 1;
}

/* DIAGNOSTIC (log only, no effect on what is shown): why the PVG surface counts as black or not. Once at the first surface and then about every 30 s of calls (the pump asks ~10x/s), it
   logs the surface size, how many pixels have a non-zero colour byte, the first such pixel and its value, and the largest colour byte. "boot-display-fallback-v2" also tells which build is running. */
static void display_diag(const uint8_t *px, uint32_t w, uint32_t h) {
    static unsigned calls;
    if (calls++ % 300) return;
    size_t n = (size_t)w * h, nz = 0, first = (size_t)-1;
    uint32_t firstv = 0, maxc = 0;
    const uint32_t *p = (const uint32_t *)(const void *)px;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = p[i] & 0x00ffffffu;
        if (!v) continue;
        if (nz++ == 0) { first = i; firstv = p[i]; }
        uint32_t m = v & 0xff; if (((v >> 8) & 0xff) > m) m = (v >> 8) & 0xff; if (((v >> 16) & 0xff) > m) m = (v >> 16) & 0xff;
        if (m > maxc) maxc = m;
    }
    fprintf(stderr, "hengevm: display: boot-display-fallback-v2 surface %ux%u nonzero_pixels=%zu first=%zd (x=%zu y=%zu) first_value=0x%08x max_colour_byte=%u\n",
            w, h, nz, first == (size_t)-1 ? (ssize_t)-1 : (ssize_t)first, w ? (first == (size_t)-1 ? 0 : first % w) : 0, w ? (first == (size_t)-1 ? 0 : first / w) : 0, firstv, maxc);
}

/* DIAGNOSTIC (files only, no effect on what is shown): full-resolution frames as uncompressed PNGs next to the GPU dump (the folder the start dialog passes in LAGFX_ARM64_DUMP_DIR;
   nothing is written when it is not set). Two kinds, named so they cannot be mixed up:
     g7_frame_boot_args_first_<W>x<H>.png        the boot_args framebuffer (what XNU's boot_args points at), saved the first time it is found: as early as the engine can see it
     g7_frame_pvg_surface_<n>_t<S>s_<W>x<H>.png  the guest's PVG surface (hv_pvg_lite_live_surface_bgra), n = 0 first frame, 1..3 the first frame seen at or after +10 s, +30 s, +60 s after the first
                                                  one, and ..._last_... = the newest one when the runner stops (hv_vm_save_last_frame). (g7_frame_pvg_* for a real GPU frame, which Android never has.)
   BOUNDS: at most FRAME_MAX_FILES files and FRAME_MAX_BYTES of PNG in a run, no frame over FRAME_MAX_PX pixels, at most FRAME_MAX_INFLIGHT writers at a time.
   THREADS: the caller (the runner's frame pump, never a vCPU or the GPU thread) only copies the pixels (a memcpy, a few ms, a handful of times per run); the PNG encoding and the file write run on a
   short-lived worker thread. Stored deflate blocks, so no compression library is needed; written as .part and renamed, so a killed process never leaves a half file under the final name. */
/* FRAME-FILES-BEGIN (tools/g7-frames-test.sh compiles exactly this region on its own) */
#define FRAME_MAX_FILES 11   /* 6 before the boot-fb probe added two (first change, ~10 s), 8 before the g7-fb0 early observation added up to three */
#define FRAME_MAX_PX (4u << 20)
#define FRAME_MAX_BYTES (80u << 20)
#define FRAME_MAX_INFLIGHT 2
static uint32_t png_crc_table[256];
static void png_crc_init(void) {
    if (png_crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; png_crc_table[n] = c; }
}
static uint32_t png_crc(uint32_t crc, const uint8_t *b, size_t n) { crc = ~crc; while (n--) crc = png_crc_table[(crc ^ *b++) & 0xff] ^ (crc >> 8); return ~crc; }
static void png_be32(uint8_t *o, uint32_t v) { o[0] = (uint8_t)(v >> 24), o[1] = (uint8_t)(v >> 16), o[2] = (uint8_t)(v >> 8), o[3] = (uint8_t)v; }
static int png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8]; png_be32(h, n); memcpy(h + 4, type, 4);
    uint32_t c = png_crc(0, h + 4, 4); c = png_crc(c, data, n);
    uint8_t t[4]; png_be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4;
}
/* Writes bgra (w x h, tightly packed) as an RGB PNG with stored deflate blocks, streaming: no second copy of the picture is made. 1 ok. */
static int png_write_stored(FILE *f, const uint8_t *bgra, uint32_t w, uint32_t h) {
    const size_t rowlen = 1 + (size_t)w * 3, raw_n = (size_t)h * rowlen, nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
    uint8_t *blk = malloc(65535);
    if (!blk) return 0;
    png_crc_init();
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    uint8_t ihdr[13]; png_be32(ihdr, w); png_be32(ihdr + 4, h); ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    int ok = fwrite(sig, 1, 8, f) == 8 && png_chunk(f, "IHDR", ihdr, 13);
    uint8_t hd[8]; png_be32(hd, (uint32_t)zn); memcpy(hd + 4, "IDAT", 4);
    uint32_t crc = png_crc(0, hd + 4, 4), a = 1, b = 0;
    ok = ok && fwrite(hd, 1, 8, f) == 8;
#define EMIT(p, n) do { if (ok) { ok = fwrite((p), 1, (n), f) == (n); crc = png_crc(crc, (p), (n)); } } while (0)
    { static const uint8_t zh[2] = { 0x78, 0x01 }; EMIT(zh, 2); }
    size_t y = 0, c = 0, x = 0; int ch = 0;   /* the cursor into the raw scanlines: row, column in the raw row (0 = filter byte), pixel, colour channel */
    for (size_t off = 0; off < raw_n && ok; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        uint8_t bh[5] = { off + n >= raw_n ? 1 : 0, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8) };
        EMIT(bh, 5);
        for (size_t i = 0; i < n; i++) {
            uint8_t v;
            if (c == 0) { v = 0; c = 1; x = 0; ch = 0; }
            else { v = bgra[(y * w + x) * 4 + (size_t)(2 - ch)]; if (++ch == 3) { ch = 0; x++; } if (++c == rowlen) { c = 0; y++; } }
            blk[i] = v; a = (a + v) % 65521u; b = (b + a) % 65521u;
        }
        EMIT(blk, n);
    }
    uint8_t ad[4]; png_be32(ad, (b << 16) | a); EMIT(ad, 4);
#undef EMIT
    uint8_t t[4]; png_be32(t, crc);
    ok = ok && fwrite(t, 1, 4, f) == 4 && png_chunk(f, "IEND", NULL, 0);
    free(blk);
    return ok;
}

typedef struct { char path[640], label[96]; uint8_t *bgra; uint32_t w, h; size_t est; } FrameJob;
static _Atomic int frame_inflight;
static _Atomic unsigned frame_files;
static _Atomic uint64_t frame_bytes;
static void *frame_worker(void *arg) {
    FrameJob *j = arg;
    char part[sizeof j->path + 8]; snprintf(part, sizeof part, "%s.part", j->path);
    FILE *f = fopen(part, "wb");
    int ok = 0;
    if (f) { ok = png_write_stored(f, j->bgra, j->w, j->h); ok = (fclose(f) == 0) && ok; }
    if (ok) ok = rename(part, j->path) == 0;
    if (!ok) remove(part);
    fprintf(stderr, "hengevm: display: frame file %s src=%ux%u %s %s\n", j->label, j->w, j->h, ok ? "saved" : "FAILED", j->path);
    free(j->bgra); free(j);
    atomic_fetch_sub(&frame_inflight, 1);
    return NULL;
}
/* Starts one save. Returns 1 started, 0 not now (the writers are busy: the caller may try again), -1 never (no folder, or a bound reached: counted as spent by the caller). stride in bytes. */
static int frame_save_async(const char *tag, const char *label, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride) {
    const char *dir = getenv("LAGFX_ARM64_DUMP_DIR");
    if (!dir || !*dir || !px || !w || !h || (uint64_t)w * h > FRAME_MAX_PX || stride < w * 4u) return -1;
    size_t est = (size_t)h * (1 + (size_t)w * 3) + 200;
    if (atomic_load(&frame_files) >= FRAME_MAX_FILES || atomic_load(&frame_bytes) + est > FRAME_MAX_BYTES) { fprintf(stderr, "hengevm: display: frame file %s src=%ux%u NOT saved (bound: %u files / %u MiB per run)\n", label, w, h, FRAME_MAX_FILES, FRAME_MAX_BYTES >> 20); return -1; }
    if (atomic_load(&frame_inflight) >= FRAME_MAX_INFLIGHT) return 0;
    FrameJob *j = calloc(1, sizeof *j);
    uint8_t *copy = malloc((size_t)w * h * 4);
    if (!j || !copy) { free(j); free(copy); return 0; }
    for (uint32_t y = 0; y < h; y++) memcpy(copy + (size_t)y * w * 4, px + (size_t)y * stride, (size_t)w * 4);
    j->bgra = copy; j->w = w; j->h = h; j->est = est;
    snprintf(j->label, sizeof j->label, "%s", label);
    snprintf(j->path, sizeof j->path, "%s/g7_frame_%s_%ux%u.png", dir, tag, w, h);
    atomic_fetch_add(&frame_inflight, 1);
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &at, frame_worker, j);
    pthread_attr_destroy(&at);
    if (rc != 0) { atomic_fetch_sub(&frame_inflight, 1); free(copy); free(j); return 0; }
    atomic_fetch_add(&frame_files, 1); atomic_fetch_add(&frame_bytes, est);
    return 1;
}
/* Waits (at most about 10 s) until every started save has finished: a process that exits first would lose the file being written. */
void hv_vm_frames_flush(void) {
    for (int i = 0; i < 1000 && atomic_load(&frame_inflight) > 0; i++) usleep(10000);
}

/* FRAME-FILES-END */
static double frame_mono(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
/* The boot_args framebuffer, once: the first time it is found (the logo may not be drawn yet; the PVG series below is the later evidence). */
static void frame_boot_args_first(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride) {
    static int done;
    if (done) return;
    int r = frame_save_async("boot_args_first", "boot_args(first)", px, w, h, stride);
    if (r != 0) done = 1;   /* 0 = writers busy: try again at the next call */
}
/* The PVG surface time series: first frame, then the first frame at or after +10 s, +30 s, +60 s. */
static double pvg_t0; static unsigned pvg_next;
static const double pvg_at[4] = { 0, 10, 30, 60 };
static void frame_pvg_series(const char *kind, const uint8_t *px, uint32_t w, uint32_t h) {
    double t = frame_mono();
    if (pvg_t0 == 0) pvg_t0 = t;
    if (pvg_next >= 4 || t - pvg_t0 < pvg_at[pvg_next]) return;
    char tag[96], label[96]; unsigned n = pvg_next;
    snprintf(tag, sizeof tag, "%s_%u_t%us", kind, n, (unsigned)(t - pvg_t0));
    snprintf(label, sizeof label, "%s(%u, +%.1fs)", kind, n, t - pvg_t0);
    int r = frame_save_async(tag, label, px, w, h, w * 4u);
    if (r != 0) pvg_next++;
}
static HvFbpState fbp;
static int fbp_png_first_pending, fbp_png_10s_pending, fbp_caps_said, fbp_final_done;
static const uint8_t *fbp_last_px; static uint32_t fbp_last_w, fbp_last_h, fbp_last_row;   /* the last valid boot framebuffer (a pointer into guest RAM, never freed) */
/* The newest PVG surface the pump read, saved as "..._last_..." (the runner calls it after it closed the display, before it stops the machine). Nothing if no PVG surface was ever read or no folder is set. */
void hv_vm_save_last_frame(void) {
    if (!fbp_final_done && fbp_last_px) {   /* g7-boot-fb kind=final: the state at VM stop (read-only) */
        char line[1024]; fbp_final_done = 1;
        if (hv_fbp_final(&fbp, frame_mono(), fbp_last_px, fbp_last_w, fbp_last_h, fbp_last_row, line, sizeof line)) fprintf(stderr, "%s\n", line);
    }
    if (!pvg_t0 || !pvg_live_buf || !pvg_last_w || !pvg_last_h) return;
    char tag[96], label[96]; double t = frame_mono() - pvg_t0;
    snprintf(tag, sizeof tag, "pvg_surface_last_t%us", (unsigned)t);
    snprintf(label, sizeof label, "pvg_surface(last, +%.1fs)", t);
    (void)frame_save_async(tag, label, pvg_live_buf, pvg_last_w, pvg_last_h, pvg_last_w * 4u);
}


/* DIAGNOSTIC (log lines "g7-boot-fb:" + at most two PNGs, no effect on what the guest or the display sees): where the boot framebuffer lives and whether anything is drawn into it.
   Line formats and caps: host/g7_fbprobe.h. It has its OWN relaxed scan of guest RAM (header + virtBase/physBase/memSize only, no Video checks) so that boot_args whose Video fields are zero or odd are still
   reported (the display path's boot_args_display() would skip them silently). Only reads guest RAM, from the display pump thread, at most once per 2 s (the scan: the first GiB at 16 KiB steps, as the display scan). */
static void g7_fbp_say(const char *line) { fprintf(stderr, "%s\n", line); }
static void g7_fbp_src(unsigned id, const char *word, uint32_t w, uint32_t h) {
    char line[160];
    if (hv_fbp_src(&fbp, frame_mono(), id, word, w, h, line, sizeof line)) g7_fbp_say(line);
}
static void g7_boot_fb_tick(void) {
    static uint64_t found; static int have; static double last_scan; static uint64_t vid[6]; static int bl;
    uint8_t *ram = board.ram; uint64_t size = board.ram_size;
    if (!ram) return;
    double now = frame_mono();
    if (!have) {
        if (last_scan != 0 && now - last_scan < HV_FBP_PERIOD_S) return;
        last_scan = now;
        uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
        for (uint64_t off = 0; off + 0x1000 <= limit && !have; off += 0x4000) {
            const uint64_t *q = (const uint64_t *)(ram + off); const uint16_t *h = (const uint16_t *)(ram + off);
            if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue;
            if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size || q[3] == 0 || q[3] > size) continue;
            found = off; have = 1;
        }
        if (!have) {
            if (fbp.scan_lines < HV_FBP_MAX_SCAN_LINES) { fbp.scan_lines++; if (fbp.t0 == 0) fbp.t0 = now; fprintf(stderr, "g7-boot-fb: scan none t=%.1fs\n", now - fbp.t0); }
            return;
        }
    }
    const uint64_t *q = (const uint64_t *)(ram + found);
    if (bl < 4 && (bl == 0 || memcmp(vid, q + 5, sizeof vid) != 0)) {   /* once, and again if the Video words change (at most 4 lines) */
        char line[512]; memcpy(vid, q + 5, sizeof vid); bl++;
        hv_fbp_boot_args_line(line, sizeof line, found, (const uint16_t *)(ram + found), q, VMAPPLE_MEM, size);
        g7_fbp_say(line);
    }
    {   /* log only: the kernel command line and device-tree words of that page (at most 4 lines: once, again if the text changes) */
        static uint64_t cl_hash; static int cl_lines;
        uint64_t hsh = 1469598103934665603ull;
        for (unsigned i = 0; i < 1132u; i++) hsh = (hsh ^ ram[found + i]) * 1099511628211ull;
        if (cl_lines < 4 && hsh != cl_hash && size - found >= 0x1000) {
            char cl[900]; cl_hash = hsh; cl_lines++;
            hv_fbp_cmdline_line(cl, sizeof cl, ram + found, 0x1000);
            g7_fbp_say(cl);
        }
    }
    uint64_t base = q[5], row = q[7], w = q[8], hh = q[9];
    if (strcmp(hv_fbp_video_check(q, VMAPPLE_MEM, size), "ok") != 0 || row > 0xffffffffu) return;
    const uint8_t *px = ram + (base - VMAPPLE_MEM);
    char line[1024]; int chg = 0, t10 = 0;
    fbp_last_px = px; fbp_last_w = (uint32_t)w; fbp_last_h = (uint32_t)hh; fbp_last_row = (uint32_t)row;
    if (hv_fbp_tick(&fbp, now, px, (uint32_t)w, (uint32_t)hh, (uint32_t)row, line, sizeof line, &chg, &t10)) {
        if (!fbp_caps_said) {   /* the PNG bounds are stated in the first line */
            fbp_caps_said = 1;
            const char *dir = getenv("LAGFX_ARM64_DUMP_DIR");
            size_t n = strlen(line);
            snprintf(line + n, sizeof line - n, " png: dump_dir=%s boot_fb_pngs=2(first change, ~10s) caps: max_files=%u(all frame kinds) max_bytes=%uMiB max_px=%u", dir && *dir ? "set" : "NOT set (no PNG)", FRAME_MAX_FILES, FRAME_MAX_BYTES >> 20, FRAME_MAX_PX);
        }
        g7_fbp_say(line);
        if (chg) fbp_png_first_pending = 1;
        if (t10) fbp_png_10s_pending = 1;
    }
    if (fbp_png_first_pending && frame_save_async("boot_fb_first_change", "boot_fb(first change)", px, (uint32_t)w, (uint32_t)hh, (uint32_t)row) != 0) fbp_png_first_pending = 0;
    if (fbp_png_10s_pending && frame_save_async("boot_fb_t10s", "boot_fb(~10s)", px, (uint32_t)w, (uint32_t)hh, (uint32_t)row) != 0) fbp_png_10s_pending = 0;
}

/* F1 (research/g7-hjit2): early boot-framebuffer observation, log lines "g7-fb0:" (format: host/g7_fb0.h) plus at most 3 dumps. Runs ONLY when the environment says G7_FB0=1 (the g7exec start dialog sets it; every other
   runner has no such thread). A detached thread of its own (never a vCPU, never the display pump): from the VM start, every HV_FB0_PERIOD_MS for HV_FB0_SECONDS, then it ends. It only READS guest RAM: the boot_args
   Video words when a boot_args page has appeared (same relaxed scan as g7_boot_fb_tick, once per second until found), else the fixed carveout window at HV_FB0_DEFAULT_BASE. No lock is taken, nothing is written to the guest. */
static void g7_fb0_dump(int n, int why, double t, int src, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, uint64_t winbytes) {
    const char *dir = getenv("LAGFX_ARM64_DUMP_DIR");
    char tag[64]; snprintf(tag, sizeof tag, "fb0_%d_%s_t%ums", n, why == 1 ? "first_nz" : "big_change", (unsigned)(t * 1000));
    if (!dir || !*dir) { fprintf(stderr, "g7-fb0: dump %s NOT saved (no dump folder: LAGFX_ARM64_DUMP_DIR is not set; enable the shader/pipeline dump on the start dialog)\n", tag); return; }
    if (src == 2) {
        int rc = 0; for (int k = 0; k < 5 && (rc = frame_save_async(tag, tag, px, w, h, stride)) == 0; k++) usleep(40000);
        fprintf(stderr, "g7-fb0: dump %s png %s\n", tag, rc == 1 ? "started" : "NOT saved (bound or no memory)");
        return;
    }
    size_t len = winbytes < HV_FB0_RAW_DUMP_MAX ? (size_t)winbytes : HV_FB0_RAW_DUMP_MAX;
    uint8_t *copy = malloc(len); char path[600], part[620];
    if (!copy) return;
    memcpy(copy, px, len);
    snprintf(path, sizeof path, "%s/g7_%s_raw%zuB.bin", dir, tag, len); snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "wb"); int ok = f && fwrite(copy, 1, len, f) == len; if (f && fclose(f) != 0) ok = 0;
    if (ok) ok = rename(part, path) == 0; else if (f) remove(part);
    fprintf(stderr, "g7-fb0: dump %s raw %zu bytes %s\n", tag, len, ok ? "saved" : "NOT saved");
    free(copy);
}
static void *g7_fb0_thread(void *unused) {
    (void)unused;
    HvFb0State st; hv_fb0_init(&st);
    char line[700]; hv_fb0_start_line(line, sizeof line); fprintf(stderr, "%s\n", line);
    double t0 = frame_mono(), total = 0, mx = 0, last_scan = -10;
    uint64_t found = 0; int have = 0, said_bargs = 0, said_nowin = 0;
    for (;;) {
        double t = frame_mono() - t0;
        if (t > HV_FB0_SECONDS) break;
        uint8_t *ram = board.ram; uint64_t size = board.ram_size;
        if (ram) {
            if (!have && t - last_scan >= 1.0) {
                last_scan = t;
                uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
                for (uint64_t off = 0; off + 0x1000 <= limit && !have; off += 0x4000) {
                    const uint64_t *q = (const uint64_t *)(ram + off); const uint16_t *h = (const uint16_t *)(ram + off);
                    if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue;
                    if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size || q[3] == 0 || q[3] > size) continue;
                    if (strcmp(hv_fbp_video_check(q, VMAPPLE_MEM, size), "ok") != 0) continue;
                    found = off; have = 1;
                }
            }
            int src = 1; uint64_t base = HV_FB0_DEFAULT_BASE; uint32_t w = 0, h = 0, row = 0;
            if (have) {
                const uint64_t *q = (const uint64_t *)(ram + found);
                if (strcmp(hv_fbp_video_check(q, VMAPPLE_MEM, size), "ok") == 0 && q[7] <= 0xffffffffull) { src = 2; base = q[5]; w = (uint32_t)q[8]; h = (uint32_t)q[9]; row = (uint32_t)q[7]; }
            }
            uint64_t winbytes = src == 2 ? (uint64_t)row * h : HV_FB0_DEFAULT_WINDOW;
            if (base < VMAPPLE_MEM || base - VMAPPLE_MEM >= size) winbytes = 0;
            else if (base - VMAPPLE_MEM + winbytes > size) winbytes = size - (base - VMAPPLE_MEM);
            if (winbytes < 4096) { if (!said_nowin) { said_nowin = 1; fprintf(stderr, "g7-fb0: t=%.2fs no window (base %#llx is outside guest RAM %#llx+%#llx)\n", t, (unsigned long long)base, (unsigned long long)VMAPPLE_MEM, (unsigned long long)size); } }
            else {
                if (src == 2 && !said_bargs) { said_bargs = 1; fprintf(stderr, "g7-fb0: boot_args t=%.2fs base=%#llx w=%u h=%u row=%u\n", t, (unsigned long long)base, w, h, row); }
                const uint8_t *px = ram + (base - VMAPPLE_MEM);
                HvFb0Sample s; double c0 = frame_mono();
                if (src == 2) hv_fb0_scan(px, w, h, row, &s); else hv_fb0_scan(px, (uint32_t)(winbytes / 4), 1, (uint32_t)winbytes, &s);
                double ms = (frame_mono() - c0) * 1000; total += ms; if (ms > mx) mx = ms;
                int dump = 0;
                if (hv_fb0_step(&st, t, &s, src, base, w, h, line, sizeof line, &dump)) fprintf(stderr, "%s\n", line);
                if (dump) g7_fb0_dump(st.dumps, dump, t, src, px, w, h, row, winbytes);
            }
        }
        usleep(HV_FB0_PERIOD_MS * 1000);
    }
    hv_fb0_end_line(&st, frame_mono() - t0, total, mx, line, sizeof line); fprintf(stderr, "%s\n", line);
    return NULL;
}
static void g7_fb0_maybe_start(void) {
    const char *e = getenv("G7_FB0");
    if (!e || strcmp(e, "1") != 0) return;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, g7_fb0_thread, NULL) != 0) fprintf(stderr, "g7-fb0: sampler thread did not start (diagnostic only; the VM is unaffected)\n");
    pthread_attr_destroy(&at);
}

int hv_vm_boot_display(HvBootDisplay *d) {
    /* What the guest's GPU has drawn, once it has drawn anything. */
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg) return -1;
    g7_boot_fb_tick();   /* log only */
    const uint8_t *frame;
    uint32_t fw, fh;
    if (hv_pvg_lite_frame(pvg, &frame, &fw, &fh) == 0) {
        g7_fbp_src(0, "pvg_frame", fw, fh);
        frame_pvg_series("pvg", frame, fw, fh);
        d->pixels = (uint8_t *)frame;
        d->width = fw, d->height = fh, d->stride = fw * 4, d->source = HV_SCREEN_PVG;
        return 0;
    }
    /* No real GPU frame (certainly true on Android: host/pvg_metal_stub.c's hv_pvg_metal_frame() always
       returns -1 there) -- try the surface the guest itself last swapped before falling back to the raw
       boot_args scan below. Read-only and bounded exactly like that scan; see hv_pvg_lite_live_surface_bgra()'s
       own comment for the locking/ownership contract that makes this safe to call continuously while the
       guest keeps running. */
    if (!pvg_live_cap) {
        pvg_live_cap = 1920u * 1080u * 4u;
        pvg_live_buf = malloc(pvg_live_cap);
        if (!pvg_live_buf) pvg_live_cap = 0;
    }
    int surf_black = 0;
    uint32_t surf_w = 0, surf_h = 0;
    if (pvg_live_buf) {
        uint32_t sw, sh;
        size_t need = 0;
        int rc = hv_pvg_lite_live_surface_bgra(pvg, pvg_live_buf, pvg_live_cap, &sw, &sh, &need);
        if (rc != 0 && need > pvg_live_cap) {
            uint8_t *grown = realloc(pvg_live_buf, need);
            if (grown) {
                pvg_live_buf = grown, pvg_live_cap = need;
                rc = hv_pvg_lite_live_surface_bgra(pvg, pvg_live_buf, pvg_live_cap, &sw, &sh, NULL);
            }
        }
        if (rc != 0) g7_fbp_src(4, "pvg_surface_none", 0, 0);
        if (rc == 0) {
            g7_fbp_src(1, "pvg_surface_read", sw, sh);
            display_diag(pvg_live_buf, sw, sh);
            pvg_last_w = sw; pvg_last_h = sh;
            frame_pvg_series("pvg_surface", pvg_live_buf, sw, sh);
            if (!bgra_all_black(pvg_live_buf, sw, sh)) {
                g7_fbp_src(2, "pvg_surface_nonblack", sw, sh);
                d->pixels = pvg_live_buf;
                d->width = sw, d->height = sh, d->stride = sw * 4, d->source = HV_SCREEN_PVG_SURFACE;
                return 0;
            }
            g7_fbp_src(3, "pvg_surface_black", sw, sh);
            surf_black = 1, surf_w = sw, surf_h = sh;
        }
    }
    /* The surface exists but is all black: the guest's GPU work was never executed (Android has no backend), so it would hide the boot framebuffer (the Apple logo) for good. Prefer the boot framebuffer, keep the surface as the last resort. */
    if (surf_black) {
        static int said;
        if (!said) { said = 1; fprintf(stderr, "hengevm: display: PVG surface is all black; showing the boot framebuffer while it stays black\n"); }
        if (boot_args_display(d) == 0) return 0;
        d->pixels = pvg_live_buf;
        d->width = surf_w, d->height = surf_h, d->stride = surf_w * 4, d->source = HV_SCREEN_PVG_SURFACE;
        return 0;
    }
    return boot_args_display(d);
}
/* See the doc comment in vm.h: just reads back the `source` hv_vm_boot_display() itself already filled in,
   rather than re-deriving it from the pixel pointer -- a pointer-range check alone cannot any longer tell
   the GPU frame and the PVG-surface copy apart (host/vm.c's new pvg_live_buf), since neither lives in
   board.ram. */
int hv_vm_boot_display_source(const HvBootDisplay *d) {
    if (!d || !d->pixels || !d->width || !d->height || !d->stride) return HV_SCREEN_NONE;
    return d->source;
}
void hv_vm_set_poll(void (*poll)(void)) { extra_poll = poll; }

/* psci_halt_valid: 0 none, 1 a writer has claimed the slot and may still be filling it (never safe to
   read), 2 complete and published with a RELEASE store (core/exception.c's psci()) -- so this ACQUIRE
   load, on seeing 2, is guaranteed to see every field below too, even from a thread other than the one
   that wrote them (core/machine.h's comment on the field has the cross-CPU reason this matters). */
int hv_vm_engine_fatal(HvVmEngineFatal *out) {
    HvEngineFatal f;
    if (!hv_engfatal_get(&m.engine_fatal, &f)) return 0;
    if (out) {
        out->cpu = f.cpu, out->flags = f.flags, out->gen = f.gen, out->flushes = f.flushes, out->rc = f.rc, out->va = f.va, out->pa = f.pa;
        out->cause_name = f.rc ? hv_translate_err_name(f.rc) : "no block and nothing pending";
    }
    return 1;
}

int hv_vm_psci_halt_info(uint32_t *fn, uint32_t *cpu_index, uint64_t *pc, uint64_t *lr) {
    if (__atomic_load_n(&m.psci_halt_valid, __ATOMIC_ACQUIRE) != 2) return 0;
    if (fn) *fn = m.psci_halt_fn;
    if (cpu_index) *cpu_index = m.psci_halt_cpu;
    if (pc) *pc = m.psci_halt_pc;
    if (lr) *lr = m.psci_halt_lr;
    return 1;
}

int hv_vm_psci_halt_stack(uint64_t *sp, uint64_t *x26, uint64_t *stack, int *n_words) {
    if (__atomic_load_n(&m.psci_halt_valid, __ATOMIC_ACQUIRE) != 2) return 0;
    if (sp) *sp = m.psci_halt_sp;
    if (x26) *x26 = m.psci_halt_x26;
    if (n_words) *n_words = m.psci_halt_stack_words;
    if (stack) for (int i = 0; i < m.psci_halt_stack_words; i++) stack[i] = m.psci_halt_stack[i];
    return 1;
}

/* [C1-4] Android panic-entry probe (host/ui_runner_vm.c): same gate, same ACQUIRE/RELEASE pair, as the two
   accessors above -- see core/machine.h's panic_hits/panic_first/panic_last comment for what this does and does
   not cover (PSCI halts only; 0 hits is a real, reportable result, not a failure). */
int hv_vm_panic_probe(uint32_t *hits, HvPanicHit *first, HvPanicHit *last) {
    if (__atomic_load_n(&m.psci_halt_valid, __ATOMIC_ACQUIRE) != 2) return 0;
    if (hits) *hits = m.panic_hits;
    if (first) *first = m.panic_first;
    if (last) *last = m.panic_last;
    return 1;
}

const char *hv_vm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported";
    case HV_EXIT_FAULT_LOOP: return "fault on the exception vector";
    default: return "other";
    }
}

/* Host-side device work on CPU 0's thread each tick. */
static void board_poll(void) {
    if (extra_poll) extra_poll();
    if (net_on) hv_net_poll();
}

static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

/* A branch landing where no translation is (an illegal instruction, or a fault
   fetching one): say where, and which direct branches in translated code lead
   there, for a crash report cannot. */
#if defined(__APPLE__) /* the report reads Darwin's ucontext; a host crash report for Linux and Android is separate work */
static struct sigaction prior_ill, prior_bus, prior_segv;
static void on_ill(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, rx = (uint64_t)(uintptr_t)m.jit.base, size = (uint64_t)m.jit.capacity * 4;
    uint64_t used = (uint64_t)m.jit.used * 4;
    int in_rx = pc >= rx && pc < rx + size;
    int fetch = (uint64_t)(uintptr_t)info->si_addr == pc;
    if (sig == SIGILL || in_rx || fetch) {
        fprintf(stderr, "hengevm: signal %d in translated code: pc %llx (rx %llx size %llx used %llx, pc is rx%+lld), full flushes %llu, lr %llx, addr %p\n",
                sig, (unsigned long long)pc, (unsigned long long)rx, (unsigned long long)size, (unsigned long long)used, (long long)(pc - rx),
                (unsigned long long)m.stats.block_flushes, (unsigned long long)uc->uc_mcontext->__ss.__lr, info->si_addr);
        for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%llx%c", i, (unsigned long long)uc->uc_mcontext->__ss.__x[i], i % 6 == 5 ? '\n' : ' ');
        fprintf(stderr, "\n");
        fprintf(stderr, "sp=%llx ram %llx size %llx\n", (unsigned long long)uc->uc_mcontext->__ss.__sp, (unsigned long long)(uintptr_t)board.ram, (unsigned long long)board.ram_size);
        {
            uint64_t x7 = uc->uc_mcontext->__ss.__x[7], x28 = uc->uc_mcontext->__ss.__x[28];
            if (x7 >= x28 && x7 < x28 + sizeof(HvCpu)) {
                const uint64_t *ent = (const uint64_t *)(uintptr_t)(x7 + 0x500);
                fprintf(stderr, "tlb entry at +%llx:", (unsigned long long)(x7 - x28));
                for (int i = 0; i < 16; i++) fprintf(stderr, " %llx", (unsigned long long)ent[i]);
                fprintf(stderr, "\n");
            }
        }
        for (int i = 0; i < ncpus; i++)
            fprintf(stderr, "  cpu %d: guest pc %llx el%u miss site %llx\n", i, (unsigned long long)cpus[i].pc, cpus[i].el, (unsigned long long)cpus[i].miss_site);
        if (in_rx) { /* the code it ran up to here, as it stands */
            uint64_t from = pc - 0x300 < rx ? rx : pc - 0x300;
            fprintf(stderr, "  words from rx+%llx:", (unsigned long long)(from - rx));
            for (uint64_t at = from; at < pc + 0x600 && at < rx + size; at += 4) {
                if (((at - from) & 0x1f) == 0) fprintf(stderr, "\n   ");
                fprintf(stderr, " %08x", *(const uint32_t *)(uintptr_t)at);
            }
            fprintf(stderr, "\n");
        }
        { /* x15 after a chained exit's check: the literal it loaded. Which exits, and where do they go now? */
            uint64_t x15 = uc->uc_mcontext->__ss.__x[15];
            const uint32_t *w15 = (const uint32_t *)(uintptr_t)rx;
            uint64_t n15 = used / 4 < m.jit.capacity ? used / 4 : m.jit.capacity;
            int shown = 0;
            for (uint64_t k = 0; k + 8 < n15 && shown < 12; k++) {
                if ((w15[k] & 0xff00001fu) != 0x5800000fu) continue; /* ldr x15, literal */
                int64_t lo = (int64_t)(int32_t)(w15[k] << 8) >> 13;
                uint64_t at = k + (uint64_t)lo;
                if (at + 1 >= n15) continue;
                uint64_t lit = w15[at] | (uint64_t)w15[at + 1] << 32;
                if (lit != x15) continue;
                /* eor x6; orr; cbnz; B */
                uint32_t b = w15[k + 4];
                if ((b & 0xfc000000u) != 0x14000000u) continue;
                int64_t off = (int64_t)(b << 6) >> 6;
                fprintf(stderr, "  exit at rx+%llx: B word %08x to rx+%llx\n", (unsigned long long)(k + 4) * 4, b, (unsigned long long)((int64_t)(k + 4) + off) * 4);
                shown++;
            }
        }
        if (!in_rx || pc >= rx + used) {
            /* Who branches there? */
            const uint32_t *w = (const uint32_t *)(uintptr_t)rx;
            int found = 0;
            uint64_t words = used / 4 < m.jit.capacity ? used / 4 : m.jit.capacity;
            for (uint64_t k = 0; k < words && found < 8; k++) {
                uint32_t i = w[k];
                int64_t off;
                if ((i & 0x7c000000u) == 0x14000000u) off = (int64_t)(i << 6) >> 6;
                else if ((i & 0xff000010u) == 0x54000000u || (i & 0x7e000000u) == 0x34000000u) off = (int64_t)(int32_t)(i << 8) >> 13;
                else if ((i & 0x7e000000u) == 0x36000000u) off = (int64_t)(int32_t)(i << 13) >> 18;
                else continue;
                if (rx + k * 4 + (uint64_t)(off * 4) != pc && rx + k * 4 + (uint64_t)(off * 4) != pc - 4) continue;
                found++;
                fprintf(stderr, "  branch at rx+%llx (word %08x) leads there; around it:", (unsigned long long)k * 4, i);
                for (int64_t d = -6; d <= 3; d++)
                    if ((int64_t)k + d >= 0 && (uint64_t)((int64_t)k + d) < words) fprintf(stderr, " %08x", w[k + d]);
                fprintf(stderr, "\n");
            }
            if (!found) fprintf(stderr, "  no direct branch in %llu MB of code leads there\n", (unsigned long long)(used >> 20));
            /* Which slot or cache entry holds it? */
            for (uint32_t k = 0; k < m.max_slots; k++)
                if (m.slots && m.slots[k] == pc) fprintf(stderr, "  chain slot %u holds it\n", k);
        }
        fflush(stderr);
    }
    struct sigaction *prior = sig == SIGILL ? &prior_ill : sig == SIGBUS ? &prior_bus : &prior_segv;
    sigaction(sig, prior, NULL);
}
#endif

void (*hv_prof_register)(void);

void hv_vm_set_display_layer(void *layer) {
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&display_lock);
    CFTypeRef previous = display_layer;
    display_layer = retained;
    display_layer_set = 1;
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (pvg) hv_pvg_lite_set_layer(pvg, layer);
    os_unfair_lock_unlock(&display_lock);
    if (previous) CFRelease(previous);
}

size_t hv_vm_gfx_diag(char *out, size_t cap) {   /* diagnostic only: see hv_pvg_lite_chan_diag */
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (out && cap) out[0] = 0;
    return pvg ? hv_pvg_lite_chan_diag(pvg, out, cap) : 0;
}

uint64_t hv_vm_gpu_frames(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_gpu_frames(pvg) : 0;
}

int hv_vm_create(const HvVmConfig *c, char *err, size_t errlen) {
    if (!c->rom || !c->disk || !c->aux) return fail(err, errlen, "The ROM, disk and auxiliary storage are all needed.");
#if defined(__APPLE__)
    struct sigaction ill = {.sa_flags = SA_SIGINFO | SA_ONSTACK};
    ill.sa_sigaction = on_ill;
    sigaction(SIGILL, &ill, &prior_ill);
    sigaction(SIGBUS, &ill, &prior_bus);
    sigaction(SIGSEGV, &ill, &prior_segv);
#endif
    signal(SIGPIPE, SIG_IGN); /* a guest connection closing at the far end must not end the machine */
    if (hv_disk_open(&disk, c->disk, 0, c->readonly_storage ? 1 : 0) != 0) return fail(err, errlen, "Cannot open the disk image.");
    if (hv_disk_open(&aux, c->aux, c->aux_offset ? c->aux_offset : 16384, c->readonly_storage ? 1 : 0) != 0)
        return fail(err, errlen, "Cannot open the auxiliary storage.");
    size_t jit = c->jit_bytes ? c->jit_bytes : (size_t)768 << 20;
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot set up the JIT region.");
    m.log = c->log;
    m.gen_checks = getenv("HV_JIT_POISON") != NULL;
    if (getenv("HV_SLOW_MEM")) m.slow_mem = 1; /* debugging: every load and store through the helper */
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK"); /* debugging: one CPU in guest code at a time */
    HvCfgParams cfg = {.nr_cpus = (uint32_t)ncpus, .ecid = c->ecid, .seed = c->seed, .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01}};
    if (c->installer) cfg.run_installer1 = cfg.run_installer2 = 1;
    cfg.usb_on_normal_boot = c->usb_on_normal_boot; /* host/vm.h: diagnostic-only, does not touch run_installer1/2 */
    if (getenv("HV_RUN_INSTALLER")) { /* experiment: "a,b" for the two installer words */
        unsigned a = 0, b2 = 0;
        sscanf(getenv("HV_RUN_INSTALLER"), "%u,%u", &a, &b2);
        cfg.run_installer1 = a, cfg.run_installer2 = b2;
    }
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    HV_STAGE("machine_ready");
    if (hv_vmapple_create(&board, &m, cpus, (c->ram_mb ? c->ram_mb : 2048) << 20, &disk, &aux, &cfg, mac) != 0)
        return fail(err, errlen, "Cannot build the machine (not enough memory for the guest's RAM?).");
    HV_STAGE("board_created");
    if (hv_vmapple_load_rom(&board, c->rom) != 0) return fail(err, errlen, "Cannot read the ROM (AVPBooter.vmapple2.bin).");
    hv_vmapple_reset_cpu(&board);
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) != 0) fprintf(stderr, "hengevm: user-mode network unavailable\n");
        else net_on = 1;
    }
    if (board.snd && !c->no_sound) hv_audio_attach(board.snd);
    board.poll = board_poll;
    if (board.vusb && (getenv("HV_VUSB_PROBE") || c->usb_probe_on_normal_boot)) hv_usb_probe_start(board.vusb);
    // The setter and publication share this lock: an older preboot target can
    // never overwrite a monitor change made while the board was being built.
    os_unfair_lock_lock(&display_lock);
    if (display_layer_set && board.pvg) hv_pvg_lite_set_layer(board.pvg, (void *)display_layer);
    if (board.pvg) hv_pvg_lite_set_cursor_listener(board.pvg, vm_cursor_listener, NULL);
    atomic_store_explicit(&display_pvg, board.pvg, memory_order_release);
    os_unfair_lock_unlock(&display_lock);
    return 0;
}

/* Thread handles and "ended" flags are kept (as in the original Henge's host/vm.c) so the CPU 0 thread can join every other thread before it flushes the disks and
   reports the end: a secondary CPU or the kicker still running past the exit callback could touch memory the caller is about to release. */
static pthread_t secondary_threads[HV_MAX_CPUS], kicker;
static int secondary_started[HV_MAX_CPUS], kicker_started;
static _Atomic int secondary_ended[HV_MAX_CPUS], kicker_ended;
static _Atomic int kicker_stopped;

static void *kicker_thread(void *unused) {
    (void)unused;
    while (!atomic_load(&kicker_stopped)) {
        usleep(1000);
        hv_machine_kick_all(&m);
    }
    atomic_store(&kicker_ended, 1);
    return NULL;
}

/* A thread that runs guest code. While a block runs its SP register holds the
   guest's SP, a guest address, so the kernel must never push a signal frame
   there: other threads take the asynchronous signals, and a handler that does
   run here (a fault) gets a stack of its own. */
void hv_vm_prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    if (hv_prof_register) hv_prof_register();
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGTRAP), sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/* iOS parks default-QoS threads on efficiency cores readily; the guest's CPUs are what the user waits on. */
void hv_vm_cpu_qos(void) {
    hv_vm_prepare_cpu_thread();
#if defined(__APPLE__)
    /* HV_QOS=ui|init|default (testing): the class the guest's CPUs run at. */
    const char *q = getenv("HV_QOS");
#if TARGET_OS_IPHONE
    qos_class_t c = QOS_CLASS_USER_INITIATED;
#else
    qos_class_t c = QOS_CLASS_DEFAULT;
#endif
    if (q && !strcmp(q, "ui")) c = QOS_CLASS_USER_INTERACTIVE;
    else if (q && !strcmp(q, "init")) c = QOS_CLASS_USER_INITIATED;
    else if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
    if (c != QOS_CLASS_DEFAULT) pthread_set_qos_class_self_np(c, 0);
#endif /* no scheduling class off Apple yet: which cores the guest's CPUs should use is a device question */
}

/* CPUs 1 and up: run whenever PSCI CPU_ON starts them; the machine stops with CPU 0. */
static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    hv_vm_cpu_qos();
    int r = hv_run(c);
    if (!__atomic_load_n(&c->halted, __ATOMIC_ACQUIRE))
        fprintf(stderr, "hengevm: cpu %u stopped (%s) at pc %llx el%u\n", c->index, hv_vm_reason_name(r),
                (unsigned long long)c->pc, c->el);
    atomic_store(&secondary_ended[c->index], 1);
    return NULL;
}

void hv_vm_flush_disks(void) {
    hv_disk_flush(&disk);
    hv_disk_flush(&aux);
}

/* Like hv_vm_flush_disks, but says whether BOTH flushes succeeded (0) or not (-1). A read-only disk reports 0. A confirmed machine stop is a separate fact from this. */
int hv_vm_flush_disks_checked(void) {
    int a = hv_disk_flush(&disk), b = hv_disk_flush(&aux);
    return (a == 0 && b == 0) ? 0 : -1;
}

void hv_vm_disk_write_stats(uint64_t *calls, uint64_t *refused) {
    uint64_t c1, r1, c2, r2;
    hv_disk_write_stats(&disk, &c1, &r1);
    hv_disk_write_stats(&aux, &c2, &r2);
    *calls = c1 + c2, *refused = r1 + r2;
}

HvDisk *hv_vm_aux_disk(void) { return &aux; }

void hv_vm_disk_stats(HvDiskStats *root, HvDiskStats *auxs) {
    hv_disk_stats(&disk, root);
    hv_disk_stats(&aux, auxs);
}

int hv_vm_run_cpu0(void) {
    return hv_run(&cpus[0]);
}

/* Join one thread, but not forever: a vCPU stuck in a host call must not keep the exit callback (and so the app's "the machine stopped") from ever being reported. The wait is
   bounded; a thread that has not ended by then is left running and said so, and the stop is still reported (the caller can see the message). */
#define HV_JOIN_WAIT_MS 5000
static int join_bounded(pthread_t t, _Atomic int *ended, const char *what) {
    for (int waited = 0; !atomic_load(ended); waited++) {
        if (waited >= HV_JOIN_WAIT_MS) {
            fprintf(stderr, "hengevm: %s did not end within %d ms after the stop; not joined\n", what, HV_JOIN_WAIT_MS);
            pthread_detach(t);
            return -1;
        }
        usleep(1000);
    }
    pthread_join(t, NULL);
    return 0;
}

/* After CPU 0 has left its run loop: stop every other CPU thread and the kicker, then finish the disk workers' DMA, so the flush that follows covers every write the
   guest was told had completed. Halts with HV_HALT "host" (3) only where no reason is recorded yet: a guest SYSTEM_OFF/RESET keeps its own. 0, or -1 if a thread was not joined. */
static int quiesce_result = -1;   /* -1 not run, 0 a thread was not joined, 1 every thread joined and the blk workers stopped */
int hv_vm_quiesced_ok(void) { return __atomic_load_n(&quiesce_result, __ATOMIC_ACQUIRE); }

int hv_vm_quiesce(void) {
    int bad = 0;
    hv_machine_halt(&m, HV_HALT_HOST);
    atomic_store(&kicker_stopped, 1);
    /* A handle is forgotten only for a thread that was joined: one that did not end stays recorded as still running. */
    for (int i = 1; i < ncpus; i++)
        if (secondary_started[i]) {
            if (join_bounded(secondary_threads[i], &secondary_ended[i], "a secondary CPU thread")) bad = 1;
            else secondary_started[i] = 0;
        }
    if (kicker_started) {
        if (join_bounded(kicker, &kicker_ended, "the kicker thread")) bad = 1;
        else kicker_started = 0;
    }
    /* Only with every CPU thread gone are the disk workers stopped: a CPU still running could submit a new request after its worker had ended, and that request would never be done. */
    if (!bad) {
        /* A DMA worker can still be writing after the guest CPUs have exited. */
        if (hv_virtio_blk_stop(board.root_blk) != 0) bad = 1;
        if (hv_virtio_blk_stop(board.aux_blk) != 0) bad = 1;
    }
    __atomic_store_n(&quiesce_result, bad ? 0 : 1, __ATOMIC_RELEASE);
    return bad ? -1 : 0;
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    hv_vm_cpu_qos();
    int r = hv_vm_run_cpu0();
    /* When the other threads or the disk workers could not be stopped, nothing says the writes the guest was told had completed all reached the file: the flush is NOT run (and not
       reported as a success), and the end is still reported (hv_vm_quiesced_ok() says 0) so the caller can end the process and not restart it. */
    if (hv_vm_quiesce() == 0) hv_vm_flush_disks();
    if (exit_cb) exit_cb(r);
    return NULL;
}

static int start_failed;
int hv_vm_start_failed(void) { return __atomic_load_n(&start_failed, __ATOMIC_ACQUIRE); }

void hv_vm_start(int cpu0, void (*on_exit)(int reason)) {
    exit_cb = on_exit;
    __atomic_store_n(&start_failed, 0, __ATOMIC_RELEASE);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t t;
    /* A thread that did not start is not silent, and is not made up for: a guest on fewer CPUs than it was told (or without the tick that drives its timers) does not run
       as configured, so the machine is stopped instead of limping on. If CPU 0's own thread was the one that failed nobody else would report the end, so the exit callback is
       called here (reason HV_EXIT_NONE: not a halt, not a fault); in the other cases CPU 0's thread, stopped by hv_vm_stop(), reports it as it always does. */
    int rc, failed = 0, cpu0_failed = 0;
    atomic_store(&kicker_stopped, 0);
    atomic_store(&kicker_ended, 0);
    /* Every join handle is published before CPU 0 can reach its exit callback, so CPU 0 is created last (as in the original Henge). */
    for (int i = 1; i < ncpus; i++) {
        atomic_store(&secondary_ended[i], 0);
        secondary_started[i] = (rc = pthread_create(&secondary_threads[i], &attr, secondary_thread, &cpus[i])) == 0;
        if (!secondary_started[i]) fprintf(stderr, "hengevm: CPU %d thread did not start (error %d)\n", i, rc), failed = 1;
    }
    kicker_started = (rc = pthread_create(&kicker, NULL, kicker_thread, NULL)) == 0;
    if (!kicker_started) fprintf(stderr, "hengevm: kicker thread did not start (error %d)\n", rc), failed = 1;
    if (cpu0 && (rc = pthread_create(&t, &attr, cpu0_thread, NULL)) != 0) fprintf(stderr, "hengevm: CPU 0 thread did not start (error %d)\n", rc), failed = cpu0_failed = 1;
    HV_STAGE("vcpus_started");
    g7_fb0_maybe_start();
    if (failed) {
        __atomic_store_n(&start_failed, 1, __ATOMIC_RELEASE);   /* callers that can fail (the G7 runner) ask hv_vm_start_failed(); HV_EXIT_NONE alone is not an error code */
        hv_vm_stop();
        if (cpu0_failed) {
            hv_vm_quiesce();
            if (exit_cb) exit_cb(HV_EXIT_NONE);
        }
    }
}

/* A host stop records "3" only where no reason is recorded yet (hv_machine_halt compares against 0): a guest SYSTEM_OFF (1) / SYSTEM_RESET (2) or a pvpanic (4) that came
   first is not overwritten, so the first reason is what the runners report. It also wakes a CPU parked in PSCI CPU_OFF. */
void hv_vm_stop(void) {
    hv_machine_halt(&m, HV_HALT_HOST);
    hv_machine_kick_all(&m);
}

/* macOS's virtio mouse driver drops BTN_RIGHT, so the right button is a Control-click, with the right
   Control. The USB keyboard is read every 10 ms and the click goes over virtio, so Control goes down
   50 ms before the click and comes up 50 ms after it. The waits run on a serial queue that the pointer's
   moves and buttons all go through, so they stay in order and the caller (often the main thread) never
   waits. The guest's right Control and left button each have two owners, and stay down while either holds them. */
#define HID_RIGHT_CONTROL 0xe4
#define LINUX_KEY_RIGHTCTRL 97
static os_unfair_lock rctrl_lock = OS_UNFAIR_LOCK_INIT;
static int rctrl_user, rctrl_click;
static int left_user, left_click, left_sent; /* only the pointer queue touches these */

#if defined(__APPLE__)
static dispatch_queue_t pointer_queue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("henge.pointer", DISPATCH_QUEUE_SERIAL); });
    return q;
}
static void pointer_post(void (*fn)(void *), void *ctx) { dispatch_async_f(pointer_queue(), ctx, fn); }
#else
/* The same serial queue: one worker thread that runs the posted jobs in order. */
typedef struct PointerJob {
    void (*fn)(void *);
    void *ctx;
} PointerJob;
#define POINTER_JOBS 256
static PointerJob pointer_jobs[POINTER_JOBS];
static unsigned pointer_head, pointer_tail;
static pthread_mutex_t pointer_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pointer_cv = PTHREAD_COND_INITIALIZER;

static void *pointer_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&pointer_mu);
        while (pointer_head == pointer_tail) pthread_cond_wait(&pointer_cv, &pointer_mu);
        PointerJob job = pointer_jobs[pointer_head++ % POINTER_JOBS];
        pthread_mutex_unlock(&pointer_mu);
        job.fn(job.ctx);
    }
    return NULL;
}

static void pointer_post(void (*fn)(void *), void *ctx) {
    static int started;
    pthread_mutex_lock(&pointer_mu);
    if (!started) {
        pthread_t t;
        if (pthread_create(&t, NULL, pointer_worker, NULL) == 0) pthread_detach(t), started = 1;
    }
    if (pointer_tail - pointer_head < POINTER_JOBS) pointer_jobs[pointer_tail++ % POINTER_JOBS] = (PointerJob){fn, ctx};
    pthread_cond_signal(&pointer_cv);
    pthread_mutex_unlock(&pointer_mu);
}
#endif

static void rctrl_hold(int *owner, int down) {
    os_unfair_lock_lock(&rctrl_lock);
    *owner = down;
    hv_xhci_key(board.usb, HID_RIGHT_CONTROL, rctrl_user || rctrl_click);
    os_unfair_lock_unlock(&rctrl_lock);
}

void hv_vm_key(uint16_t code, int down) {
    if (code == LINUX_KEY_RIGHTCTRL) rctrl_hold(&rctrl_user, down);
    else hv_xhci_key_linux(board.usb, code, down);
}

void hv_vm_key_hid(uint8_t usage, int down) {
    if (usage == HID_RIGHT_CONTROL) rctrl_hold(&rctrl_user, down);
    else hv_xhci_key(board.usb, usage, down);
}

static void move_now(void *ctx) {
    int64_t packed = (int64_t)(intptr_t)ctx;
    int32_t dx = (int32_t)(packed >> 32), dy = (int32_t)packed;
    if (dx) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, dx);
    if (dy) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, dy);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

void hv_vm_mouse_move(int dx, int dy) {
    int64_t packed = (int64_t)((uint64_t)(uint32_t)dx << 32 | (uint32_t)dy);
    pointer_post(move_now, (void *)(intptr_t)packed);
}

static void left_update(void) {
    int down = left_user || left_click;
    if (down == left_sent) return;
    left_sent = down;
    hv_virtio_input_send(board.mouse, HV_EV_KEY, HV_BTN_LEFT, down);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

static void button_now(void *ctx) {
    intptr_t v = (intptr_t)ctx;
    int button = (int)(v >> 1), down = (int)(v & 1);
    if (button == 0) {
        left_user = down;
        left_update();
    } else if (button == 1 && down != left_click) {
        if (down) {
            rctrl_hold(&rctrl_click, 1);
            usleep(50000);
        }
        left_click = down;
        left_update();
        if (!down) {
            usleep(50000);
            rctrl_hold(&rctrl_click, 0);
        }
    } else if (button == 2) {
        hv_virtio_input_send(board.mouse, HV_EV_KEY, HV_BTN_MIDDLE, down);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
    }
}

void hv_vm_mouse_button(int button, int down) {
    if (button < 0 || button > 2) return;
    pointer_post(button_now, (void *)(intptr_t)(button << 1 | !!down));
}

/* The guest takes a wheel event as 8 bits: -200 arrives as +56, the wrong way. So big moves go in pieces. */
static void wheel(uint16_t axis, int amount) {
    while (amount) {
        int piece = amount > 100 ? 100 : amount < -100 ? -100 : amount;
        hv_virtio_input_send(board.mouse, HV_EV_REL, axis, piece);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
        amount -= piece;
    }
}

void hv_vm_mouse_wheel(int dy) { wheel(HV_REL_WHEEL, dy); }
void hv_vm_mouse_hwheel(int dx) { wheel(HV_REL_HWHEEL, dx); }

int hv_vm_pointer(int *x, int *y) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    const uint8_t *page = pvg ? hv_pvg_lite_display_page(pvg) : NULL;
    if (!page) return -1;
    uint32_t at = *(const volatile uint32_t *)(page + 0xe00);
    if (at == 0xffffffff) return -1;
    *x = (int)(at & 0xffff), *y = (int)(at >> 16);
    return 0;
}

int hv_vm_snapshot(uint8_t *bgra, int width, int height) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg || width <= 0 || height <= 0) return -1;
    return hv_pvg_lite_snapshot(pvg, bgra, (uint32_t)width, (uint32_t)height);
}

int hv_vm_capture_screen(uint8_t *bgra, int width, int height, int *source, HvScreenCaptureInfo *info) {
    if (source) *source = HV_SCREEN_NONE;
    if (info) memset(info, 0, sizeof *info);
    if (width <= 0 || height <= 0) return -1;
    if (hv_vm_snapshot(bgra, width, height) == 0) {
        if (source) *source = HV_SCREEN_PVG;
        return 0;
    }
    HvBootDisplay d;
    if (hv_vm_boot_display(&d) != 0 || !d.pixels || !d.width || !d.height || !d.stride) return -1;
    if (info) info->orig_width = d.width, info->orig_height = d.height, info->orig_stride = d.stride;
    /* Henge: scale the WHOLE source frame to exactly fill width x height (nearest-neighbor, independently on each
       axis) rather than copying only its top-left width x height corner -- see the doc comment in vm.h for why. */
    for (uint32_t y = 0; y < (uint32_t)height; y++) {
        uint32_t sy = (y * d.height) / (uint32_t)height;
        if (sy >= d.height) sy = d.height - 1; /* guards the single case y==height-1 could round past d.height-1 */
        const uint8_t *srow = d.pixels + (size_t)sy * d.stride;
        uint8_t *drow = bgra + (size_t)y * (size_t)width * 4;
        for (uint32_t x = 0; x < (uint32_t)width; x++) {
            uint32_t sx = (x * d.width) / (uint32_t)width;
            if (sx >= d.width) sx = d.width - 1;
            memcpy(drow + (size_t)x * 4, srow + (size_t)sx * 4, 4);
        }
    }
    /* hv_vm_boot_display() now fills d.source itself (HV_SCREEN_PVG, HV_SCREEN_PVG_SURFACE, or
       HV_SCREEN_BOOT_ARGS) -- a pointer-range check alone could no longer tell the GPU frame and the new
       PVG-surface copy apart, since neither lives in board.ram; see host/vm.h's doc comment on HvBootDisplay. */
    if (source) *source = d.source;
    return 0;
}
