#ifndef HENGEVM_PVG_LAGFX_H
#define HENGEVM_PVG_LAGFX_H

#include <stdint.h>

#include "gicv3.h"

/* S7 of docs/gpu-research/implementation-plan.md, first part: the glue between Henge's machine (guest memory, interrupt) and
   libapplegfx-vulkan's device (lagfx_device_t). libapplegfx-vulkan reads the same register layout as devices/pvg_lite.c (primary ring
   doorbell +0x1008, status +0x1018, channel doorbell +0x1020, +0x122c), so it takes over the whole "gfx" MMIO window rather than being a
   HvPvgBackend behind pvg_lite's own command decoder.

   NOT built into anything yet: no Makefile, CMake file or workflow lists devices/pvg_lagfx.c, and pvg_lite.c does not call it. It is
   compiled only with -DHV_WITH_LAGFX and the library's headers on the include path (see docs/gpu-research/s7-integration-design.md). */
typedef struct HvPvgLagfx HvPvgLagfx;

/* NULL if the library refuses (no Vulkan, no device, out of memory); *why then holds a short text (static storage, may be NULL). */
HvPvgLagfx *hv_pvg_lagfx_create(HvMachine *m, HvGic *gic, int gfx_irq, const char **why);
void hv_pvg_lagfx_destroy(HvPvgLagfx *g);
/* The gfx window (offset from its base). Same shape as the handlers in pvg_lite.c. */
uint64_t hv_pvg_lagfx_read(HvPvgLagfx *g, uint64_t off);
void hv_pvg_lagfx_write(HvPvgLagfx *g, uint64_t off, uint64_t value);
/* Root ring pointers as the library holds them. Diagnostics only: reads nothing from the guest and runs no ring. */
void hv_pvg_lagfx_root_ptrs(HvPvgLagfx *g, uint32_t *wp, uint32_t *rp);
/* Where the library's Display ops (1/2/8) go: raw command words and byte length, 4-byte aligned. Set once, before the guest runs. */
void hv_pvg_lagfx_set_display_hook(HvPvgLagfx *g, void (*hook)(void *opaque, const uint32_t *cmd, uint32_t len), void *opaque);

#endif
