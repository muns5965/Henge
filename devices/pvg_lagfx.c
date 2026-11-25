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
};

/* Guest memory is reached through hv_phys_ptr(): a host pointer, or NULL for an address with no RAM behind it (or one that crosses the end of a region). */
static bool cb_read_memory(void *opaque, uint64_t gpa, uint64_t length, void *dst) {
    HvPvgLagfx *g = opaque;
    if (!dst || length == 0) return length == 0;
    const void *src = hv_phys_ptr(g->m, gpa, length);
    if (!src) return false;
    memcpy(dst, src, length);
    return true;
}

static bool cb_write_memory(void *opaque, uint64_t gpa, uint64_t length, const void *srcbuf) {
    HvPvgLagfx *g = opaque;
    if (!srcbuf || length == 0) return length == 0;
    void *dst = hv_phys_ptr(g->m, gpa, length);
    if (!dst) return false;
    memcpy(dst, srcbuf, length);
    hv_dma_written(g->m, gpa, length);
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
        void *host = hv_phys_ptr(g->m, ranges[i].guest_physical_address, ranges[i].length);
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
    d.mmio_region_size = 0x10000;   /* the gfx window the machine registered */
    d.thread_count = 0;             /* library default */
    char *err = NULL;
    g->dev = lagfx_device_new(&d, &err);
    if (!g->dev) {
        snprintf(reason, sizeof reason, "lagfx_device_new failed: %s", err ? err : "no reason given");
        free(err);
        if (why) *why = reason;
        free(g);
        return NULL;
    }
    return g;
}

void hv_pvg_lagfx_destroy(HvPvgLagfx *g) {
    if (!g) return;
    lagfx_device_free(g->dev);
    free(g);
}

uint64_t hv_pvg_lagfx_read(HvPvgLagfx *g, uint64_t off) { return g ? lagfx_mmio_read(g->dev, off) : 0; }

void hv_pvg_lagfx_write(HvPvgLagfx *g, uint64_t off, uint64_t value) {
    if (g) lagfx_mmio_write(g->dev, off, (uint32_t)value);
}
#endif /* HV_WITH_LAGFX */
