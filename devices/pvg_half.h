/* Half-float display surface ('RGhA', RGBA16Float, 8 bytes per pixel) to display bytes. Platform independent: no Metal, no Apple API.
   Shared by devices/pvg_metal.m (Apple) and devices/pvg_lite.c (the Android and plain-C display path), so the one colour policy of the original
   Henge applies to both.

   Policy (unchanged from the original, see the comment on hv_half_mode_get): WindowServer writes DISPLAY-ENCODED values (extended sRGB) into the
   surface, so mode 0 passes them through: clamp to [0, 1], times 255, rounded. A negative value, zero and NaN are 0; one or more (and +infinity)
   is 255. Mode 1 (HV_PVG_HALF=linear) treats the values as LINEAR light and applies the sRGB encode. No other gamma, no guessing of channel order:
   the surface is R, G, B, A in memory (16 bits each, little endian) and the output is B, G, R, 255. */
#ifndef HV_PVG_HALF_H
#define HV_PVG_HALF_H

#include <math.h> /* powf: only used where HV_HALF_HAVE_LINEAR is defined (below) */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HV_FOURCC_BGRA 0x42475241u /* 'BGRA' */
#define HV_FOURCC_RGHA 0x52476841u /* 'RGhA' */

/* How the half-float display surface is shown. WindowServer writes DISPLAY-ENCODED values there (extended sRGB: the guest's gray 128 is
   0.502, measured with the colour probe), so mode 0 passes them through. Mode 1 treats them as LINEAR light and applies the sRGB encode,
   which is what this code did before (and was only right for a linear display profile such as ACES CG Linear: with every other profile
   it encodes twice and the screen looks pale). HV_PVG_HALF=linear selects mode 1 at start; with the colour probe on, a file named
   mode-linear or mode-encoded in its folder switches it while running. */
static _Atomic int hv_half_mode = -1;
static inline int hv_half_mode_get(void) {
    int m = atomic_load(&hv_half_mode);
    if (m < 0) {
        const char *e = getenv("HV_PVG_HALF");
        m = e && !strcmp(e, "linear") ? 1 : 0;
        atomic_store(&hv_half_mode, m);
    }
    return m;
}
/* Mode 1 needs powf (libm). Apple always has it; the plain-C (Android) link lines of this project do not all link libm, and the runner removes HV_* variables so mode 1 cannot be
   selected there. So outside Apple, mode 1 is only built when the build says it links libm (-DHV_HALF_LIBM); otherwise it shows the values as mode 0 does. */
#if defined(__APPLE__) || defined(HV_HALF_LIBM)
#define HV_HALF_HAVE_LINEAR 1
#endif
static inline float hv_half_shown(float v, int mode) { /* what the screen shows for a half-float component */
    v = v < 0 ? 0 : v > 1 ? 1 : v;
#ifdef HV_HALF_HAVE_LINEAR
    return mode == 1 ? (v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f) : v;
#else
    (void)mode;
    return v;
#endif
}

/* IEEE 754 binary16 to float, exactly (every half value is representable), without a compiler half type (not every host has one). */
static inline float hv_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ffu, bits;
    if (e == 0) {
        if (m == 0) bits = sign;
        else { /* subnormal: normalise */
            e = 1;
            while (!(m & 0x400u)) m <<= 1, e--;
            m &= 0x3ffu;
            bits = sign | ((e + 112u) << 23) | (m << 13);
        }
    } else if (e == 31) bits = sign | 0x7f800000u | (m << 13); /* infinity or NaN */
    else bits = sign | ((e + 112u) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* Every possible half-float value to a display byte, as the screen would show it in the current mode. Built once per mode. */
static inline const uint8_t *hv_half_to_byte_table(void) {
    static _Atomic(uint8_t *) tables[2];
    int mode = hv_half_mode_get();
    uint8_t *t = atomic_load(&tables[mode]);
    if (t) return t;
    t = malloc(65536);
    if (!t) return NULL;
    for (uint32_t i = 0; i < 65536; i++) {
        float v = hv_half_to_float((uint16_t)i);
        t[i] = !(v > 0) ? 0 : v >= 1 ? 255 : (uint8_t)(hv_half_shown(v, mode) * 255.0f + 0.5f);
    }
    uint8_t *none = NULL;
    if (!atomic_compare_exchange_strong(&tables[mode], &none, t)) { free(t); t = none; } /* another thread was first: use its table */
    return t;
}

/* One row of `w` RGhA pixels (w * 4 uint16: R, G, B, A) to `w` BGRA pixels (B, G, R, 255), exactly as the original's software path does. */
static inline void hv_half_row_to_bgra(const uint16_t *in, uint8_t *out, uint64_t w, const uint8_t *lut) {
    for (uint64_t x = 0; x < w; x++, in += 4, out += 4) out[0] = lut[in[2]], out[1] = lut[in[1]], out[2] = lut[in[0]], out[3] = 255;
}

#endif
