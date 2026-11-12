package dev.muns.hengeprobe;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/** C0 + B0 screen test. NO virtual machine: a synthetic picture drawn through host/display_buf.h, touch and keys turned into guest-mouse and guest-key calls
 *  (host/touch_input.c, host/key_map.c) that only drive a simulated pointer and counters (ui_jni.c). The shutdown order is the contract of host/display_buf.h: close the
 *  buffer, join the render thread, destroy. */
public class DisplayTestActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("henge_ui");
    }

    private static native int nativeStart();
    private static native void nativeClose();
    private static native void nativeDestroy();
    private static native void nativeSetSurface(android.view.Surface s);
    private static native void nativeSetView(float w, float h);
    private static native void nativeSetFeedback(boolean on);
    private static native int nativeRender();
    private static native void nativeTouch(int action, int id, float x, float y, long tMs);
    private static native void nativeTick(long tMs);
    private static native int nativeKey(int keycode, boolean down);
    private static native int nativeKeyMapped(int keycode);
    private static native String nativeStatus();

    // the same numbers as host/touch_input.h
    private static final int T_DOWN = 0, T_UP = 1, T_MOVE = 2, T_CANCEL = 3;

    private SurfaceView surface;
    private TextView status;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile Thread renderThread;
    private volatile boolean stopRender;
    private boolean surfaceReady;
    private boolean started;
    private boolean feedback = true;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setBackgroundColor(0xAA000000);
        status.setTypeface(Typeface.MONOSPACE);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        status.setPadding(12, 8, 12, 8);
        status.setText("C0+B0 screen test (no virtual machine). Touch: tap = click, hold = right click, two fingers = scroll. Keys: a hardware keyboard.\nTap here to switch the pointer feedback.");
        status.setOnClickListener(v -> { feedback = !feedback; nativeSetFeedback(feedback); });
        root.addView(status, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        setContentView(root);
        if (nativeStart() < 0) status.setText("nativeStart failed");
        else started = true;
    }

    @Override
    protected void onResume() {
        super.onResume();
        startRender();
    }

    @Override
    protected void onPause() {
        stopRender();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        // the order of host/display_buf.h: close the buffer, the render thread is already joined (onPause), destroy
        if (started) { nativeClose(); stopRender(); nativeDestroy(); started = false; }
        super.onDestroy();
    }

    private void startRender() {
        if (!started || renderThread != null || !surfaceReady) return;
        stopRender = false;
        Thread t = new Thread(() -> {
            long lastStatus = 0;
            while (!stopRender) {
                long now = SystemClock.uptimeMillis();
                nativeTick(now);
                if (nativeRender() < 0) break;
                if (now - lastStatus >= 250) {
                    lastStatus = now;
                    final String s = nativeStatus();
                    ui.post(() -> { if (status != null) status.setText(s); });
                }
                long wait = 16 - (SystemClock.uptimeMillis() - now);
                if (wait > 0) try { Thread.sleep(wait); } catch (InterruptedException e) { break; }
            }
        }, "henge-ui-render");
        renderThread = t;
        t.start();
    }

    private void stopRender() {
        Thread t = renderThread;
        stopRender = true;
        if (t != null) {
            t.interrupt();
            try { t.join(2000); } catch (InterruptedException ignored) { }
            renderThread = null;
        }
    }

    @Override public void surfaceCreated(SurfaceHolder h) { nativeSetSurface(h.getSurface()); surfaceReady = true; startRender(); }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { nativeSetView(w, hh); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { surfaceReady = false; stopRender(); nativeSetSurface(null); }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (!started) return super.dispatchTouchEvent(e);
        long t = e.getEventTime();
        int idx = e.getActionIndex();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                nativeTouch(T_DOWN, e.getPointerId(idx), e.getX(idx), e.getY(idx), t);
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) nativeTouch(T_MOVE, e.getPointerId(i), e.getX(i), e.getY(i), t);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                nativeTouch(T_UP, e.getPointerId(idx), e.getX(idx), e.getY(idx), t);
                break;
            case MotionEvent.ACTION_CANCEL:
                nativeTouch(T_CANCEL, 0, 0, 0, t);
                break;
            default:
                break;
        }
        return true;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (!started) return super.dispatchKeyEvent(e);
        int a = e.getAction();
        if (a != KeyEvent.ACTION_DOWN && a != KeyEvent.ACTION_UP) return super.dispatchKeyEvent(e);
        // the guest repeats a held key itself: Android's repeats are not sent
        if (a == KeyEvent.ACTION_DOWN && e.getRepeatCount() > 0) return nativeKeyMapped(e.getKeyCode()) == 1 || super.dispatchKeyEvent(e);
        if (nativeKey(e.getKeyCode(), a == KeyEvent.ACTION_DOWN) == 1) return true;
        return super.dispatchKeyEvent(e);   // Back and the other keys the guest does not get
    }
}
