package dev.muns.hengeprobe;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.concurrent.TimeUnit;
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
 *  (host/touch_input.c, host/key_map.c) that only drive a simulated pointer and counters (ui_jni.c).
 *
 *  Shutdown (host/display_buf.h, host/render_gate.h): nativeClose (the buffer takes no more frames, no new render may enter), the render thread is stopped AND CONFIRMED ended
 *  (isAlive), then nativeDestroy. If the thread did not end, nothing is freed: the Activity is marked dead, the thread ends later by itself (nativeRender then returns -1) and does
 *  the nativeDestroy at its own end. nativeDestroy gives the right to free to one caller only, so both paths may call it.
 *
 *  C1-1 (intent extra "runner" = true): the picture, the input and the pointer feedback go through a SEPARATE PROCESS, the runner (libuirunner.so, host/ui_runner.c, synthetic mode: no virtual
 *  machine), over the shared file of host/frame_shm.h. This Activity starts it with the file's path, attaches to the file once the runner has made it (checking that the file names the
 *  process it started, so a file left by an earlier runner is never taken for it), and stops it like this: tell it the app is closing, send "quit", wait up to 5 s for its exit, and only
 *  then kill it. A runner that had to be killed is logged as FORCED, and shown; a clean one as "stopped, confirmed". The runner also stops by itself when its input closes, i.e. if this
 *  app dies.
 *
 *  Touch comes from the SurfaceView (its own coordinates, the ones ui_jni.c is told the size of), not from the Activity, so the status text on top stays clickable. */
public class DisplayTestActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "henge-ui";

    static {
        System.loadLibrary("henge_ui");
    }

    private static native int nativeStart();
    private static native void nativeClose();
    private static native int nativeDestroy();
    private static native void nativeSetSurface(android.view.Surface s);
    private static native void nativeSetView(float w, float h);
    private static native void nativeSetFeedback(boolean on);
    private static native int nativeRender();
    private static native void nativeTouch(int action, int id, float x, float y, long tMs);
    private static native void nativeTick(long tMs);
    private static native int nativeKey(int keycode, boolean down);
    private static native int nativeKeyMapped(int keycode);
    private static native String nativeStatus();
    private static native void nativeUseRunner(boolean on);
    private static native int nativeAttachRunner(String path, long pid);
    private static native void nativeRunnerAppClose();

    // the same numbers as host/touch_input.h
    private static final int T_DOWN = 0, T_UP = 1, T_MOVE = 2, T_CANCEL = 3;

    private SurfaceView surface;
    private TextView status;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile Thread renderThread;
    private volatile boolean stopRender;
    private volatile boolean dead;          // onDestroy ran: nothing may touch the views any more; the render thread cleans up when it ends
    private boolean surfaceReady;
    private boolean started;
    private boolean feedback = true;

    // C1-1: the runner process
    private boolean runnerMode;
    private volatile Process runner;
    private volatile long runnerPid;                 // the pid the runner printed on its first line
    private volatile String runnerLine = "runner: not started";     // the last status/result line it printed, or why it ended
    private final java.util.ArrayDeque<String> runnerLog = new java.util.ArrayDeque<>();

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        surface.setOnTouchListener((v, e) -> { handleTouch(e); return true; });   // view coordinates; the status text above is not covered by this
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
        int r = nativeStart();
        if (r < 0) status.setText("nativeStart failed (" + r + (r == -2 ? ": the previous screen is still being closed, try again in a moment" : "") + ")");
        else {
            started = true;
            runnerMode = getIntent().getBooleanExtra("runner", false);
            if (runnerMode) { nativeUseRunner(true); startRunnerProcess(); }
        }
    }

    /** Starts libuirunner.so with the shared file's path, reads its output, and attaches to its file. Nothing here touches the machine: there is none. */
    private void startRunnerProcess() {
        final File exe = new File(getApplicationInfo().nativeLibraryDir, "libuirunner.so");
        final File dir = new File(getFilesDir(), "c1");
        final File shm = new File(dir, "ui.shm");
        if (!exe.exists()) { runnerLine = "runner: libuirunner.so is not in this APK (its build failed); nothing was started"; return; }
        dir.mkdirs();
        shm.delete();                                    // a file left by an earlier run must not be mistaken for this one's
        try {
            final Process p = new ProcessBuilder(exe.getAbsolutePath(), "--shm", shm.getAbsolutePath()).directory(getFilesDir()).redirectErrorStream(true).start();
            runner = p;
            runnerPid = 0;
            new Thread(() -> {                           // output: keep the last lines; the last "ui_runner ..." line is what the screen shows
                try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                    String ln;
                    while ((ln = br.readLine()) != null) {
                        synchronized (runnerLog) { runnerLog.addLast(ln); while (runnerLog.size() > 40) runnerLog.removeFirst(); }
                        if (ln.startsWith("ui_runner")) runnerLine = ln;
                        if (ln.startsWith("ui_runner: started pid ")) {      // the pid the runner names itself: android.jar's Process has no pid()
                            try { runnerPid = Long.parseLong(ln.substring(23).split("[ ,]")[0]); } catch (NumberFormatException ignored) { }
                        }
                        Log.i(TAG, ln);
                    }
                } catch (java.io.IOException ignored) { }
                try { int code = p.waitFor(); runnerLine = "runner exited, code " + code + (code == 0 ? " (stopped, confirmed)" : code == 3 ? " (stop NOT confirmed by the runner)" : " (not a clean stop)") + "; last: " + runnerLine; Log.i(TAG, "runner exit code " + code); }
                catch (InterruptedException ignored) { }
            }, "henge-runner-out").start();
            new Thread(() -> {                           // attach: the file appears when the runner has started; it must name this process
                String path = shm.getAbsolutePath();
                long t0 = SystemClock.uptimeMillis();
                while (!dead && SystemClock.uptimeMillis() - t0 < 8000) {
                    long pid = runnerPid;                // 0 until the runner has printed its own pid
                    if (pid > 0) {
                        int rc = nativeAttachRunner(path, pid);
                        if (rc >= 0) { Log.i(TAG, "attached to the runner, pid " + pid); return; }
                        if (rc == -1) return;            // no instance here any more
                    }
                    try { Thread.sleep(50); } catch (InterruptedException e) { return; }
                }
                if (!dead) runnerLine = "runner: its file did not appear within 8 s (is it running?)";
            }, "henge-runner-attach").start();
            new Thread(() -> {                           // ask for its status line now and then (the guest's counters live in the runner)
                while (!dead && runner == p && p.isAlive()) {
                    try { OutputStream os = p.getOutputStream(); os.write("status\n".getBytes()); os.flush(); } catch (java.io.IOException e) { return; }
                    try { Thread.sleep(2000); } catch (InterruptedException e) { return; }
                }
            }, "henge-runner-poll").start();
        } catch (java.io.IOException e) {
            runnerLine = "runner: could not be started: " + e.getMessage();
        }
    }

    /** Stop sequence for the runner, off the UI thread: the app is closing, "quit", wait up to 5 s, then (and only then) kill. Logged either way. */
    private void stopRunnerProcess() {
        final Process p = runner;
        if (p == null) return;
        runner = null;
        try { nativeRunnerAppClose(); } catch (Throwable ignored) { }   // may already be gone: then the "quit" below does it
        new Thread(() -> {
            try { OutputStream os = p.getOutputStream(); os.write("quit\n".getBytes()); os.flush(); } catch (java.io.IOException ignored) { }
            boolean ended = false;
            try { ended = p.waitFor(5, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
            if (ended) Log.i(TAG, "runner stopped, exit code " + p.exitValue());
            else { Log.w(TAG, "runner did not stop within 5 s: FORCED kill"); p.destroyForcibly(); }
        }, "henge-runner-stop").start();
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
        dead = true;
        if (started) {
            started = false;
            nativeClose();                       // 1. the buffer takes no more frames, no new render enters
            if (stopRender()) {                  // 2. the render thread is confirmed ended
                nativeDestroy();                 // 3. free (the right is given to one caller only)
                if (runnerMode) stopRunnerProcess();   // 4. the runner: after the app has let go of its file
            } else {
                // The thread is still inside a native call. Nothing is freed and no new thread is started; the thread frees when it ends.
                Log.w(TAG, "the render thread did not end within 2 s; the native buffers are freed when it does");
                if (runnerMode) stopRunnerProcess();   // the runner is stopped anyway: it does not depend on the render thread
            }
        }
        super.onDestroy();
    }

    private void startRender() {
        Thread old = renderThread;
        if (old != null && !old.isAlive()) { renderThread = null; old = null; }   // one that was stuck and has ended since
        if (!started || dead || old != null || !surfaceReady) return;             // a thread that is still alive (stuck) blocks a second one
        stopRender = false;
        Thread t = new Thread(() -> {
            long lastStatus = 0;
            while (!stopRender) {
                long now = SystemClock.uptimeMillis();
                nativeTick(now);
                if (nativeRender() < 0) break;
                if (now - lastStatus >= 250 && !dead) {
                    lastStatus = now;
                    final String s = nativeStatus() + (runnerMode ? "\n" + runnerLine : "");
                    ui.post(() -> { if (!dead && status != null) status.setText(s); });
                }
                long wait = 16 - (SystemClock.uptimeMillis() - now);
                if (wait > 0) try { Thread.sleep(wait); } catch (InterruptedException e) { break; }
            }
            // the Activity is gone and this thread was not joined in time: this is the later clean-up (nativeDestroy gives the right to free to one caller only)
            if (dead) nativeDestroy();
        }, "henge-ui-render");
        renderThread = t;
        t.start();
    }

    /** true: no render thread exists any more (ended, or there was none). false: it is still alive after 2 s; it stays referenced. */
    private boolean stopRender() {
        Thread t = renderThread;
        stopRender = true;
        if (t == null) return true;
        t.interrupt();
        try { t.join(2000); } catch (InterruptedException ignored) { }
        if (t.isAlive()) return false;
        renderThread = null;
        return true;
    }

    @Override public void surfaceCreated(SurfaceHolder h) { nativeSetSurface(h.getSurface()); surfaceReady = true; startRender(); }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { nativeSetView(w, hh); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { surfaceReady = false; stopRender(); nativeSetSurface(null); }

    private void handleTouch(MotionEvent e) {
        if (!started || dead) return;
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
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (!started || dead) return super.dispatchKeyEvent(e);
        int a = e.getAction();
        if (a != KeyEvent.ACTION_DOWN && a != KeyEvent.ACTION_UP) return super.dispatchKeyEvent(e);
        // the guest repeats a held key itself: Android's repeats are not sent
        if (a == KeyEvent.ACTION_DOWN && e.getRepeatCount() > 0) return nativeKeyMapped(e.getKeyCode()) == 1 || super.dispatchKeyEvent(e);
        if (nativeKey(e.getKeyCode(), a == KeyEvent.ACTION_DOWN) == 1) return true;
        return super.dispatchKeyEvent(e);   // Back and the other keys the guest does not get
    }
}
