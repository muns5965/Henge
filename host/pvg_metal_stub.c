/* NOT a GPU. Stand-in for devices/pvg_metal.m on hosts without Metal, so the rest of the graphics device
   (devices/pvg_lite.c) can be compiled and linked and the machine brought up without drawing anything.
   hv_pvg_metal_create() returns NULL, which pvg_lite.c already handles (it is what HV_PVG_NO_METAL does on
   Apple): the guest's graphics commands are completed but nothing is rendered. A real Android backend
   replaces this file. */
#include "pvg_lite.h"

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) {
    (void)lite;
    return NULL;
}
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t channel, uint32_t *command, uint32_t length) {
    (void)m; (void)lite; (void)channel; (void)command; (void)length;
}
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t width, uint32_t height) {
    (void)m; (void)bgra; (void)width; (void)height;
    return -1;
}
int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **pixels, uint32_t *width, uint32_t *height) {
    (void)m; (void)pixels; (void)width; (void)height;
    return -1;
}
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping) { (void)m; (void)mapping; }
void hv_pvg_metal_cursor_moved(HvPvgMetal *m) { (void)m; }
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer) { (void)m; (void)layer; }
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) {
    (void)m;
    return 0;
}
