/* C0 + B0 screen test (no virtual machine): draws host/display_buf.c's synthetic picture into a SurfaceView through the contract of host/display_buf.h, and turns touch and key
   events into the guest-mouse and guest-key calls of host/touch_input.c and host/key_map.c. The "guest" here is a small simulation in this file: a pointer that moves by the
   relative moves it is sent (with an acceleration like the real one's) and is drawn as a white cross in the picture; keys and buttons are only counted. The real machine
   (hv_vm_*) is NOT linked and is not touched.

   Threads: the render thread calls nativeRender and nativeTick; the UI thread calls nativeTouch, nativeKey, nativeSetView and the surface calls. One mutex (g_mu) guards the touch
   state and the simulated guest; one (g_win_mu) guards the window pointer, so a surface that goes away waits for a picture being drawn.

   LIFETIME of the buffer and the source picture (g_buf, g_src): host/render_gate.h. nativeRender uses them only between hv_gate_enter and hv_gate_leave; nativeDestroy frees them only
   when hv_gate_try_destroy gave it the right (nobody inside, new entries refused, one owner), and reports hv_gate_released when they are gone; nativeStart builds a new instance only
   when the gate is idle. The window pointer (g_win_mu) is a separate matter and is not covered by the gate. Lock order: g_mu is never held while taking g_win_mu or the gate's mutex. */
#include <jni.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_buf.h"
#include "key_map.h"
#include "render_gate.h"
#include "touch_input.h"

#define TAG "henge-ui"
#define FRAME_W 1280
#define FRAME_H 720

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_win_mu = PTHREAD_MUTEX_INITIALIZER;
static HvGate g_gate;
static pthread_once_t g_gate_once = PTHREAD_ONCE_INIT;
static void gate_init_once(void) { hv_gate_init(&g_gate); }
static HvDispBuf *g_buf;
static ANativeWindow *g_win;
static int g_win_w, g_win_h;
static uint8_t *g_src;           /* the synthetic "guest picture" the fetch callback hands out, BGRA */
static uint32_t g_frame;
static HvTouch g_touch;
static int g_feedback = 1;

static struct { int x, y; long moves, buttons_down, buttons_up, wheel, hwheel, keys, unmapped_keys; unsigned last_key, last_button; int held; } g_guest;

static int accel(int d) { double a = 1.0 + (abs(d) / 40.0 > 1.5 ? 1.5 : abs(d) / 40.0); return (int)(d * a + (d < 0 ? -0.5 : 0.5)); }
static void sink_move(int dx, int dy) {
    g_guest.moves++;
    g_guest.x += accel(dx); g_guest.y += accel(dy);
    g_guest.x = g_guest.x < 0 ? 0 : g_guest.x > FRAME_W - 1 ? FRAME_W - 1 : g_guest.x;
    g_guest.y = g_guest.y < 0 ? 0 : g_guest.y > FRAME_H - 1 ? FRAME_H - 1 : g_guest.y;
}
static void sink_button(int b, int down) { g_guest.last_button = (unsigned)b; if (down) g_guest.buttons_down++; else g_guest.buttons_up++; if (b == 0) g_guest.held = down; }
static void sink_wheel(int n) { g_guest.wheel += n; }
static void sink_hwheel(int n) { g_guest.hwheel += n; }
static void sink_key(uint16_t code, int down) { (void)down; g_guest.keys++; g_guest.last_key = code; }

static int fetch_synthetic(void *ctx, const uint8_t **px, uint32_t *w, uint32_t *h, uint32_t *stride) {
    *px = ctx; *w = FRAME_W; *h = FRAME_H; *stride = FRAME_W * 4;
    return 0;
}

/* 0: built and open. 1: already running. -1: building failed. -2: the previous instance is still closing or being freed (not yet released): nothing was built. */
JNIEXPORT jint JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeStart(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    pthread_once(&g_gate_once, gate_init_once);
    int st = hv_gate_begin_start(&g_gate);
    if (st == 0) return 1;
    if (st < 0) return -2;
    HvDispBuf *b = hv_dispbuf_create();
    uint8_t *src = malloc((size_t)FRAME_W * FRAME_H * 4);
    if (!b || !src) {
        if (b) hv_dispbuf_destroy(b);
        free(src);
        hv_gate_start_failed(&g_gate);
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    g_buf = b; g_src = src;
    memset(&g_guest, 0, sizeof g_guest);
    g_guest.x = FRAME_W / 2; g_guest.y = FRAME_H / 2;
    g_frame = 0;
    HvInputSink sk = {sink_key, sink_move, sink_button, sink_wheel, sink_hwheel};
    hv_touch_init(&g_touch, &sk);
    hv_touch_set_display(&g_touch, FRAME_W, FRAME_H);
    pthread_mutex_unlock(&g_mu);
    hv_gate_open(&g_gate);   /* only now may nativeRender enter */
    return 0;
}

/* First step of the shutdown (host/display_buf.h): the buffer stops taking frames and no new render may enter. It does not free anything and is safe to call twice. */
JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeClose(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    pthread_once(&g_gate_once, gate_init_once);
    if (hv_gate_enter(&g_gate)) {          /* the buffer exists while we are inside */
        hv_dispbuf_close(g_buf);           /* waits for a copy in progress */
        hv_gate_leave(&g_gate);
    }
    hv_gate_begin_close(&g_gate);
}

/* Last step: frees the buffer and the source picture, only if this call got the right to (host/render_gate.h). 1: freed. 0: a render is still inside, nothing was freed, try again
   (the render thread does, when it ends). -1: not this call's to free (another one has it, or nothing is built). Safe to call from the UI thread and the render thread. */
JNIEXPORT jint JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeDestroy(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    pthread_once(&g_gate_once, gate_init_once);
    int r = hv_gate_try_destroy(&g_gate);
    if (r != 1) return r;
    pthread_mutex_lock(&g_mu);
    hv_touch_release_all(&g_touch);
    HvDispBuf *b = g_buf; g_buf = NULL;
    uint8_t *src = g_src; g_src = NULL;
    pthread_mutex_unlock(&g_mu);
    if (b) hv_dispbuf_destroy(b);
    free(src);
    hv_gate_released(&g_gate);   /* only now can a new instance start */
    return 1;
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeSetSurface(JNIEnv *env, jclass cls, jobject surface) {
    (void)cls;
    pthread_mutex_lock(&g_win_mu);
    if (g_win) { ANativeWindow_release(g_win); g_win = NULL; }
    if (surface) g_win = ANativeWindow_fromSurface(env, surface);
    g_win_w = g_win_h = 0;
    pthread_mutex_unlock(&g_win_mu);
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeSetView(JNIEnv *env, jclass cls, jfloat w, jfloat h) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    hv_touch_set_view(&g_touch, 0, 0, w, h);
    pthread_mutex_unlock(&g_mu);
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeSetFeedback(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    g_feedback = on ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
}

/* One picture: the synthetic guest frame is made, pulled through the contract, and the newest copy is shown. Returns the frame number shown, 0 = nothing new, -1 = closed or not running.
   The whole body, up to the single exit, is between hv_gate_enter and hv_gate_leave: that is what keeps g_buf and g_src alive. */
JNIEXPORT jint JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeRender(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    pthread_once(&g_gate_once, gate_init_once);
    if (!hv_gate_enter(&g_gate)) return -1;
    int result = 0;
    HvDispBuf *b = g_buf;
    uint8_t *src = g_src;      /* stable while we are inside */
    pthread_mutex_lock(&g_mu);
    int cx = g_guest.x, cy = g_guest.y;
    uint32_t frame = ++g_frame;
    hv_testpattern(src, FRAME_W, FRAME_H, FRAME_W * 4, frame, cx, cy);
    pthread_mutex_unlock(&g_mu);

    int r = hv_dispbuf_pull(b, fetch_synthetic, src);
    if (r == HV_DISP_CLOSED) { result = -1; goto out; }
    if (r != HV_DISP_NEW) goto out;

    pthread_mutex_lock(&g_win_mu);
    if (g_win) {
        uint32_t w, h;
        if (hv_dispbuf_size(b, &w, &h) == 0) {
            if (g_win_w != (int)w || g_win_h != (int)h) {
                ANativeWindow_setBuffersGeometry(g_win, (int32_t)w, (int32_t)h, WINDOW_FORMAT_RGBX_8888);
                g_win_w = (int)w; g_win_h = (int)h;
            }
            ANativeWindow_Buffer wb;
            if (ANativeWindow_lock(g_win, &wb, NULL) == 0) {
                static uint64_t seen;
                if (wb.format == WINDOW_FORMAT_RGBX_8888 || wb.format == WINDOW_FORMAT_RGBA_8888) {
                    if (hv_dispbuf_blit(b, &seen, (uint8_t *)wb.bits, (uint32_t)wb.stride, (uint32_t)wb.width, (uint32_t)wb.height) == 1) result = (int)frame;
                } else {
                    __android_log_print(ANDROID_LOG_WARN, TAG, "window format %d is not RGBX/RGBA", wb.format);
                }
                ANativeWindow_unlockAndPost(g_win);
            }
        }
    }
    pthread_mutex_unlock(&g_win_mu);
out:
    hv_gate_leave(&g_gate);
    return result;
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeTouch(JNIEnv *env, jclass cls, jint action, jint id, jfloat x, jfloat y, jlong t_ms) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    hv_touch_event(&g_touch, action, id, x, y, t_ms);
    pthread_mutex_unlock(&g_mu);
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeTick(JNIEnv *env, jclass cls, jlong t_ms) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    hv_touch_pointer(&g_touch, g_feedback, g_guest.x, g_guest.y);
    hv_touch_tick(&g_touch, t_ms);
    pthread_mutex_unlock(&g_mu);
}

/* 1 when the key was mapped and sent, 0 when it is not a key the guest gets (the caller then handles it itself, e.g. Back). */
JNIEXPORT jint JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeKey(JNIEnv *env, jclass cls, jint keycode, jboolean down) {
    (void)env; (void)cls;
    uint16_t l = hv_keymap_android(keycode);
    pthread_mutex_lock(&g_mu);
    if (l) sink_key(l, down ? 1 : 0); else g_guest.unmapped_keys++;
    pthread_mutex_unlock(&g_mu);
    return l ? 1 : 0;
}

/* 1 when the guest gets this key (no event is sent): the caller uses it to swallow Android's key repeats, which the guest makes itself. */
JNIEXPORT jint JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeKeyMapped(JNIEnv *env, jclass cls, jint keycode) {
    (void)env; (void)cls;
    return hv_keymap_android(keycode) ? 1 : 0;
}

JNIEXPORT jstring JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeStatus(JNIEnv *env, jclass cls) {
    (void)cls;
    char s[512];
    HvDispStats st = {0};
    pthread_mutex_lock(&g_mu);
    if (g_buf) hv_dispbuf_stats(g_buf, &st);
    snprintf(s, sizeof s,
             "frames %llu (copied), shown %llu, rejected %llu, after close %llu\nguest pointer %d,%d  moves %ld  feedback %s\nbuttons down %ld up %ld (last %u, left %s)  wheel %ld  hwheel %ld\nkeys %ld (last Linux code %u), unmapped %ld",
             (unsigned long long)st.frames, (unsigned long long)st.blits, (unsigned long long)st.rejected, (unsigned long long)st.closed_refused,
             g_guest.x, g_guest.y, g_guest.moves, g_feedback ? "on" : "off (dead reckoning)",
             g_guest.buttons_down, g_guest.buttons_up, g_guest.last_button, g_guest.held ? "held" : "up", g_guest.wheel, g_guest.hwheel,
             g_guest.keys, g_guest.last_key, g_guest.unmapped_keys);
    pthread_mutex_unlock(&g_mu);
    return (*env)->NewStringUTF(env, s);
}
