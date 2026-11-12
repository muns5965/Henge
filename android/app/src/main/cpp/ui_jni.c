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
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_buf.h"
#include "frame_shm.h"
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

/* C1-1: the runner mode */
#define SHM_PX_CAP (1920u * 1200u * 4u)
static int g_want_runner;          /* nativeUseRunner: the picture, the input and the pointer feedback go through the shared file */
static HvShm *g_shm;               /* set by nativeAttachRunner once the runner has made its file; under g_mu */
static uint8_t *g_shm_px;          /* the bounce buffer one picture is copied into before display_buf copies it again */
static long g_shm_fail;            /* calls that could not take the file's lock */
static int g_shm_known, g_shm_px_x, g_shm_px_y;

/* Diagnostics for the test screen (under g_mu). They only watch; nothing here feeds back into the touch module. */
static struct {
    long down, move, up, cancel;           /* events received by nativeTouch (touches on the status text are not counted: that text takes them) */
    float view_w, view_h;                  /* what nativeSetView was told */
    int64_t up_ms; int settle_armed, settle_valid; int sdx, sdy;   /* error 0.5 s after the last finger lifted */
} g_dg;

static struct { int x, y; long moves, buttons_down, buttons_up, wheel, hwheel, keys, unmapped_keys; unsigned last_key, last_button; int held; } g_guest;

static int accel(int d) { double a = 1.0 + (abs(d) / 40.0 > 1.5 ? 1.5 : abs(d) / 40.0); return (int)(d * a + (d < 0 ? -0.5 : 0.5)); }
static void sink_move(int dx, int dy) {
    if (g_want_runner) { if (g_shm && hv_shm_push_cmd(g_shm, HV_SHM_CMD_MOVE, dx, dy) < 0) g_shm_fail++; return; }
    g_guest.moves++;
    g_guest.x += accel(dx); g_guest.y += accel(dy);
    g_guest.x = g_guest.x < 0 ? 0 : g_guest.x > FRAME_W - 1 ? FRAME_W - 1 : g_guest.x;
    g_guest.y = g_guest.y < 0 ? 0 : g_guest.y > FRAME_H - 1 ? FRAME_H - 1 : g_guest.y;
}
static void sink_button(int b, int down) { if (g_want_runner) { if (g_shm && hv_shm_push_cmd(g_shm, HV_SHM_CMD_BUTTON, b, down ? 1 : 0) < 0) g_shm_fail++; return; } g_guest.last_button = (unsigned)b; if (down) g_guest.buttons_down++; else g_guest.buttons_up++; if (b == 0) g_guest.held = down; }
static void sink_wheel(int n) { if (g_want_runner) { if (g_shm && hv_shm_push_cmd(g_shm, HV_SHM_CMD_WHEEL, n, 0) < 0) g_shm_fail++; return; } g_guest.wheel += n; }
static void sink_hwheel(int n) { if (g_want_runner) { if (g_shm && hv_shm_push_cmd(g_shm, HV_SHM_CMD_HWHEEL, n, 0) < 0) g_shm_fail++; return; } g_guest.hwheel += n; }
static void sink_key(uint16_t code, int down) { if (g_want_runner) { if (g_shm && hv_shm_push_cmd(g_shm, HV_SHM_CMD_KEY, code, down ? 1 : 0) < 0) g_shm_fail++; return; } (void)down; g_guest.keys++; g_guest.last_key = code; }

static int fetch_synthetic(void *ctx, const uint8_t **px, uint32_t *w, uint32_t *h, uint32_t *stride) {
    *px = ctx; *w = FRAME_W; *h = FRAME_H; *stride = FRAME_W * 4;
    return 0;
}

typedef struct { HvShm *shm; uint8_t *px; } ShmSrc;
static int fetch_shm(void *ctx, const uint8_t **px, uint32_t *w, uint32_t *h, uint32_t *stride) {
    ShmSrc *src = ctx;
    static uint64_t seen;                       /* the render thread only */
    if (!src->shm || !src->px) return -1;
    if (hv_shm_read_frame(src->shm, &seen, src->px, SHM_PX_CAP, w, h) != 1) return -1;   /* nothing newer, or none yet */
    *px = src->px; *stride = *w * 4;
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
    { float vw = g_dg.view_w, vh = g_dg.view_h; memset(&g_dg, 0, sizeof g_dg); g_dg.view_w = vw; g_dg.view_h = vh; }
    g_guest.x = FRAME_W / 2; g_guest.y = FRAME_H / 2;
    g_frame = 0;
    g_want_runner = 0; g_shm_fail = 0; g_shm_known = 0;
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
    HvShm *shm = g_shm; g_shm = NULL;           /* after this nothing under g_mu can reach it any more */
    uint8_t *spx = g_shm_px; g_shm_px = NULL;
    g_want_runner = 0;
    pthread_mutex_unlock(&g_mu);
    if (b) hv_dispbuf_destroy(b);
    free(src);
    hv_shm_close(shm);
    free(spx);
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
    g_dg.view_w = w; g_dg.view_h = h;
    pthread_mutex_unlock(&g_mu);
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeSetFeedback(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    g_feedback = on ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
}

/* C1-1: switch the screen to the runner's shared file. Call right after nativeStart, before the render thread starts. */
JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeUseRunner(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    g_want_runner = on ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
}

/* Opens the runner's file, which must name `pid` (the process the app started; pid <= 0 = any). 0 attached, 1 already attached, -1 no instance is running here (or it is being closed), -2 the file is not there yet or is not a good one (try again). */
JNIEXPORT jint JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeAttachRunner(JNIEnv *env, jclass cls, jstring jpath, jlong pid) {
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return -2;
    pthread_mutex_lock(&g_mu);
    int pre = g_shm ? 1 : !g_buf ? -1 : 0;
    pthread_mutex_unlock(&g_mu);
    if (pre) { (*env)->ReleaseStringUTFChars(env, jpath, path); return pre; }
    HvShm *s = NULL;
    int r = hv_shm_open(path, &s);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (r != 0) return -2;
    HvShmStats rs;
    if (hv_shm_stats(s, &rs) != 0 || (pid > 0 && rs.runner_pid != (uint32_t)pid)) { hv_shm_close(s); return -2; }   /* a file an earlier runner left behind is not the one we started */
    uint8_t *px = malloc(SHM_PX_CAP);
    pthread_mutex_lock(&g_mu);
    if (!px || !g_buf || g_shm) { int was = g_shm ? 1 : -1; pthread_mutex_unlock(&g_mu); free(px); hv_shm_close(s); return was; }
    g_shm = s; g_shm_px = px;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

/* Tells the runner the app is closing (it then stops by itself; the Activity also sends it "quit"). */
JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeRunnerAppClose(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    if (g_shm) hv_shm_app_close(g_shm);
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
    int want = g_want_runner;
    ShmSrc ss = {g_shm, g_shm_px};
    if (!want) hv_testpattern(src, FRAME_W, FRAME_H, FRAME_W * 4, frame, cx, cy);
    pthread_mutex_unlock(&g_mu);

    int r = want ? hv_dispbuf_pull(b, fetch_shm, &ss) : hv_dispbuf_pull(b, fetch_synthetic, src);
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
    switch (action) { case HV_TOUCH_DOWN: g_dg.down++; break; case HV_TOUCH_MOVE: g_dg.move++; break; case HV_TOUCH_UP: g_dg.up++; break; case HV_TOUCH_CANCEL: g_dg.cancel++; break; default: break; }
    HvTouchDiag d; hv_touch_diag(&g_touch, &d);
    if (action == HV_TOUCH_DOWN) { g_dg.settle_armed = 0; g_dg.settle_valid = 0; }
    if ((action == HV_TOUCH_UP || action == HV_TOUCH_CANCEL) && d.fingers == 0) { g_dg.up_ms = t_ms; g_dg.settle_armed = 1; g_dg.settle_valid = 0; }
    pthread_mutex_unlock(&g_mu);
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_DisplayTestActivity_nativeTick(JNIEnv *env, jclass cls, jlong t_ms) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_mu);
    if (g_want_runner) {                                  /* the feedback is the runner's guest pointer, from the shared file */
        int x = 0, y = 0, v = g_shm ? hv_shm_get_pointer(g_shm, &x, &y) : 0;
        if (v < 0) { g_shm_fail++; }                      /* the lock was not taken: keep what we knew */
        else { g_shm_known = v; if (v) { g_guest.x = x; g_guest.y = y; } }
        hv_touch_pointer(&g_touch, g_feedback && g_shm_known, g_guest.x, g_guest.y);
    } else hv_touch_pointer(&g_touch, g_feedback, g_guest.x, g_guest.y);
    hv_touch_tick(&g_touch, t_ms);
    HvTouchDiag d; hv_touch_diag(&g_touch, &d);
    if (g_dg.settle_armed && d.has_target && t_ms - g_dg.up_ms >= 500) {      /* the finger has been up for 0.5 s: what is left is the settled error */
        g_dg.sdx = (int)lround(d.target_x) - g_guest.x; g_dg.sdy = (int)lround(d.target_y) - g_guest.y;
        g_dg.settle_valid = 1; g_dg.settle_armed = 0;
    }
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
    char s[2400], touch[320], err[160], runner[420] = "";
    HvDispStats st = {0};
    pthread_mutex_lock(&g_mu);
    if (g_buf) hv_dispbuf_stats(g_buf, &st);
    HvTouchDiag d; hv_touch_diag(&g_touch, &d);          /* one copy, under g_mu: everything below is from the same moment */
    static const char *const an[] = {"DOWN", "UP", "MOVE", "CANCEL"};
    const char *last = d.last_action >= 0 && d.last_action < 4 ? an[d.last_action] : "-";
    if (d.events == 0) snprintf(touch, sizeof touch, "touch: none yet");
    else if (!d.has_target) snprintf(touch, sizeof touch, "last event %s %.0f,%.0f  target: none (cancelled or no finger yet)  conv N/A  [seq %d]", last, d.last_x, d.last_y, d.tgt_seq);
    else {
        /* what the target must be, worked out here from the point that set it and the view size (the same view size the module was given: conv=0 only says the two calculations agree, not that the screen mapping is right) */
        double fx = g_dg.view_w > 0 ? d.tgt_src_x / g_dg.view_w : 0, fy = g_dg.view_h > 0 ? d.tgt_src_y / g_dg.view_h : 0;
        fx = fx < 0 ? 0 : fx > 1 ? 1 : fx; fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
        int ex = (int)lround(fx * (FRAME_W - 1)), ey = (int)lround(fy * (FRAME_H - 1));
        int gx = (int)lround(d.target_x), gy = (int)lround(d.target_y);
        char conv[96];
        if (d.fingers >= 2) snprintf(conv, sizeof conv, "conv N/A (two fingers: target is from the earlier single finger)");
        else snprintf(conv, sizeof conv, "conv %+d,%+d", gx - ex, gy - ey);
        snprintf(touch, sizeof touch, "last event %s %.0f,%.0f  target set by %.0f,%.0f [seq %d]  expected %d,%d  target %d,%d  %s", last, d.last_x, d.last_y, d.tgt_src_x, d.tgt_src_y, d.tgt_seq, ex, ey, gx, gy, conv);
    }
    if (d.has_target) {
        int gx = (int)lround(d.target_x), gy = (int)lround(d.target_y);
        char settled[64];
        if (g_dg.settle_valid) snprintf(settled, sizeof settled, "%+d,%+d", g_dg.sdx, g_dg.sdy);
        else snprintf(settled, sizeof settled, d.fingers ? "(finger down)" : g_dg.settle_armed ? "(waiting 0.5 s)" : "-");
        snprintf(err, sizeof err, "pointer %d,%d  error now %+d,%+d  settled %s", g_guest.x, g_guest.y, gx - g_guest.x, gy - g_guest.y, settled);
    } else snprintf(err, sizeof err, "pointer %d,%d  error: no target", g_guest.x, g_guest.y);
    if (g_want_runner) {
        HvShmStats rs; int ok = g_shm && hv_shm_stats(g_shm, &rs) == 0;
        static const char *const sn[] = {"none", "starting", "running", "stopping", "stopped"};
        if (!g_shm) snprintf(runner, sizeof runner, "runner: not attached yet\n");
        else if (!ok) snprintf(runner, sizeof runner, "runner: file lock not taken (%ld failures)\n", g_shm_fail);
        else snprintf(runner, sizeof runner, "runner pid %u %s heartbeat %llu  published %llu refused %llu  cmds pushed %llu applied %llu dropped %llu  lock recovered %u failed %ld  pointer %s\n",
                      rs.runner_pid, rs.state >= 0 && rs.state < 5 ? sn[rs.state] : "?", (unsigned long long)rs.heartbeat, (unsigned long long)rs.frames, (unsigned long long)rs.rejected,
                      (unsigned long long)rs.cmds_pushed, (unsigned long long)rs.cmds_applied, (unsigned long long)rs.cmds_dropped, rs.recovered, g_shm_fail, g_shm_known ? "known" : "not yet");
    }
    snprintf(s, sizeof s,
             "frames %llu (copied), shown %llu, rejected %llu, after close %llu\n"
             "events DOWN %ld MOVE %ld UP %ld CANCEL %ld (last %s)  fingers %d  view %.0fx%.0f  guest %dx%d\n"
             "%s\n%s\n%s"
             "moves %ld  feedback %s  buttons down %ld up %ld (last %u, left %s)  wheel %ld hwheel %ld  keys %ld (last %u) unmapped %ld\n"
             "(touches on this text are not counted; tap it to switch the feedback. conv=0 only says two calculations from the same view size agree; it does not prove the screen mapping.)",
             (unsigned long long)st.frames, (unsigned long long)st.blits, (unsigned long long)st.rejected, (unsigned long long)st.closed_refused,
             g_dg.down, g_dg.move, g_dg.up, g_dg.cancel, last, d.fingers, g_dg.view_w, g_dg.view_h, FRAME_W, FRAME_H,
             touch, err, runner,
             g_guest.moves, g_feedback ? "on" : "off (dead reckoning)", g_guest.buttons_down, g_guest.buttons_up, g_guest.last_button, g_guest.held ? "held" : "up",
             g_guest.wheel, g_guest.hwheel, g_guest.keys, g_guest.last_key, g_guest.unmapped_keys);
    pthread_mutex_unlock(&g_mu);
    return (*env)->NewStringUTF(env, s);
}
