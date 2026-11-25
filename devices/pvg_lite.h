#ifndef HENGEVM_PVG_LITE_H
#define HENGEVM_PVG_LITE_H

#include <stddef.h>
#include <stdint.h>

#include "gicv3.h"

/* HengeVM's own graphics device, for hosts without
   ParavirtualizedGraphics.framework (iOS). It speaks the protocol of Apple's
   guest driver (AppleParavirtGPU) so the driver starts and its commands
   complete; pvg_metal.m runs what they draw. */
typedef struct HvPvgLite HvPvgLite;

HvPvgLite *hv_pvg_lite_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq);
/* Completes doorbells rung since the last call. Device lock held. */
void hv_pvg_lite_poll(HvPvgLite *p);
/* The newest frame, BGRA, width * 4 bytes per row, if drawing has produced one. Returns 0 if so. */
int hv_pvg_lite_frame(HvPvgLite *p, const uint8_t **pixels, uint32_t *width, uint32_t *height);

/* Between the device and its Metal side (pvg_metal.m). */
typedef struct HvPvgMetal HvPvgMetal;
HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite);
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t channel, uint32_t *command, uint32_t length);
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t width, uint32_t height);
int hv_pvg_lite_snapshot(HvPvgLite *p, uint8_t *bgra, uint32_t width, uint32_t height);
int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **pixels, uint32_t *width, uint32_t *height);
int hv_pvg_lite_task_read(HvPvgLite *p, uint32_t task, uint64_t offset, void *to, uint32_t length);
uint32_t hv_pvg_lite_task_page(HvPvgLite *p, uint32_t task, uint64_t offset);
uint8_t *hv_pvg_lite_page(HvPvgLite *p, uint32_t guest_page);
void *hv_pvg_lite_phys(HvPvgLite *p, uint64_t pa, uint64_t length);
uint64_t hv_pvg_lite_task_objects(HvPvgLite *p, uint32_t task);
/* A surface's descriptor (1 KB), or NULL if the mapper has none for it. */
const uint8_t *hv_pvg_lite_surface(HvPvgLite *p, uint32_t mapping);
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping);
void hv_pvg_metal_cursor_moved(HvPvgMetal *m);
/* Frames go to this CAMetalLayer from now on, drawn by the GPU. Any thread. */
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer);
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m);
/* The function table between the device and whatever draws (B1-b). The device speaks the guest's protocol; the backend is handed what was drawn-for and runs it.
   Ownership: `command` passed to submit is a malloc'd copy that belongs to the backend from the call on; it frees it (or hands it on) exactly once, on any thread.
   Order: submit is called in the order the device accepted the commands (barriers already honoured); the backend completes work in that order and reports each
   stamp with hv_pvg_lite_complete(). `create` and `submit` are required, everything else may be NULL (then: nothing happens / -1 / 0). `create` returning NULL is
   a failed backend, which is not the same as no backend: see hv_pvg_lite_backend_state. The default is Apple's: pvg_metal.m, or host/pvg_metal_stub.c where there is no Metal. */
typedef struct HvPvgBackend {
    const char *name;
    HvPvgMetal *(*create)(HvPvgLite *lite);
    void (*submit)(HvPvgMetal *m, HvPvgLite *lite, uint32_t channel, uint32_t *command, uint32_t length);
    int (*snapshot)(HvPvgMetal *m, uint8_t *bgra, uint32_t width, uint32_t height);
    int (*frame)(HvPvgMetal *m, const uint8_t **pixels, uint32_t *width, uint32_t *height);
    void (*unmap)(HvPvgMetal *m, uint32_t mapping);
    void (*cursor_moved)(HvPvgMetal *m);
    void (*set_layer)(HvPvgMetal *m, void *layer);
    uint64_t (*gpu_frames)(HvPvgMetal *m);
} HvPvgBackend;
enum { HV_PVG_BACKEND_NOT_TRIED = 0, HV_PVG_BACKEND_NONE = 1, HV_PVG_BACKEND_FAILED = 2, HV_PVG_BACKEND_OK = 3 };
const HvPvgBackend *hv_pvg_apple_backend(void);
/* Before the first channel command only (the backend is made lazily on it). Returns 0, or -1 for a table without create/submit or when it is too late. */
int hv_pvg_lite_set_backend(HvPvgLite *p, const HvPvgBackend *backend);
/* NOT_TRIED until the first channel command; NONE if HV_PVG_NO_METAL is set; FAILED if create returned NULL; OK otherwise. */
int hv_pvg_lite_backend_state(HvPvgLite *p);
void hv_pvg_lite_task_pages(HvPvgLite *p, uint32_t task, uint64_t first, uint64_t n, uint32_t *out);
void hv_pvg_lite_set_layer(HvPvgLite *p, void *layer);
uint64_t hv_pvg_lite_gpu_frames(HvPvgLite *p);
void hv_pvg_lite_barrier_stats(HvPvgLite *p, uint64_t out[4]);
/* Diagnostic only: one line of per-channel ring pointers and doorbell counts (see devices/pvg_lite.c). Returns its length. */
size_t hv_pvg_lite_chan_diag(HvPvgLite *p, char *out, size_t cap);
/* The display's shared state page, or NULL before the guest named it. */
uint8_t *hv_pvg_lite_display_page(HvPvgLite *p);
void hv_pvg_lite_command(HvPvgLite *p, const uint32_t *command, uint32_t length);
/* Valid channels are 1 .. 15; anything else is ignored (no stamp, no status bit, no interrupt). */
void hv_pvg_lite_complete(HvPvgLite *p, uint32_t channel, uint32_t stamp);
/* How many ignored completions have been logged so far (stops at 8). */
uint32_t hv_pvg_lite_bad_complete_logged(void);

/* The size macOS is told its screen is. Set before the machine starts; default 1920 x 1080. */
void hv_pvg_lite_set_mode(uint32_t width, uint32_t height);
void hv_pvg_lite_set_size_mm(uint32_t width_mm, uint32_t height_mm);

/* HV_PVG_TALLY diagnostic (read-only; set at creation time, before hv_pvg_lite_create -- see p->tally in
   pvg_lite.c): a snapshot of the per-encoder-type command tally tally_exec()/tally_buffer() have accumulated
   since creation, for a caller that wants the final counts even if the run ends before tally_exec()'s own
   periodic (execs % 2000 == 0) dump ever fires -- which a short, bounded diagnostic run (such as one G6
   attempt) usually will. counts_out, if non-NULL, must point at 6*256 uint64_t (the same [type][opcode] shape
   tally_buffer() keeps internally: type 0 render, 1 compute, 2 blit, 4 info, per that function's own comment).
   All-zero (execs and bad left at 0) if p is NULL or HV_PVG_TALLY was never set (p->tally off): this does not
   turn tallying on, it only reads back what has already been counted. */
void hv_pvg_lite_tally_snapshot(HvPvgLite *p, uint64_t *execs, uint64_t *bad, uint64_t counts_out[6][256]);

/* HV_PVG_TALLY diagnostic (read-only; same gating as hv_pvg_lite_tally_snapshot above): decodes the most
   recent display_swap()'s surface descriptor and walks its guest page list the way pvg_metal.m's
   makeMappedTexture() does, logging one bounded line (non-black pixel count and a hash over the whole
   surface, never raw pixel bytes) under the label `when`. For a caller that wants to see whether a
   surface's actual guest-memory content changed between the swap that named it and some later point (e.g.
   right before a run ends). A no-op if p is NULL, HV_PVG_TALLY was never set, or there has been no swap
   yet. */
void hv_pvg_lite_tally_surface(HvPvgLite *p, const char *when);

/* HV_PVG_TALLY diagnostic (read-only; same gating/validity rules as hv_pvg_lite_tally_surface above): a
   malloc'd BGRA copy (caller frees it) of the last swapped surface's actual pixels, for a caller that wants
   the real bytes -- e.g. to write them out as a BMP -- rather than tally_surface()'s one-line summary.
   Returns 0 and fills out, width and height on success; -1 (nothing allocated, out left NULL) if there has
   been no swap yet, the descriptor and page list do not check out, or the surface's fourcc is not 'BGRA'
   (0x42475241) -- the only format this has verified the byte order of; see pvg_lite.c's own comment on it. */
int hv_pvg_lite_last_surface_bgra(HvPvgLite *p, uint8_t **out, uint32_t *width, uint32_t *height);

/* General-purpose counterpart to hv_pvg_lite_last_surface_bgra() above, NOT gated by HV_PVG_TALLY: for a
   caller (host/vm.c's hv_vm_boot_display(), called roughly every frame of the live VM display) that wants
   this every tick without a malloc/free per call. Copies into the caller-owned buf (buf_cap bytes) instead
   of allocating one; takes the machine's device lock for the whole validate+copy, the same lock the guest's
   own swap/unmap already run under, so a racing guest-side remap cannot be read half-old/half-new -- see
   pvg_lite.c's own comment for the full concurrency and ownership contract. Returns 0 and fills width/height
   on success; -1 (buf untouched) if there has been no swap yet, the descriptor/page list do not check out,
   the fourcc is not 'BGRA', or buf_cap is too small for the current surface -- in that last case only,
   *need_out (if non-NULL) is set to the bytes needed (width*height*4) so the caller can grow once and retry;
   0 in every other failure case. */
int hv_pvg_lite_live_surface_bgra(HvPvgLite *p, uint8_t *buf, size_t buf_cap, uint32_t *width, uint32_t *height, size_t *need_out);

#endif
