#ifndef HENGEVM_DISPLAY_BUF_H
#define HENGEVM_DISPLAY_BUF_H
/* B0: the host's copy of the guest's picture, and the rules for getting it.

   What the engine hands out (hv_vm_boot_display, host/vm.c) is a pointer INTO GUEST RAM (the boot framebuffer) or into the graphics device's own frame. The guest keeps
   writing to it, nothing in vm.c locks it, and it stops being valid when the machine's memory goes away. So a host display layer must never hold that pointer. The one
   rule this module enforces is structural: the pointer exists only inside hv_dispbuf_pull, under the buffer's lock, between the fetch callback returning it and the copy
   into the buffer's own memory finishing. Everything after that (converting, scaling, showing) works on the copy.

   THE CONTRACT (C1 must keep it when it connects the real machine; C0+B0 only runs it on synthetic frames):
     1. The producer thread calls hv_dispbuf_pull with a fetch callback that calls hv_vm_boot_display afresh every time. It never keeps a frame pointer between calls.
     2. The copy may be torn (the guest can write while it is read). That is accepted for a boot picture; a pointer that is stale or freed is not.
     3. BEFORE the machine's memory is freed or the machine is created again, whatever the reason (the user's stop, the guest powering off, an execution error, the app
        going away), the host calls hv_dispbuf_close. It takes the same lock, so it returns only after a copy in progress has finished, and every later pull returns
        HV_DISP_CLOSED without calling fetch. A "running" flag checked before the call is NOT enough: the memory could be freed between the check and the copy.
        The order is: close the buffer, then stop/free the machine. Never the other way round.
     4. After a close, the buffer is dead. A new machine gets a new buffer (hv_dispbuf_destroy, hv_dispbuf_create): the cached boot-picture position inside the engine
        (vm.c) is per process, and whether it survives a second machine is NOT established here; that is C1's to decide.
     5. The fetch callback must be quick and must not call back into this module (the lock is held).
   The consumer (the screen) only reads the buffer's copy through hv_dispbuf_blit, which also takes the lock; it never sees a guest pointer. */
#include <stddef.h>
#include <stdint.h>

typedef struct HvDispBuf HvDispBuf;

/* Larger frames are refused (a phone has no use for them and the copy would be huge): 4096 x 4096 at most, both sides at least 1. */
#define HV_DISP_MAX_DIM 4096u

enum { HV_DISP_NEW = 1, HV_DISP_NONE = 0, HV_DISP_CLOSED = -1, HV_DISP_REJECTED = -2 };

/* What the guest side offers: pixels (BGRA, 4 bytes each), size and row length in bytes. Returns 0 with the three values set, or -1 when there is no frame yet.
   Same shape as hv_vm_boot_display (HvBootDisplay) so C1 can wrap it in a few lines. */
typedef int (*HvFrameFetch)(void *ctx, const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *stride_bytes);

HvDispBuf *hv_dispbuf_create(void);
/* Frees it. The caller guarantees no thread is inside any call on it (close first, join the threads, then destroy). */
void hv_dispbuf_destroy(HvDispBuf *b);

/* Producer. HV_DISP_NEW: a frame was copied in. HV_DISP_NONE: fetch had no frame. HV_DISP_CLOSED: the buffer is closed, fetch was not called.
   HV_DISP_REJECTED: fetch gave a size or row length that cannot be a frame (zero, over the limit, row shorter than width * 4, a size that overflows); nothing was copied
   and the previous frame stays. */
int hv_dispbuf_pull(HvDispBuf *b, HvFrameFetch fetch, void *ctx);
/* The same for a caller that already owns its pixels (a synthetic frame): copy in, same checks. */
int hv_dispbuf_put(HvDispBuf *b, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride_bytes);

/* Consumer. The size of the frame held, or -1 before the first frame (and after close: a closed buffer keeps its last picture but reports closed to pullers only). */
int hv_dispbuf_size(HvDispBuf *b, uint32_t *width, uint32_t *height);
/* If a frame newer than *seen exists: converts it to RGBX (what an Android window takes: bytes R G B 0xFF) into dst, whose rows are dst_stride_px pixels apart and which is
   dst_w x dst_h; copies min(width, dst_w) x min(height, dst_h); sets *seen to the frame's number and returns 1. Returns 0 if nothing is newer, -1 if there is no frame. */
int hv_dispbuf_blit(HvDispBuf *b, uint64_t *seen, uint8_t *dst, uint32_t dst_stride_px, uint32_t dst_w, uint32_t dst_h);

/* After this no pull reaches fetch. Returns when a copy in progress has finished. Idempotent. */
void hv_dispbuf_close(HvDispBuf *b);
int hv_dispbuf_is_closed(HvDispBuf *b);

typedef struct { uint64_t frames, none, rejected, closed_refused, blits; } HvDispStats;
void hv_dispbuf_stats(HvDispBuf *b, HvDispStats *out);

/* The conversion alone (pure): BGRA source rows sstride bytes apart -> RGBX destination rows dstride_px pixels apart; min(sw, dw) x min(sh, dh) pixels. Returns the
   number of pixels written, 0 for a bad argument (NULL, zero size, sstride < sw * 4). */
size_t hv_disp_bgra_to_rgbx(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t sstride, uint8_t *dst, uint32_t dstride_px, uint32_t dw, uint32_t dh);

/* A synthetic BGRA frame for the screen test: eight colour bars over a gradient, a white square moving with `frame`, and the frame number as 16 black/white 8 x 8 cells
   in the top row (bit 0 first, white = 1) so a test can read it back after conversion. A white cross at (cx, cy) when cx >= 0 (the simulated pointer). stride_bytes >= width * 4;
   width >= 128 and height >= 64. */
void hv_testpattern(uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t frame, int cx, int cy);
/* Reads the frame number back from an RGBX picture made by hv_testpattern + hv_disp_bgra_to_rgbx; -1 if the cells are not black or white. */
int hv_testpattern_frame_rgbx(const uint8_t *rgbx, uint32_t stride_px, uint32_t width, uint32_t height);

#endif
