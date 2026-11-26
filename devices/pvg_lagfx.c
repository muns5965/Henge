#ifdef HV_WITH_LAGFX
#include "pvg_lagfx.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libapplegfx-vulkan.h"
#include "machine.h"

struct HvPvgLagfx {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq;
    lagfx_device_t *dev;
    char *error;
    void (*display_hook)(void *opaque, const uint32_t *cmd, uint32_t len);
    void *display_opaque;
    const uint8_t *(*surface_hook)(void *opaque, uint32_t mapping);
    void *surface_opaque;
    volatile int closing;
};

/* Guest memory is reached with the address the library computed, unchanged. (An earlier version added the RAM base to any address below it; the
   rings turned out to be 16 KiB-page numbers, so that read the wrong memory.) A failed lookup is logged a few times, with the first bytes at "address +
   RAM base" for comparison only: nothing is read from or written to that address on the real path. */
#define LAGFX_RAM_BASE 0x70000000ull
static void *lagfx_ptr(HvPvgLagfx *g, uint64_t gpa, uint64_t length, uint64_t *used) {
    void *p = hv_phys_ptr(g->m, gpa, length);
    *used = gpa;
    if (p) return p;
    static int failed;
    if (failed < 16) {
        failed++;
        const uint8_t *alt = gpa < LAGFX_RAM_BASE ? hv_phys_ptr(g->m, gpa + LAGFX_RAM_BASE, length < 16 ? length : 16) : NULL;
        fprintf(stderr, "hengevm: lagfx: gpa %#llx len %llu: no RAM there", (unsigned long long)gpa, (unsigned long long)length);
        if (alt) {
            fprintf(stderr, " (for comparison only, at %#llx:", (unsigned long long)(gpa + LAGFX_RAM_BASE));
            for (uint64_t i = 0; i < (length < 16 ? length : 16); i++) fprintf(stderr, " %02x", alt[i]);
            fputc(')', stderr);
        }
        fputc('\n', stderr);
    }
    return NULL;
}

/* Guest memory is reached through hv_phys_ptr(): a host pointer, or NULL for an address with no RAM behind it (or one that crosses the end of a region). */
static bool cb_read_memory(void *opaque, uint64_t gpa, uint64_t length, void *dst) {
    HvPvgLagfx *g = opaque;
    if (!dst || length == 0) return length == 0;
    uint64_t used;
    const void *src = lagfx_ptr(g, gpa, length, &used);
    if (!src) return false;
    memcpy(dst, src, length);
    return true;
}

static bool cb_write_memory(void *opaque, uint64_t gpa, uint64_t length, const void *srcbuf) {
    HvPvgLagfx *g = opaque;
    if (!srcbuf || length == 0) return length == 0;
    uint64_t used;
    void *dst = lagfx_ptr(g, gpa, length, &used);
    if (!dst) return false;
    memcpy(dst, srcbuf, length);
    hv_dma_written(g->m, used, length);
    return true;
}

static lagfx_task_t *cb_create_task(void *opaque, uint64_t vm_size, void **base_out) {
    (void)opaque;
    if (vm_size == 0 || vm_size > (uint64_t)SIZE_MAX) return NULL;
    return lagfx_task_create((size_t)vm_size, base_out);
}

static void cb_destroy_task(void *opaque, lagfx_task_t *task) {
    (void)opaque;
    lagfx_task_destroy(task);
}

/* COPY, NOT ALIAS (the first version, on purpose). Apple's mapMemory aliases the guest's pages into the task, so a GPU write shows up in the
   guest and a guest write shows up for the GPU. lagfx_task_map_host_memory() copies the bytes in once. That is enough for what the guest
   hands the GPU to read (command buffers, textures, shader libraries) and wrong for anything the GPU writes into guest-visible memory or
   the guest changes later. Aliasing needs guest RAM to be a shared (memfd) mapping; whether the Android runner has one is a question the
   design document lists first. Until then a mapping failure or a shortfall is reported to the library as `false`, never hidden. */
static bool cb_map_memory(void *opaque, lagfx_task_t *task, uint64_t virtual_offset, const lagfx_physical_range_t *ranges, size_t range_count, bool read_only) {
    HvPvgLagfx *g = opaque;
    uint64_t at = virtual_offset;
    for (size_t i = 0; i < range_count; i++) {
        uint64_t used;
        void *host = lagfx_ptr(g, ranges[i].guest_physical_address, ranges[i].length, &used);
        if (!host) return false;
        if (!lagfx_task_map_host_memory(task, at, host, ranges[i].length, read_only)) return false;
        at += ranges[i].length;
    }
    return true;
}

static bool cb_unmap_memory(void *opaque, lagfx_task_t *task, uint64_t virtual_offset, uint64_t length) {
    (void)opaque;
    return lagfx_task_unmap(task, virtual_offset, length);
}

/* Raised from whichever thread the library completes work on; hv_gicv3_pend() is the call pvg_lite.c makes from the CPU that rang. */
static void cb_raise_interrupt(void *opaque, uint32_t vector) {
    HvPvgLagfx *g = opaque;
    (void)vector;   /* this machine has one gfx interrupt line */
    hv_gicv3_pend(g->gic, g->gfx_irq);
}

/* Display ops 1/2/8 of the ARM64 protocol (set-page, ack, swap). The library hands them over as the guest wrote them; pvg_lite.c's own decoder owns
   the shared display page and the swapped-surface state that the Surface frame path reads. The library still completes the stamp and raises the
   interrupt for the command, so nothing here touches status or IRQ. */
static void cb_display_command(void *opaque, uint32_t chan_id, const uint8_t *cmd, uint32_t len) {
    HvPvgLagfx *g = opaque;
    (void)chan_id;
    if (!g->display_hook || !cmd || len < 12 || len > 4096) return;
    uint32_t aligned[1024];           /* the ring bytes carry no alignment promise */
    memcpy(aligned, cmd, len);
    g->display_hook(g->display_opaque, aligned, len);
}

/* The library asks for a surface descriptor (mapping id -> the 0x400-byte slot the guest's mapper table points at) when it builds a render target that
   lives in a guest display surface. The slot is copied; nothing is kept and the library is not re-entered. */
static bool cb_get_surface_desc(void *opaque, uint32_t mapping_id, uint8_t *out, size_t cap, size_t *len_out) {
    HvPvgLagfx *g = opaque;
    if (!g || !out || !g->surface_hook) return false;
    const uint8_t *d = g->surface_hook(g->surface_opaque, mapping_id);
    if (!d) return false;
    size_t n = cap < 0x400 ? cap : 0x400;
    memcpy(out, d, n);
    if (len_out) *len_out = n;
    return true;
}

void hv_pvg_lagfx_set_surface_hook(HvPvgLagfx *g, const uint8_t *(*hook)(void *opaque, uint32_t mapping), void *opaque) {
    if (!g) return;
    g->surface_hook = hook;
    g->surface_opaque = opaque;
}

void hv_pvg_lagfx_set_display_hook(HvPvgLagfx *g, void (*hook)(void *opaque, const uint32_t *cmd, uint32_t len), void *opaque) {
    if (!g) return;
    g->display_hook = hook;
    g->display_opaque = opaque;
}

/* The runner returns from main() without destroying the PVG device, so the library's teardown statistics (g7-gpu-final) were never printed. An exit
   handler prints them: statistics only, nothing is freed or torn down (the device may still be in use by a vCPU thread at that moment). */
static HvPvgLagfx *volatile s_exit_g;
static void pvg_lagfx_at_exit(void) {
    HvPvgLagfx *g = s_exit_g;
    if (g && !g->closing && g->dev) lagfx_device_diag_final(g->dev);
}

HvPvgLagfx *hv_pvg_lagfx_create(HvMachine *m, HvGic *gic, int gfx_irq, const char **why) {
    static char reason[160];
    HvPvgLagfx *g = calloc(1, sizeof *g);
    if (!g) { if (why) *why = "out of memory"; return NULL; }
    g->m = m; g->gic = gic; g->gfx_irq = gfx_irq;
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof d);
    d.shell.opaque = g;
    d.shell.create_task = cb_create_task;
    d.shell.destroy_task = cb_destroy_task;
    d.shell.map_memory = cb_map_memory;
    d.shell.unmap_memory = cb_unmap_memory;
    d.shell.read_memory = cb_read_memory;
    d.shell.write_memory = cb_write_memory;
    d.shell.raise_interrupt = cb_raise_interrupt;
    d.shell.display_command = cb_display_command;
    d.shell.get_surface_desc = cb_get_surface_desc;
    /* This machine's guest is the ARM64 vmapple one: cumulative root-ring pointer, Apple's Device Info table, 0x25/0x20 as Apple's pvg_metal.m reads them. */
    d.protocol_mode = LAGFX_PROTO_ARM64_VMAPPLE;
    d.mmio_region_size = 0x10000;   /* the gfx window the machine registered */
    d.thread_count = 0;             /* library default */
    /* The guest's ring and shared-page numbers are 16 KiB page numbers (devices/pvg_lite.c shifts them by 14). A test setting until a real command has been
       processed on the phone; 12 gives the library's old 4 KiB reading. */
#ifndef HV_PVG_LAGFX_RING_PAGE_SHIFT
#define HV_PVG_LAGFX_RING_PAGE_SHIFT 14
#endif
    d.ring_page_shift = HV_PVG_LAGFX_RING_PAGE_SHIFT;
    char *err = NULL;
    g->dev = lagfx_device_new(&d, &err);
    if (!g->dev) {
        snprintf(reason, sizeof reason, "lagfx_device_new failed: %s", err ? err : "no reason given");
        free(err);
        if (why) *why = reason;
        free(g);
        return NULL;
    }
    {
        static int registered;
        s_exit_g = g;
        if (!registered) { registered = 1; atexit(pvg_lagfx_at_exit); }
    }
    return g;
}

void hv_pvg_lagfx_destroy(HvPvgLagfx *g) {
    if (!g) return;
    g->closing = 1; /* the tick may still call poll: it stops here, before the device goes away */
    if (s_exit_g == g) s_exit_g = NULL;
    lagfx_device_free(g->dev);
    free(g);
}

void hv_pvg_lagfx_root_ptrs(HvPvgLagfx *g, uint32_t *wp, uint32_t *rp) {
    uint32_t w = 0, r = 0;
    if (g) lagfx_diag_root_ptrs(g->dev, &w, &r);
    if (wp) *wp = w;
    if (rp) *rp = r;
}

uint64_t hv_pvg_lagfx_read(HvPvgLagfx *g, uint64_t off) { return g ? lagfx_mmio_read(g->dev, off) : 0; }

void hv_pvg_lagfx_write(HvPvgLagfx *g, uint64_t off, uint64_t value) {
    if (g) lagfx_mmio_write(g->dev, off, (uint32_t)value);
}

/* Called from the CPU 0 tick (which keeps running while the guest sleeps in WFI, at most 10 ms apart). Does nothing unless GPU work is
   still pending; retiring a stamp raises the interrupt through cb_raise_interrupt. */
void hv_pvg_lagfx_poll(HvPvgLagfx *g) {
    if (!g || g->closing || !g->dev) return;
    lagfx_device_diag_tick(g->dev);   /* statistics must not depend on guest MMIO (idle guest = none) */
    if (lagfx_device_pending_completions(g->dev) != 0) lagfx_device_poll_completions(g->dev);
}
#endif /* HV_WITH_LAGFX */
