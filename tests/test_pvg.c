/* B1-a: the PROTOCOL half of the graphics device, devices/pvg_lite.c, compiled unchanged and driven the way the guest driver drives it: through the two MMIO
   register windows and rings laid out in guest memory exactly as the file's header comment describes (root ring, channel records and page tables, stamps, the
   display page, the surface mapper, task memory). Any Linux, any CPU: the machine is replaced by a flat RAM array, the interrupt controller by a counter, and the
   Metal side (pvg_metal.m) by a RECORDER that stores what the device hands it and lets the test complete stamps the way the real worker does.
   What this shows: that the rings are read, bounded, ordered and acknowledged as the protocol says, that stamps, status bits and interrupts follow, that barriers
   hold a channel back until the other channel's stamp (with wrap-around), and that malformed input does not crash.
   What it does NOT show: anything about drawing. Nothing here executes a shader, a Metal call, a Vulkan call or a real guest; the recorder is not a GPU and the
   guest builder below is written from the header comment, so a wrong reading of the real driver would be wrong in both places. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../core/machine.h"
#include "../devices/pvg_lite.h"

/* ---- the machine: flat RAM ---- */
#define RB 0x70000000ull
#define RSZ (16u << 20)
static uint8_t *ram;
static HvMachine M;
static int wakes, pends, last_intid;

void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) {
    (void)m;
    if (pa < RB || len > RSZ || pa - RB > RSZ - len) return NULL;
    return ram + (pa - RB);
}
static uint64_t (*gfx_r)(void *, uint64_t, unsigned), (*ios_r)(void *, uint64_t, unsigned);
static void (*gfx_w)(void *, uint64_t, uint64_t, unsigned), (*ios_w)(void *, uint64_t, uint64_t, unsigned);
static void *gfx_o, *ios_o;
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque, uint64_t (*r)(void *, uint64_t, unsigned), void (*w)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    (void)m, (void)pa, (void)size;
    if (!strcmp(name, "gfx")) gfx_r = r, gfx_w = w, gfx_o = opaque;
    else ios_r = r, ios_w = w, ios_o = opaque;
    return 0;
}
void hv_machine_wake(HvMachine *m) { (void)m; wakes++; }
void hv_gicv3_pend(HvGic *g, int intid) { (void)g; pends++; last_intid = intid; }

/* ---- the Metal side: a recorder ---- */
typedef struct { int ch; uint32_t len; uint8_t d[9000]; } Sub;
static Sub subs[512];
static int nsubs, created, unmapped[32], nun, cursor_moves;
static int dummy;
HvPvgMetal *hv_pvg_metal_create(HvPvgLite *l) { (void)l; created++; return (HvPvgMetal *)&dummy; }
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *l, uint32_t ch, uint32_t *cmd, uint32_t len) {
    (void)m, (void)l;
    if (nsubs < 512) { subs[nsubs].ch = (int)ch; subs[nsubs].len = len; memcpy(subs[nsubs].d, cmd, len < sizeof subs[0].d ? len : sizeof subs[0].d); nsubs++; }
    free(cmd);   /* the real worker frees the copy */
}
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *b, uint32_t w, uint32_t h) { (void)m, (void)b, (void)w, (void)h; return 0; }
int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **px, uint32_t *w, uint32_t *h) { (void)m; static uint8_t f[16]; *px = f, *w = 2, *h = 2; return 0; }
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t id) { (void)m; if (nun < 32) unmapped[nun++] = (int)id; }
void hv_pvg_metal_cursor_moved(HvPvgMetal *m) { (void)m; cursor_moves++; }
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *l) { (void)m, (void)l; }
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) { (void)m; return 42; }

/* ---- the guest ---- */
static int bad;
#define EXPECT(cond, ...) do { int ok_ = !!(cond); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

static uint32_t next_page, fifo_pn, root_pn, root_wp;
static HvPvgLite *dev;
static uint32_t alloc_page(void) { return next_page++; }
static uint8_t *pgp(uint32_t pn) { return ram + ((uint64_t)pn << 14) - RB; }
static void W(uint64_t off, uint64_t v) { gfx_w(gfx_o, off, v, 4); }
static uint64_t Rg(uint64_t off) { return gfx_r(gfx_o, off, 4); }
static uint32_t stamp_of(int ch) { return *(uint32_t *)(pgp(fifo_pn) + 4 * ch); }

static HvPvgLite *dev_new(int no_metal) {
    memset(ram, 0, RSZ);
    memset(subs, 0, sizeof subs);
    nsubs = created = nun = cursor_moves = wakes = pends = 0;
    next_page = (uint32_t)(RB >> 14) + 1;
    if (no_metal) setenv("HV_PVG_NO_METAL", "1", 1); else unsetenv("HV_PVG_NO_METAL");
    M.ncpus = 1;
    dev = hv_pvg_lite_create(&M, (HvGic *)&dummy, 0x30200000ull, 0x30210000ull, 7, 8);
    fifo_pn = alloc_page(); alloc_page();   /* two pages: the stamps, then the 16 KB root ring */
    root_pn = alloc_page();
    root_wp = 0;
    W(0x1000, 1); W(0x1004, 0x8000); W(0x1010, 0x4000); W(0x1030, fifo_pn); W(0x101c, root_pn);
    return dev;
}

static void root_cmd(uint32_t op, uint32_t len, uint32_t stamp, const uint32_t *pl, int n) {
    uint32_t w[3] = {op, len, stamp};
    for (int i = 0; i < 3; i++) *(uint32_t *)(pgp(fifo_pn) + 0x4000 + (root_wp + 4u * i) % 0x4000) = w[i];
    for (int i = 0; i < n; i++) *(uint32_t *)(pgp(fifo_pn) + 0x4000 + (root_wp + 12u + 4u * i) % 0x4000) = pl[i];
    root_wp += len;
    W(0x1008, root_wp);
}

typedef struct { uint32_t table_pn, pages, wr; } Ch;
static Ch chs[16];
static uint32_t *rec(int c) { return (uint32_t *)(pgp(root_pn) + 0x400 + 0x14 * (c - 1)); }
static void chan_setup(int c, int pages) {
    uint32_t t = alloc_page();
    for (int i = 0; i < pages; i++) ((uint32_t *)pgp(t))[i] = alloc_page();
    uint32_t *r = rec(c);
    r[0] = 0, r[1] = 0, r[2] = 0xffffffffu, r[3] = (uint32_t)c, r[4] = t;
    chs[c] = (Ch){t, (uint32_t)pages, 0};
}
static uint8_t *ring_at(int c, uint32_t at) {
    at %= chs[c].pages << 14;
    return pgp(((uint32_t *)pgp(chs[c].table_pn))[at >> 14]) + (at & 0x3fff);
}
/* A command: header {op, len_field, stamp}, payload bytes; the write pointer moves by `advance` (normally len_field). */
static void chan_raw(int c, uint32_t op, uint32_t len_field, uint32_t stamp, const void *pl, uint32_t plen, uint32_t advance) {
    uint32_t h[3] = {op, len_field, stamp};
    for (uint32_t i = 0; i < 12; i++) *ring_at(c, chs[c].wr + i) = ((uint8_t *)h)[i];
    for (uint32_t i = 0; i < plen; i++) *ring_at(c, chs[c].wr + 12 + i) = ((const uint8_t *)pl)[i];
    chs[c].wr += advance;
    __atomic_store_n(&rec(c)[0], chs[c].wr, __ATOMIC_RELEASE);
}
static void chan_cmd(int c, uint32_t op, uint32_t stamp, const void *pl, uint32_t plen) { chan_raw(c, op, 12 + plen, stamp, pl, plen, 12 + plen); }
static void ring_bell(int c) { W(0x1020, (uint64_t)c); }
static uint32_t rd_ptr(int c) { return rec(c)[1]; }

/* ---- scenarios ---- */
static void s_root(void) {
    dev_new(1);
    uint32_t pl[3] = {5, 0, (uint32_t)(fifo_pn + 3)};   /* count at +12, page at +20 */
    uint32_t info_pn = alloc_page();
    pl[2] = info_pn;
    root_cmd(0x3a, 24, 0x111, pl, 3);
    uint32_t rp = (uint32_t)Rg(0x100c);
    const uint32_t *info = (const uint32_t *)pgp(info_pn);
    EXPECT(rp == 24, "root: the read pointer moved past the command (%u)", rp);
    EXPECT(stamp_of(0) == 0x111, "root: the stamp was stored (%x)", stamp_of(0));
    EXPECT(info[0] == 1 && info[1] == 4 && info[2] == 2 && info[3] == 0 && info[4] == 3 && info[5] == 0x400, "root: the device-info keys were written into the guest's page");
    uint32_t pl2[4] = {10, 0, 0, 77};   /* task root: id at +12 (>>1 = task), page at +24 */
    root_cmd(0x38, 28, 0x112, pl2, 4);
    EXPECT(Rg(0x100c) == 52 && stamp_of(0) == 0x112, "root: a second command follows the first");
    /* a command shorter than its own header: the ring is dropped up to the write pointer, the next one still works */
    root_cmd(0x99, 4, 0x113, NULL, 0);
    uint32_t after_bad = (uint32_t)Rg(0x100c);
    EXPECT(after_bad == root_wp && stamp_of(0) == 0x112, "root: a too-short command is dropped (rp %u == wp %u), no stamp for it", after_bad, root_wp);
    root_cmd(0x3b, 12, 0x114, NULL, 0);
    EXPECT(Rg(0x100c) == root_wp && stamp_of(0) == 0x114, "root: after the bad one the ring works again");
}

static void s_channel_nometal(void) {
    dev_new(1);
    chan_setup(2, 1);
    uint32_t pl[2] = {1, 2};
    chan_cmd(2, 5, 11, pl, 8); chan_cmd(2, 5, 12, pl, 8); chan_cmd(2, 5, 13, pl, 8);
    ring_bell(2);
    EXPECT(rd_ptr(2) == chs[2].wr, "no backend: the channel is consumed (rd %u == wr %u)", rd_ptr(2), chs[2].wr);
    EXPECT(stamp_of(2) == 13, "no backend: the stamp is the LAST command's (%u)", stamp_of(2));
    uint32_t st = (uint32_t)Rg(0x1018);
    EXPECT(st == 1u << 2, "no backend: status has bit 2 (%x)", st);
    EXPECT(Rg(0x1018) == 0, "no backend: reading the status clears it");
    EXPECT(pends >= 1 && last_intid == 7, "no backend: the graphics interrupt was raised (%d pends, intid %d)", pends, last_intid);
    EXPECT(created == 0 && nsubs == 0, "no backend: HV_PVG_NO_METAL means no backend was created");
    EXPECT(hv_pvg_lite_gpu_frames(dev) == 0 && hv_pvg_lite_snapshot(dev, NULL, 0, 0) == -1, "no backend: no frames, snapshot unavailable");
    const uint8_t *px; uint32_t w, h;
    EXPECT(hv_pvg_lite_frame(dev, &px, &w, &h) == -1, "no backend: frame() says there is none");
}

static void s_channel_backend(void) {
    dev_new(0);
    chan_setup(2, 1);
    uint32_t a[3] = {0xa1, 0xa2, 0xa3}, b[2] = {0xb1, 0xb2};
    chan_cmd(2, 0x43, 21, a, 12);
    chan_cmd(2, 0x25, 22, b, 8);
    ring_bell(2);
    EXPECT(created == 1 && nsubs == 2, "backend: created once, two commands handed over (%d, %d)", created, nsubs);
    EXPECT(subs[0].ch == 2 && subs[0].len == 24 && ((uint32_t *)subs[0].d)[0] == 0x43 && ((uint32_t *)subs[0].d)[2] == 21 && ((uint32_t *)subs[0].d)[3] == 0xa1 && ((uint32_t *)subs[0].d)[5] == 0xa3, "backend: first command byte for byte, channel 2");
    EXPECT(subs[1].ch == 2 && subs[1].len == 20 && ((uint32_t *)subs[1].d)[2] == 22 && ((uint32_t *)subs[1].d)[4] == 0xb2, "backend: second command, in order");
    EXPECT(rd_ptr(2) == chs[2].wr, "backend: the read pointer moved when the commands were handed over");
    EXPECT(stamp_of(2) == 0 && Rg(0x1018) == 0, "backend: NO stamp and NO status before the backend completes");
    int p0 = pends;
    hv_pvg_lite_complete(dev, 2, 22);
    EXPECT(stamp_of(2) == 22, "backend: complete() stored the stamp");
    EXPECT(pends == p0 + 1 && last_intid == 7 && wakes == 1, "backend: complete() raised the interrupt at once and woke the CPU (single-CPU machine)");
    EXPECT(Rg(0x1018) == 1u << 2, "backend: complete() set the channel's status bit");
    EXPECT(hv_pvg_lite_gpu_frames(dev) == 42, "backend: gpu_frames comes from the backend");
    const uint8_t *px; uint32_t w, h;
    EXPECT(hv_pvg_lite_frame(dev, &px, &w, &h) == 0 && w == 2 && h == 2, "backend: frame() comes from the backend");
    W(0x1220, 1);
    EXPECT(cursor_moves == 1, "backend: the display doorbell tells the backend the pointer moved");
}

/* ---- B1-b: the function table ---- */
static int t_created, t_subs, t_order[16], t_unmaps, t_fail_create, t_cursor;
static int t_inst;
static HvPvgMetal *t_create(HvPvgLite *l) { (void)l; t_created++; return t_fail_create ? NULL : (HvPvgMetal *)&t_inst; }
static void t_submit(HvPvgMetal *m, HvPvgLite *l, uint32_t ch, uint32_t *cmd, uint32_t len) {
    (void)m, (void)l, (void)ch, (void)len;
    if (t_subs < 16) t_order[t_subs] = (int)cmd[2];
    t_subs++;
    free(cmd);   /* ownership: the backend frees the copy, once */
}
static void t_unmap(HvPvgMetal *m, uint32_t id) { (void)m, (void)id; t_unmaps++; }
static void t_cur(HvPvgMetal *m) { (void)m; t_cursor++; }
static const HvPvgBackend T_FULL = {"test", t_create, t_submit, NULL, NULL, t_unmap, t_cur, NULL, NULL};

static void s_backend(void) {
    const HvPvgBackend *ap = hv_pvg_apple_backend();
    EXPECT(ap && !strcmp(ap->name, "apple") && ap->create == hv_pvg_metal_create && ap->submit == hv_pvg_metal_submit && ap->frame == hv_pvg_metal_frame && ap->snapshot == hv_pvg_metal_snapshot &&
           ap->unmap == hv_pvg_metal_unmap && ap->cursor_moved == hv_pvg_metal_cursor_moved && ap->set_layer == hv_pvg_metal_set_layer && ap->gpu_frames == hv_pvg_metal_gpu_frames,
           "backend table: the default is the existing link functions, unchanged");
    dev_new(0);
    EXPECT(hv_pvg_lite_backend_state(dev) == HV_PVG_BACKEND_NOT_TRIED, "backend table: not tried before the first channel command");
    HvPvgBackend bad1 = T_FULL, bad2 = T_FULL;
    bad1.create = NULL; bad2.submit = NULL;
    EXPECT(hv_pvg_lite_set_backend(dev, NULL) == -1 && hv_pvg_lite_set_backend(dev, &bad1) == -1 && hv_pvg_lite_set_backend(dev, &bad2) == -1 && hv_pvg_lite_set_backend(NULL, &T_FULL) == -1,
           "backend table: a table without create or submit is refused");
    t_created = t_subs = t_unmaps = t_fail_create = t_cursor = 0;
    EXPECT(hv_pvg_lite_set_backend(dev, &T_FULL) == 0, "backend table: accepted before the first command");
    chan_setup(2, 1);
    uint32_t a[1] = {1};
    chan_cmd(2, 0x43, 31, a, 4); chan_cmd(2, 0x44, 32, a, 4); chan_cmd(2, 0x45, 33, a, 4);
    ring_bell(2);
    EXPECT(t_created == 1 && created == 0 && nsubs == 0, "backend table: created once, and the default functions were not touched (%d, %d, %d)", t_created, created, nsubs);
    EXPECT(t_subs == 3 && t_order[0] == 31 && t_order[1] == 32 && t_order[2] == 33, "backend table: three commands, in order (%d: %d %d %d)", t_subs, t_order[0], t_order[1], t_order[2]);
    EXPECT(hv_pvg_lite_backend_state(dev) == HV_PVG_BACKEND_OK, "backend table: state ok");
    EXPECT(stamp_of(2) == 0 && Rg(0x1018) == 0, "backend table: no stamp before the backend completes, as with Apple's");
    hv_pvg_lite_complete(dev, 2, 33);
    EXPECT(stamp_of(2) == 33 && Rg(0x1018) == 1u << 2, "backend table: complete() reports the stamp the same way");
    EXPECT(hv_pvg_lite_gpu_frames(dev) == 0 && hv_pvg_lite_snapshot(dev, NULL, 0, 0) == -1, "backend table: an absent optional function means 0 / -1, not a crash");
    const uint8_t *px; uint32_t w, h;
    EXPECT(hv_pvg_lite_frame(dev, &px, &w, &h) == -1, "backend table: absent frame() is -1");
    W(0x1220, 1); hv_pvg_lite_set_layer(dev, NULL);
    EXPECT(t_cursor == 1, "backend table: the optional cursor function is called");
    EXPECT(hv_pvg_lite_set_backend(dev, ap) == -1 && hv_pvg_lite_backend_state(dev) == HV_PVG_BACKEND_OK, "backend table: too late to replace once the first command created one");
    /* a backend whose create fails: not the same as none */
    dev_new(0);
    t_created = t_subs = t_fail_create = 0; t_fail_create = 1;
    hv_pvg_lite_set_backend(dev, &T_FULL);
    chan_setup(2, 1);
    chan_cmd(2, 0x43, 41, a, 4); chan_cmd(2, 0x43, 42, a, 4);
    ring_bell(2);
    EXPECT(hv_pvg_lite_backend_state(dev) == HV_PVG_BACKEND_FAILED && t_created == 1, "backend table: create returned NULL: state failed, tried once");
    EXPECT(t_subs == 0 && rd_ptr(2) == chs[2].wr && stamp_of(2) == 42, "backend table: failed backend: commands are still consumed and completed (stamp %u)", stamp_of(2));
    chan_cmd(2, 0x43, 43, a, 4); ring_bell(2);
    EXPECT(t_created == 1 && stamp_of(2) == 43, "backend table: create is not retried on every command");
    /* none by choice */
    dev_new(1);
    t_created = 0;
    hv_pvg_lite_set_backend(dev, &T_FULL);
    chan_setup(2, 1); chan_cmd(2, 0x43, 51, a, 4); ring_bell(2);
    EXPECT(hv_pvg_lite_backend_state(dev) == HV_PVG_BACKEND_NONE && t_created == 0 && stamp_of(2) == 51, "backend table: HV_PVG_NO_METAL means none (not failed): state none, create never called");
}

/* ---- B1: completion notices and command ownership. Lines tagged [existing] fix what the CURRENT source does (observed, reproducible); [correctness] judges it against the
   contract stamps need (monotonic, one interrupt per notice, buffer valid until the backend frees it). A [FINDING] is a place where the first does not meet the second:
   it is recorded here as observed behaviour, not changed. The recorder is not Apple's asynchronous worker: nothing here shows real GPU synchronisation is safe. */
static uint8_t *held[8]; static uint32_t held_len[8]; static int nheld;
static void h_submit(HvPvgMetal *m, HvPvgLite *l, uint32_t ch, uint32_t *cmd, uint32_t len) { (void)m, (void)l, (void)ch; if (nheld < 8) { held[nheld] = (uint8_t *)cmd; held_len[nheld++] = len; } else free(cmd); }
static const HvPvgBackend H_ASYNC = {"async", t_create, h_submit, NULL, NULL, NULL, NULL, NULL, NULL};

static void s_complete(void) {
    dev_new(1);
    chan_setup(2, 1); chan_setup(3, 1);
    int p0 = pends;
    hv_pvg_lite_complete(dev, 2, 22);
    hv_pvg_lite_complete(dev, 2, 22);
    EXPECT(stamp_of(2) == 22 && pends == p0 + 2, "[existing] a duplicate completion: same stamp, and one interrupt per notice (%d)", pends - p0);
    EXPECT(Rg(0x1018) == 1u << 2 && Rg(0x1018) == 0, "[existing] the status bit is one bit however many notices; reading clears it");
    hv_pvg_lite_complete(dev, 2, 22);
    EXPECT(Rg(0x1018) == 1u << 2, "[existing] a notice after the status was read sets the bit again");
    hv_pvg_lite_complete(dev, 2, 30);
    hv_pvg_lite_complete(dev, 2, 25);   /* a late, older stamp */
    EXPECT(stamp_of(2) == 25, "[existing] an older stamp after a newer one replaces it (observed: the stamp goes %u, not kept at 30)", stamp_of(2));
    printf("[FINDING] [correctness] hv_pvg_lite_complete stores the stamp unconditionally: a late or reversed notice moves a channel's stamp BACKWARD; the guest's wait on stamp 30 would see 25.\n"
           "          Apple's worker completes in order, so this is not reached today; an asynchronous or multi-queue backend would have to keep order itself (or the device must clamp). Not changed here.\n");
    hv_pvg_lite_complete(dev, 3, 7);
    EXPECT(stamp_of(3) == 7 && stamp_of(2) == 25, "[existing] channels' stamps are independent");
    /* before the guest named the stamp page: nothing to store to */
    dev_new(1);
    W(0x1030, 0);
    int ok = 1; hv_pvg_lite_complete(dev, 2, 5);
    EXPECT(ok && Rg(0x1018) == 1u << 2, "[existing] with no stamp page named the notice stores nothing but still sets status and interrupt, no crash");
    /* ownership: the backend holds the copies and frees them LATER; the device must not touch or reuse them */
    dev_new(0);
    nheld = 0; t_fail_create = 0;
    hv_pvg_lite_set_backend(dev, &H_ASYNC);
    chan_setup(2, 1);
    uint32_t a[2] = {0xaa, 0xbb};
    for (int k = 0; k < 4; k++) chan_cmd(2, 0x40 + (uint32_t)k, 60 + (uint32_t)k, a, 8);
    ring_bell(2);
    for (int k = 0; k < 4; k++) chan_cmd(2, 0x50 + (uint32_t)k, 70 + (uint32_t)k, a, 8);   /* more traffic while the first four are still held */
    ring_bell(2);
    int distinct = 1, intact = nheld == 8;
    for (int i = 0; i < nheld; i++) {
        for (int j = i + 1; j < nheld; j++) if (held[i] == held[j]) distinct = 0;
        uint32_t *c = (uint32_t *)held[i];
        if (i < nheld && (held_len[i] != 20 || c[0] != (uint32_t)((i < 4 ? 0x40 + i : 0x50 + i - 4)) || c[2] != (uint32_t)(i < 4 ? 60 + i : 70 + i - 4) || c[3] != 0xaa || c[4] != 0xbb)) intact = 0;
    }
    EXPECT(distinct && intact, "[correctness] copies handed over are distinct and unchanged while held (8 commands, later traffic did not overwrite earlier ones)");
    for (int i = 0; i < nheld; i++) free(held[i]);   /* freed exactly once, here; ASan reports a device access or a double free */
    hv_pvg_lite_complete(dev, 2, 73);
    EXPECT(stamp_of(2) == 73, "[correctness] completing after the copies were freed does not need them");
}

/* ---- completion channel range ---- */
/* The CANDIDATE circular comparison (not used by the device, which still stores unconditionally: see the [FINDING] above; generation and initial-value rules are unsettled).
   This only pins the arithmetic of the rule so a later change has a checked starting point. */
static int stamp_newer(uint32_t old_stamp, uint32_t new_stamp) { uint32_t delta = new_stamp - old_stamp; return delta != 0 && delta < 0x80000000u; }

static void s_chrange(void) {
    dev_new(1);
    chan_setup(1, 1); chan_setup(15, 1);
    /* valid edges: 1 and 15 work as before */
    int p0 = pends;
    hv_pvg_lite_complete(dev, 1, 11);
    hv_pvg_lite_complete(dev, 15, 99);
    EXPECT(stamp_of(1) == 11 && stamp_of(15) == 99 && Rg(0x1018) == ((1u << 1) | (1u << 15)) && pends == p0 + 2, "chrange: channels 1 and 15 complete as before (stamps 11 and 99, two status bits, two interrupts)");
    /* everything else changes nothing */
    uint32_t before[16]; for (int i = 0; i < 16; i++) before[i] = stamp_of(i);
    uint32_t bad_ch[] = {0, 16, 17, 31, 32, 33, 63, 0x80000000u, 0xffffffffu};
    int p1 = pends, w1 = wakes, clean = 1;
    for (unsigned i = 0; i < sizeof bad_ch / sizeof bad_ch[0]; i++) {
        hv_pvg_lite_complete(dev, bad_ch[i], 0x5a5a5a5a);
        if (Rg(0x1018) != 0) clean = 0;   /* a status bit would show here (and reading clears it) */
    }
    int same = 1; for (int i = 0; i < 16; i++) if (stamp_of(i) != before[i]) same = 0;
    EXPECT(clean && same && pends == p1 && wakes == w1, "chrange: channels 0, 16, 17, 31, 32, 33, 63, 2^31, 2^32-1: no stamp stored, no status bit, no interrupt, no CPU wake");
    EXPECT(hv_pvg_lite_bad_complete_logged() <= 8, "chrange: the warning counter stops at its limit (%u)", hv_pvg_lite_bad_complete_logged());
    for (int i = 0; i < 1000; i++) hv_pvg_lite_complete(dev, 32 + (uint32_t)i, 1);
    EXPECT(hv_pvg_lite_bad_complete_logged() == 8, "chrange: after 1000 more bad completions the counter is exactly at the limit (8), it did not grow");
    /* the stamp page's last word is not reachable by a bad channel (16 KB page: 4 * 4095 is the last word; channel 4095 is out of range) */
    uint32_t last = *(uint32_t *)(pgp(fifo_pn) + 0x3ffc);
    hv_pvg_lite_complete(dev, 4095, 7);
    EXPECT(*(uint32_t *)(pgp(fifo_pn) + 0x3ffc) == last, "chrange: channel 4095 (the stamp page's last word) is out of range and not written");
    /* the candidate circular comparison, as a pure function */
    EXPECT(stamp_newer(30, 31) && !stamp_newer(30, 30) && !stamp_newer(30, 25), "stamp rule (candidate, not applied): 30->31 newer, 30->30 not, 30->25 not");
    EXPECT(stamp_newer(0xfffffffeu, 2) && stamp_newer(0xffffffffu, 0), "stamp rule (candidate, not applied): across the wrap, 0xFFFFFFFE->2 and 0xFFFFFFFF->0 are newer");
    EXPECT(!stamp_newer(0, 0x80000000u) && !stamp_newer(5, 0x80000005u) && stamp_newer(0, 0x7fffffffu), "stamp rule (candidate, not applied): exactly half a turn is refused, one less is newer");
}

static void s_wrap(void) {
    dev_new(0);
    chan_setup(3, 2);   /* two 16 KB pages = 32 KB ring */
    enum { PL = 4096 - 12, LEN = 4096 };
    uint8_t pat[PL];
    int total = 0, okc = 1;
    for (int round = 0; round < 6; round++) {   /* 6 x 6 x 4 KB = 144 KB through a 32 KB ring */
        nsubs = 0;
        for (int k = 0; k < 6; k++) {
            for (int i = 0; i < PL; i++) pat[i] = (uint8_t)(total * 7 + i);
            chan_cmd(3, 0x40, (uint32_t)(100 + total), pat, PL);
            total++;
        }
        ring_bell(3);
        for (int k = 0; k < 6; k++) {
            int n = total - 6 + k;
            const Sub *s = &subs[k];
            if (k >= nsubs || s->ch != 3 || s->len != LEN || ((uint32_t *)s->d)[2] != (uint32_t)(100 + n)) { okc = 0; break; }
            for (int i = 0; i < PL; i++) if (s->d[12 + i] != (uint8_t)(n * 7 + i)) { okc = 0; break; }
        }
        hv_pvg_lite_complete(dev, 3, (uint32_t)(100 + total - 1));
    }
    EXPECT(okc && total == 36, "wrap: 36 commands of 4 KB through a 32 KB ring arrive intact and in order (crossing page boundaries and the end)");
    EXPECT(chs[3].wr > 4 * 0x8000 && rd_ptr(3) == chs[3].wr, "wrap: the pointers ran past the ring size several times and ended equal (wr %u)", chs[3].wr);
}

static void s_bad_channel(void) {
    dev_new(0);
    chan_setup(2, 1);
    chan_raw(2, 5, 8, 31, NULL, 0, 12);   /* length field 8 < 12 */
    ring_bell(2);
    EXPECT(nsubs == 0 && rd_ptr(2) == chs[2].wr, "bad channel: a length below 12 is dropped, ring consumed (rd %u wr %u)", rd_ptr(2), chs[2].wr);
    chan_raw(2, 5, 0x20000, 32, NULL, 0, 12);   /* longer than the ring */
    ring_bell(2);
    EXPECT(nsubs == 0 && rd_ptr(2) == chs[2].wr, "bad channel: a length above the ring size is dropped");
    uint32_t pl = 9;
    chan_cmd(2, 0x40, 33, &pl, 4);
    ring_bell(2);
    EXPECT(nsubs == 1 && subs[0].len == 16, "bad channel: the next good command still arrives");
    ring_bell(0); ring_bell(15); W(0x1020, 99);
    EXPECT(nsubs == 1, "bad channel: doorbells for channel 0, an empty channel and a channel number out of range do nothing");
}

/* channel `c` gets a one-command stamp; `barrier` waits for the (bch, bstamp) pair */
static void barrier_cmd(int c, uint32_t stamp, int bch, uint32_t bstamp) {
    uint32_t pl[2] = {(uint32_t)bch, bstamp};
    chan_raw(c, 0x10000u | 0x40, 12 + 8, stamp, pl, 8, 20);
}
static void s_barrier(void) {
    uint64_t st[4];
    /* A: the other channel's command is already in its ring: it is drained first */
    dev_new(0);
    chan_setup(3, 1); chan_setup(5, 1);
    chan_cmd(3, 0x40, 100, NULL, 0);
    barrier_cmd(5, 7, 3, 100);
    ring_bell(5);
    EXPECT(nsubs == 2 && subs[0].ch == 3 && subs[1].ch == 5, "barrier A: channel 3's command (stamp 100) went first, then channel 5's (order %d,%d of %d)", subs[0].ch, subs[1].ch, nsubs);
    hv_pvg_lite_barrier_stats(dev, st);
    EXPECT(st[0] == 1 && st[1] == 1 && st[2] == 0 && st[3] == 0, "barrier A: stats barriers 1, drains 1, retries 0, timeouts 0 (%llu %llu %llu %llu)", (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3]);

    /* B: not written yet: channel 5 waits, and goes on after the other one arrives (picked up by poll) */
    dev_new(0);
    chan_setup(3, 1); chan_setup(5, 1);
    barrier_cmd(5, 7, 3, 100);
    ring_bell(5);
    hv_pvg_lite_barrier_stats(dev, st);
    EXPECT(nsubs == 0 && rd_ptr(5) == 0 && st[2] == 1, "barrier B: channel 5 is held back (nothing delivered, rd stays 0, one retry)");
    hv_pvg_lite_poll(dev);
    EXPECT(nsubs == 0, "barrier B: a poll while the stamp is still missing keeps holding");
    chan_cmd(3, 0x40, 100, NULL, 0);
    hv_pvg_lite_poll(dev);
    EXPECT(nsubs == 2 && subs[0].ch == 3 && subs[1].ch == 5 && rd_ptr(5) == chs[5].wr, "barrier B: after the stamp arrives a poll delivers 3 then 5");

    /* C: the stamp never comes: after 20000 waits it gives up instead of hanging, and says so in the statistics */
    dev_new(0);
    chan_setup(3, 1); chan_setup(5, 1);
    barrier_cmd(5, 7, 3, 100);
    ring_bell(5);
    int polls = 0;
    while (nsubs == 0 && polls < 30000) { hv_pvg_lite_poll(dev); polls++; }
    hv_pvg_lite_barrier_stats(dev, st);
    EXPECT(nsubs == 1 && subs[0].ch == 5 && st[3] == 1 && polls >= 19000 && polls <= 20100, "barrier C: gave up after %d polls, delivered channel 5's command, 1 timeout counted", polls);

    /* D: stamps compare with wrap-around: 0xfffffff8 is in the future of 0xfffffff0, and 2 is in the future of both */
    dev_new(0);
    chan_setup(3, 1); chan_setup(5, 1);
    chan_cmd(3, 0x40, 0xfffffff0u, NULL, 0);
    ring_bell(3);
    barrier_cmd(5, 7, 3, 0xfffffff8u);
    ring_bell(5);
    EXPECT(nsubs == 1 && subs[0].ch == 3, "barrier D: stamp fffffff0 is not enough for a wait on fffffff8 (held back)");
    chan_cmd(3, 0x40, 2, NULL, 0);
    hv_pvg_lite_poll(dev);
    EXPECT(nsubs == 3 && subs[1].ch == 3 && subs[2].ch == 5, "barrier D: stamp 2 (after the wrap) satisfies it");
}

static void s_display(void) {
    hv_pvg_lite_set_mode(800, 600);
    hv_pvg_lite_set_size_mm(0, 0);
    dev_new(1);
    chan_setup(4, 1);
    uint32_t dpn = alloc_page();
    uint32_t a1[2] = {0, dpn};
    chan_cmd(4, 1, 41, a1, 8);
    ring_bell(4);
    uint8_t *d = hv_pvg_lite_display_page(dev);
    EXPECT(d == pgp(dpn), "display: op 1 names the shared page");
    if (!d) return;
#define U32(o) (*(uint32_t *)(d + (o)))
    EXPECT(U32(0x00) == 1 && U32(0x200) == 1 && U32(0x208) == 1 && U32(0x210) == (600u << 16 | 800u), "display: version 1, mode-list sequence 1, one mode of 800x600 (%x)", U32(0x210));
    EXPECT(U32(0x14) == ((400u * 600 / 800) << 16 | 400u), "display: size in mm keeps the pixel density at the mode's shape");
    EXPECT(d[0xfa9] == 1 && U32(0xfc0) == 1 && U32(0x100) == 4, "display: ack pending, state 1, mode-change flag raised");
    uint32_t a2[2] = {0, 1};
    chan_cmd(4, 2, 42, a2, 8);
    ring_bell(4);
    EXPECT(U32(0xfbc) == 1 && d[0xfa9] == 0 && U32(0xfc0) == 2, "display: op 2 (ack) clears the pending flag and moves to state 2");
    uint32_t a8[10] = {0, 0, 0x1234, 0x5, 0, 0, 0, 0, 3, 0xabcd};
    chan_cmd(4, 8, 43, a8, 40);
    ring_bell(4);
    EXPECT(U32(0xf98) == 1 && U32(0xfb0) == 0x1234 && (U32(0x100) & 2), "display: op 8 (swap) counts, records the mapping id and raises the swap flag");
    chan_cmd(4, 8, 44, a8, 40);
    ring_bell(4);
    EXPECT(U32(0xf98) == 2, "display: the swap counter follows");
    EXPECT(stamp_of(4) == 44 && (Rg(0x1018) & (1u << 4)), "display: every command is stamped and acknowledged");
    /* with a backend the device leaves 1, 2, 8 to it; the backend calls hv_pvg_lite_command */
    hv_pvg_lite_set_mode(1920, 1080);
    dev_new(0);
    uint32_t dpn2 = alloc_page();
    uint32_t cmd[5] = {1, 20, 9, 0, dpn2};
    hv_pvg_lite_command(dev, cmd, 20);
    EXPECT(hv_pvg_lite_display_page(dev) == pgp(dpn2) && *(uint32_t *)(pgp(dpn2) + 0x210) == (1080u << 16 | 1920u), "display (backend path): hv_pvg_lite_command applies op 1 (1920x1080)");
    uint32_t short_cmd[3] = {8, 12, 9};
    hv_pvg_lite_command(dev, short_cmd, 12);
    EXPECT(*(uint32_t *)(pgp(dpn2) + 0xf98) == 0, "display (backend path): a swap with no arguments changes nothing");
#undef U32
}

static void s_mapper(void) {
    dev_new(0);
    chan_setup(2, 1);   /* the backend is made on the first channel command; unmaps are passed to it only then */
    chan_cmd(2, 0x40, 1, NULL, 0);
    ring_bell(2);
    EXPECT(created == 1, "mapper: (set-up) the backend exists");
    uint32_t ringp = alloc_page(), tabp = alloc_page(), descp = alloc_page();
    uint64_t rpa = (uint64_t)ringp << 14, tpa = (uint64_t)tabp << 14;
    uint32_t *e = (uint32_t *)pgp(ringp);
    e[0] = 2; e[1] = 7;      /* unmap 7 */
    e[4] = 1; e[5] = 8;      /* map 8: not an unmap */
    e[8] = 2; e[9] = 9;      /* unmap 9 */
    ((uint32_t *)pgp(tabp))[0] = (uint32_t)((((uint64_t)descp << 14) >> 12) | 1);   /* descriptors 0..15 present */
    memset(pgp(descp) + 0x400 * 3, 0xd3, 0x400);
    ios_w(ios_o, 0x1000, rpa, 8); ios_w(ios_o, 0x1008, 16, 8); ios_w(ios_o, 0x1010, tpa, 8);
    ios_w(ios_o, 0x1018, 3, 8);
    EXPECT(ios_r(ios_o, 0x1020, 8) == 3, "mapper: completed index follows the submitted one (%llu)", (unsigned long long)ios_r(ios_o, 0x1020, 8));
    EXPECT(nun == 2 && unmapped[0] == 7 && unmapped[1] == 9, "mapper: the backend was told to unmap 7 and 9, not 8 (%d: %d %d)", nun, nun > 0 ? unmapped[0] : -1, nun > 1 ? unmapped[1] : -1);
    EXPECT(pends >= 1 && last_intid == 8, "mapper: the mapper interrupt was raised");
    EXPECT(ios_r(ios_o, 0x1028, 8) == 1, "mapper: queue count reads 1");
    const uint8_t *s = hv_pvg_lite_surface(dev, 3);
    EXPECT(s == pgp(descp) + 0x400 * 3 && s[0] == 0xd3, "mapper: surface 3 is descriptor 3 of the present table entry");
    EXPECT(hv_pvg_lite_surface(dev, 16) == NULL, "mapper: a surface in an absent table entry is NULL");
}

static void s_task(void) {
    dev_new(0);
    chan_setup(2, 1);
    uint32_t troot = alloc_page(), tab = alloc_page(), d0 = alloc_page(), d1 = alloc_page();
    ((uint32_t *)pgp(troot))[0] = tab; ((uint32_t *)pgp(troot))[1] = 1;   /* one level */
    ((uint32_t *)pgp(tab))[0] = d0 | 0x80000000u; ((uint32_t *)pgp(tab))[1] = d1;   /* bit 31 is a flag */
    for (int i = 0; i < 0x4000; i++) pgp(d0)[i] = (uint8_t)i, pgp(d1)[i] = (uint8_t)(i + 0x55);
    uint32_t args[4] = {10, 0, 0, troot};   /* task id 10 >> 1 = 5 */
    chan_cmd(2, 0x38, 51, args, 16);
    ring_bell(2);
    uint8_t buf[64];
    EXPECT(hv_pvg_lite_task_read(dev, 5, 0x3fe0, buf, 64) == 0, "task: a read across the page boundary works");
    int okb = 1;
    for (int i = 0; i < 32; i++) okb &= buf[i] == (uint8_t)(0x3fe0 + i);
    for (int i = 0; i < 32; i++) okb &= buf[32 + i] == (uint8_t)(i + 0x55);
    EXPECT(okb, "task: the bytes come from the two guest pages in order (flag bit ignored)");
    EXPECT(hv_pvg_lite_task_page(dev, 5, 0x4000) == d1, "task: task_page names the guest page");
    EXPECT(hv_pvg_lite_task_read(dev, 5, 0x8000, buf, 4) == -1, "task: an unmapped page fails");
    EXPECT(hv_pvg_lite_task_read(dev, 6, 0, buf, 4) == -1 && hv_pvg_lite_task_read(dev, 600, 0, buf, 4) == -1, "task: an unknown task and a task number out of range fail");
    ((uint32_t *)pgp(troot))[1] = 4;
    EXPECT(hv_pvg_lite_task_read(dev, 5, 0, buf, 4) == -1, "task: a depth of 4 is refused");
    ((uint32_t *)pgp(troot))[1] = 0;
    EXPECT(hv_pvg_lite_task_read(dev, 5, 0, buf, 4) == -1, "task: a depth of 0 is refused");
}

/* 0x38 and 0x44 each read a fixed number of argument bytes (16, 28) regardless of how much
   the command itself declares. A command shorter than that must not be read at all: nothing
   it names is in range yet, so reading past it would pull in whatever follows in the ring
   (the next command's bytes, at the time this was written). */
static void s_len_checks(void) {
    dev_new(1);   /* no metal backend: stamps are written synchronously, as in swap_at_page_end */
    chan_setup(2, 1);

    /* A valid one-level task root (as in s_task), reused as the target of every 0x38 below:
       a successful task_read/task_page proves task_root was actually set to it. */
    uint32_t troot = alloc_page(), tab = alloc_page(), d0 = alloc_page();
    ((uint32_t *)pgp(troot))[0] = tab; ((uint32_t *)pgp(troot))[1] = 1;
    ((uint32_t *)pgp(tab))[0] = d0;

    /* 0x38, full 16 bytes (task 20 >> 1 = 10): applied, as s_task already relies on. */
    uint32_t full38[4] = {20, 0, 0, troot};
    chan_cmd(2, 0x38, 1, full38, 16);
    ring_bell(2);
    uint8_t buf[4];
    EXPECT(hv_pvg_lite_task_read(dev, 10, 0, buf, 4) == 0, "0x38 len=16: task root applied");

    /* 0x38, 8 bytes (short of the 16 it reads): must not be applied, and must not read into
       the next command's bytes in the ring. Task 22 >> 1 = 11 must stay unset. */
    uint32_t short38[2] = {22, 0};
    chan_cmd(2, 0x38, 2, short38, 8);
    uint32_t full38b[4] = {24, 0, 0, troot};   /* task 12: would be misread as task 22's tail if 0x38 over-read */
    chan_cmd(2, 0x38, 3, full38b, 16);
    ring_bell(2);
    EXPECT(hv_pvg_lite_task_read(dev, 11, 0, buf, 4) == -1, "0x38 len=8 (< 16): not applied, task root stays unset");
    EXPECT(hv_pvg_lite_task_read(dev, 12, 0, buf, 4) == 0, "0x38 len=8 followed by a full 0x38: the ring moved on correctly, the next command still applies");
    EXPECT(stamp_of(2) == 3, "0x38 len=8: the short command was still consumed and stamped (ring position untouched by the length check)");

    /* 0x44, full 28 bytes (task 30): applied. */
    uint32_t full44[7] = {30, 0x1111, 0, 0x2222, 0, 0, 0};
    chan_cmd(2, 0x44, 4, full44, 28);
    ring_bell(2);
    EXPECT(hv_pvg_lite_task_objects(dev, 30) == 0x1111, "0x44 len=28: task objects applied");

    /* 0x44, 4 bytes (short of the 28 it reads, as tests/test_pvg.c's own backend-order test at
       "chan_cmd(2, 0x44, 32, a, 4)" already sends without checking this). Task 31 must stay 0. */
    uint32_t short44[1] = {31};
    chan_cmd(2, 0x44, 5, short44, 4);
    uint32_t full44b[7] = {32, 0x3333, 0, 0x4444, 0, 0, 0};   /* would be misread as task 31's tail if 0x44 over-read */
    chan_cmd(2, 0x44, 6, full44b, 28);
    ring_bell(2);
    EXPECT(hv_pvg_lite_task_objects(dev, 31) == 0, "0x44 len=4 (< 28): not applied, task objects stays 0");
    EXPECT(hv_pvg_lite_task_objects(dev, 32) == 0x3333, "0x44 len=4 followed by a full 0x44: the ring moved on correctly, the next command still applies");
    EXPECT(stamp_of(2) == 6, "0x44 len=4: the short command was still consumed and stamped");
}

/* ---- random content in well-formed rings: must not crash (ASan / UBSan builds are what makes this mean something). The commands are chained from the start of the
   ring and the write pointer is the end of the chain: a pointer the walk can never reach is covered by s_ring below, so the chains here are kept simple. ---- */
static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint32_t noise0;
static uint32_t rnd_page(void) { return rnd() % 5 ? noise0 + rnd() % 24 : rnd(); }   /* mostly the noise pages below, sometimes anywhere (nearly always outside the RAM) */
static void fuzz_cmd(uint32_t *w, uint32_t words) {
    static const uint32_t ops[] = {0x38, 0x3a, 0x44, 0x43, 0x45, 0x25, 0x28, 1, 2, 8, 0x40};
    w[0] = rnd() % 4 ? ops[rnd() % (sizeof ops / sizeof ops[0])] : rnd() & 0xffff;
    if (rnd() % 8 == 0) w[0] |= (rnd() % 3) << 16;                           /* barriers */
    w[1] = words * 4; w[2] = rnd();
    for (uint32_t i = 3; i < words; i++) w[i] = rnd() & 1 ? rnd_page() : rnd() % 600;
}
static void s_fuzz(int iters) {
    for (int it = 0; it < iters; it++) {
        dev_new(it & 1);
        for (int c = 1; c < 6; c++) chan_setup(c, 1 + (int)(rnd() % 2));
        noise0 = next_page;   /* guest memory the random page numbers point at: noise, not rings */
        for (int q = 0; q < 24; q++) { uint32_t pn = alloc_page(); for (int i = 0; i < 0x4000; i += 4) *(uint32_t *)(pgp(pn) + i) = rnd() & 1 ? rnd() : rnd() % 64; }
        for (int c = 1; c < 6; c++) {
            uint32_t total = 0, limit = (chs[c].pages << 14) - 256;
            while (total < limit && rnd() % 40) {
                uint32_t words = 3 + rnd() % 14, w[17];
                fuzz_cmd(w, words);
                for (uint32_t i = 0; i < words * 4; i++) *ring_at(c, total + i) = ((uint8_t *)w)[i];
                total += words * 4;
            }
            chs[c].wr = total;
            __atomic_store_n(&rec(c)[0], total, __ATOMIC_RELEASE);
        }
        { uint32_t total = 0; while (total < 0x4000 - 256 && rnd() % 40) { uint32_t words = 3 + rnd() % 14, w[17]; fuzz_cmd(w, words); for (uint32_t i = 0; i < words * 4; i++) *(pgp(fifo_pn + 1) + (total + i)) = ((uint8_t *)w)[i]; total += words * 4; } root_wp = total; W(0x1008, total); }
        if (it % 2) {   /* arbitrary pointers: the walk must end whatever they are */
            for (int c = 1; c < 6; c++) __atomic_store_n(&rec(c)[0], rnd() & 1 ? rnd() : rnd() % 0x8000, __ATOMIC_RELEASE);
            W(0x1008, rnd() & 1 ? rnd() : rnd() % 0x8000);
        }
        for (int k = 0; k < 8; k++) { (void)Rg(0x100c); W(0x1020, 1 + rnd() % 6); hv_pvg_lite_poll(dev); (void)Rg(0x1018); }
        { uint32_t cmdw[64] = {0}; fuzz_cmd(cmdw, 16); hv_pvg_lite_command(dev, cmdw, 64); }   /* a buffer with slack: the short-command over-read has its own probe */
        ios_w(ios_o, 0x1000, RB + (rnd() % 0x1000) * 16ull, 8); ios_w(ios_o, 0x1008, rnd() % 64, 8); ios_w(ios_o, 0x1010, RB + (rnd() % 0x800) * 4ull, 8);
        ios_w(ios_o, 0x1018, rnd() % 40, 8);
        (void)hv_pvg_lite_surface(dev, rnd() % 4000);
        uint8_t b[32];
        (void)hv_pvg_lite_task_read(dev, rnd() % 600, ((uint64_t)rnd() << 8) | rnd() % 256, b, sizeof b);
        (void)hv_pvg_lite_task_page(dev, rnd() % 600, ((uint64_t)rnd() << 8) | rnd() % 256);
        if (nsubs) for (int i = 0; i < nsubs && i < 8; i++) hv_pvg_lite_complete(dev, (uint32_t)subs[i].ch, 1 + rnd() % 100);
    }
    EXPECT(1, "fuzz: %d random commands, odd runs also with arbitrary pointers (even/odd NO_METAL/backend alternate separately), no crash and no loop", iters);
}

/* ---- pointers that do not describe whole commands. The walk used to compare "rd != wr" for equality, so a write pointer off a command boundary behind a ring of
   well-formed commands was never reached (a loop the guest controls). It now takes only commands that lie entirely inside wr - rd and drops the rest. ---- */
static void fill16(int root) {   /* a ring full of 16-byte commands: 16 divides the ring, so the walk is periodic */
    for (int i = 0; i < 0x4000 / 16; i++) {
        uint32_t w[4] = {0x77, 16, (uint32_t)(i + 1), 0};
        memcpy(root ? pgp(fifo_pn + 1) + 16 * i : ring_at(2, 16 * i), w, 16);
    }
}
/* Runs one case in a child with a timer: a regression back to a loop fails here instead of hanging the job. Returns 1 when the child finished. */
static int finishes(void (*fn)(int), int arg) {
    pid_t pid = fork();
    if (pid == 0) { alarm(5); fn(arg); _exit(0); }
    int stt = 0;
    waitpid(pid, &stt, 0);
    return WIFEXITED(stt) && WEXITSTATUS(stt) == 0;
}
static void walk_periodic(int root) {
    dev_new(1);
    if (!root) chan_setup(2, 1);
    fill16(root);
    if (root) { W(0x1008, 17); (void)Rg(0x100c); } else { __atomic_store_n(&rec(2)[0], 17u, __ATOMIC_RELEASE); ring_bell(2); }
}
static void walk_aligned(int root) {   /* control: the same ring, a pointer on a boundary */
    dev_new(1);
    if (!root) chan_setup(2, 1);
    fill16(root);
    if (root) { W(0x1008, 32); (void)Rg(0x100c); } else { __atomic_store_n(&rec(2)[0], 32u, __ATOMIC_RELEASE); ring_bell(2); }
}
static void s_ring(void) {
    for (int root = 0; root < 2; root++) {
        const char *n = root ? "root" : "channel";
        EXPECT(finishes(walk_aligned, root), "%s ring: a pointer on a command boundary returns (control)", n);
        EXPECT(finishes(walk_periodic, root), "%s ring: a pointer off a command boundary (17, behind 16-byte commands) returns", n);
    }
    /* channel ring cases, in process (the global timer catches a loop) */
    dev_new(0); chan_setup(2, 1); fill16(0);
    __atomic_store_n(&rec(2)[0], 17u, __ATOMIC_RELEASE); ring_bell(2);
    EXPECT(nsubs == 1 && subs[0].len == 16 && rd_ptr(2) == 17, "channel ring: pointer 17 behind 16-byte commands: the first whole command is delivered, the 1 byte left is dropped (rd %u)", rd_ptr(2));
    dev_new(0); chan_setup(2, 1);
    chan_raw(2, 0x40, 16, 1, (uint32_t[]){0}, 4, 16);
    __atomic_store_n(&rec(2)[0], 21u, __ATOMIC_RELEASE);   /* one command and 5 bytes: less than a header */
    ring_bell(2);
    EXPECT(nsubs == 1 && rd_ptr(2) == 21, "channel ring: 5 bytes after a command (less than a header) are dropped, the command before them is delivered");
    dev_new(0); chan_setup(2, 1);
    chan_raw(2, 0x40, 64, 1, (uint32_t[]){0}, 4, 32);   /* the header says 64 bytes, 32 are pending */
    ring_bell(2);
    EXPECT(nsubs == 0 && rd_ptr(2) == 32, "channel ring: a command longer than what is pending is dropped, not delivered");
    dev_new(0); chan_setup(2, 1);
    chan_raw(2, 0x40, 16, 1, (uint32_t[]){0}, 4, 16);
    __atomic_store_n(&rec(2)[0], 0x4000u + 100u, __ATOMIC_RELEASE);   /* more pending than the ring holds */
    ring_bell(2);
    EXPECT(nsubs == 0 && rd_ptr(2) == 0x4000u + 100u, "channel ring: more pending than the ring holds: everything is dropped (rd %u)", rd_ptr(2));
    dev_new(0); chan_setup(2, 1);
    rec(2)[1] = 0xffffffd0u; chs[2].wr = 0xffffffd0u; __atomic_store_n(&rec(2)[0], 0xffffffd0u, __ATOMIC_RELEASE);
    for (int k = 0; k < 4; k++) chan_cmd(2, 0x40, (uint32_t)(60 + k), (uint32_t[5]){0}, 20);   /* 4 x 32 bytes: crosses 2^32 */
    ring_bell(2);
    EXPECT(nsubs == 4 && rd_ptr(2) == chs[2].wr && chs[2].wr < 0x100, "channel ring: the counters wrap past 2^32 (rd %x, wr %x), all 4 commands delivered", rd_ptr(2), chs[2].wr);
    /* root ring */
    dev_new(1);
    root_cmd(0x3b, 12, 0x201, NULL, 0);
    uint32_t good = (uint32_t)Rg(0x100c);
    W(0x1008, root_wp + 5);   /* 5 bytes: less than a header */
    EXPECT((uint32_t)Rg(0x100c) == root_wp + 5 && good == 12 && stamp_of(0) == 0x201, "root ring: 5 bytes after a good command are dropped, the command before them was done");
    dev_new(1);
    { uint32_t *r = (uint32_t *)pgp(fifo_pn + 1); r[0] = 0x3b, r[1] = 64, r[2] = 0x202; root_wp = 32; W(0x1008, root_wp); }   /* the header says 64, 32 are pending */
    EXPECT((uint32_t)Rg(0x100c) == 32 && stamp_of(0) == 0, "root ring: a command longer than what is pending is dropped (no stamp)");
    dev_new(1);
    W(0x1008, 0x4000u + 100u);
    EXPECT((uint32_t)Rg(0x100c) == 0x4000u + 100u && stamp_of(0) == 0, "root ring: more pending than the ring holds: dropped");
    dev_new(1);
    W(0x1008, 0xfffffff0u);   /* a 'negative' distance */
    EXPECT((uint32_t)Rg(0x100c) == 0xfffffff0u, "root ring: a huge pending count is dropped at once");
}

/* A swap is 10 words (40 bytes of arguments). A shorter one is refused on both paths; before, hv_pvg_lite_command accepted 16 bytes and display_swap read 40. */
static void swap_at_page_end(int arg_bytes) {
    dev_new(1);
    uint32_t dpn = alloc_page();
    uint32_t c1[5] = {1, 20, 1, 0, dpn};
    hv_pvg_lite_command(dev, c1, 20);
    long pgsz = sysconf(_SC_PAGESIZE);
    uint8_t *m = mmap(NULL, 2 * pgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m + pgsz, pgsz, PROT_NONE) != 0) _exit(3);
    uint32_t len = 12 + (uint32_t)arg_bytes;
    uint32_t *cmd = (uint32_t *)(m + pgsz - len);   /* ends exactly where the protected page begins */
    memset(cmd, 0, len);
    cmd[0] = 8, cmd[1] = len, cmd[2] = 5, cmd[5] = 0x77;
    hv_pvg_lite_command(dev, cmd, len);
    uint32_t swaps = *(uint32_t *)(pgp(dpn) + 0xf98);
    if ((arg_bytes >= 40) != (swaps == 1)) _exit(4);   /* a full swap counts, a short one does not */
}
static void s_swap(void) {
    EXPECT(finishes(swap_at_page_end, 40), "swap with all 40 bytes of arguments at the end of a page is applied");
    EXPECT(finishes(swap_at_page_end, 16), "swap with 16 bytes of arguments at the end of a page: refused, nothing read past the command");
    EXPECT(finishes(swap_at_page_end, 36), "swap with 36 bytes (one word short): refused");
    /* the channel path (no backend) refuses short swaps too, and still applies a full one */
    hv_pvg_lite_set_mode(800, 600);
    dev_new(1);
    chan_setup(4, 1);
    uint32_t dpn = alloc_page();
    uint32_t a1[2] = {0, dpn};
    chan_cmd(4, 1, 41, a1, 8); ring_bell(4);
    uint32_t a8[10] = {0, 0, 0x1234, 0x5, 0, 0, 0, 0, 3, 0xabcd};
    chan_cmd(4, 8, 42, a8, 16); ring_bell(4);
    EXPECT(*(uint32_t *)(pgp(dpn) + 0xf98) == 0, "swap on the channel path with 16 bytes: refused (no swap counted)");
    chan_cmd(4, 8, 43, a8, 40); ring_bell(4);
    EXPECT(*(uint32_t *)(pgp(dpn) + 0xf98) == 1 && *(uint32_t *)(pgp(dpn) + 0xfb0) == 0x1234, "swap on the channel path with 40 bytes: applied");
    EXPECT(stamp_of(4) == 43, "the refused command was still consumed and stamped, the ring went on");
    hv_pvg_lite_set_mode(1920, 1080);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    ram = aligned_alloc(0x4000, RSZ);
    if (!ram) return 2;
    const char *sc = argc > 1 ? argv[1] : "all";
    int all = !strcmp(sc, "all");
    alarm(120);   /* a loop in the device must fail the run, not hang it */
    if (all || !strcmp(sc, "root")) s_root();
    if (all || !strcmp(sc, "channel")) s_channel_nometal(), s_channel_backend(), s_bad_channel();
    if (all || !strcmp(sc, "backend")) s_backend();
    if (all || !strcmp(sc, "complete")) s_complete();
    if (all || !strcmp(sc, "chrange")) s_chrange();
    if (all || !strcmp(sc, "wrap")) s_wrap();
    if (all || !strcmp(sc, "barrier")) s_barrier();
    if (all || !strcmp(sc, "display")) s_display();
    if (all || !strcmp(sc, "mapper")) s_mapper(), s_task(), s_len_checks();
    if (all || !strcmp(sc, "fuzz")) s_fuzz(argc > 2 ? atoi(argv[2]) : 400);
    if (all || !strcmp(sc, "ring")) s_ring();
    if (all || !strcmp(sc, "swap")) s_swap();
    printf("%s test_pvg %s\n", bad ? "FAIL" : "PASS", sc);
    return bad ? 1 : 0;
}
