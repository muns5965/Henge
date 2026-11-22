package dev.muns.hengeprobe;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.concurrent.TimeUnit;

/** [A] The real VM's screen. Unlike DisplayTestActivity (C0+B0: a synthetic picture, no virtual machine), this
 *  screen shows the ACTUAL guest, through the VM runner (host/ui_runner_vm.c) started the one place that is
 *  allowed to: VmRunnerLauncher -- see that class for the compile-time switch deciding whether starting it is
 *  refused or allowed in this build (currently: allowed, bounded by RUN_SECONDS and the watchdog below).
 *  onCreate() checks that switch BEFORE anything else -- before nativeStart(), before reading or validating the
 *  ROM/disk/aux paths, before touching any file -- so when the switch is off this screen only ever shows the
 *  refusal text and does nothing further; when it is on, as in this build, onCreate() goes on to validate the
 *  rom/disk/aux paths and only then starts the native pipeline and the VM runner. Flipping that switch either way
 *  is a separate change with its own approval.
 *
 *  ROM/disk/aux: this screen does not default or create them. All three must be given as explicit Intent extra
 *  paths ("rom", "disk", "aux") and must already exist; if any is missing or does not exist, VmRunnerLauncher.start()
 *  is never called and nativeStart() never runs.
 *
 *  Rendering reuses the exact native pipeline DisplayTestActivity's runner mode (C1-1) uses -- same shared file
 *  protocol (host/frame_shm.h), same JNI entry points (see android/app/src/main/cpp/ui_jni.c's impl_* functions,
 *  exported here under this class's own JNI names) -- pointed at the VM runner (named only in VmRunnerLauncher)
 *  instead of the synthetic one DisplayTestActivity starts directly. The two screens never run at once (one
 *  native singleton backs both), which is fine: they are alternative screens, not simultaneous ones.
 *
 *  Shutdown mirrors DisplayTestActivity's: nativeClose (no more frames in), the render thread stopped AND
 *  CONFIRMED ended, then nativeDestroy; the VM runner process (if one was actually started) is told the app is
 *  closing, sent "quit", given 5 s, and only then killed -- logged either way. */
public class VmDisplayActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "henge-ui-vm";
    private static final int REQ_EXPORT_LOG = 1;

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
    // [2026-11-22] The guest's current picture size (width in the high 32 bits, height in the low 32 bits; 0
    // before the first frame) -- host/ui_jni.c's impl_nativeFrameSize(). Used by fitSurfaceToFrame() below so
    // this screen's SurfaceView is sized to the guest's own aspect ratio instead of the full (differently
    // shaped) phone screen; see that method's comment for why a full-screen SurfaceView distorts the picture.
    private static native long nativeFrameSize();

    // the same numbers as host/touch_input.h
    private static final int T_DOWN = 0, T_UP = 1, T_MOVE = 2, T_CANCEL = 3;

    // host/ui_runner_vm.c's own startup line (NOT ui_runner.c's "ui_runner: started pid " -- see its header comment)
    private static final String STARTED_PID_PREFIX = "ui_runner_vm: started pid ";

    private SurfaceView surface;
    private TextView status;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile Thread renderThread;
    private volatile boolean stopRender;
    private volatile boolean dead;          // onDestroy ran: nothing may touch the views any more; the render thread cleans up when it ends
    private boolean surfaceReady;
    private boolean started;                // nativeStart() succeeded
    private boolean vmStarted;               // VmRunnerLauncher actually started a process for this screen instance
    // The last frame size fitSurfaceToFrame() actually laid the SurfaceView out for, read only from the render
    // thread (never the UI thread) -- so a same-size frame is a no-op instead of posting a relayout every tick.
    private int fitW, fitH;

    private volatile Process runner;
    private volatile long runnerPid;
    private volatile String runnerLine = "vm runner: not started";
    private final java.util.ArrayDeque<String> runnerLog = new java.util.ArrayDeque<>();
    // Every line the VM runner process prints, not just the last 40 kept in runnerLog above for the on-screen
    // status overlay (which has no scrollback): written as each line arrives so a hang or crash still leaves
    // whatever was printed so far on disk, and exported on request via exportLogBtn/onActivityResult below --
    // same ACTION_CREATE_DOCUMENT pattern SnapshotViewActivity already uses for snapshot.bmp. One fixed path per
    // app (overwritten at the start of each run, same convention as vm/snapshot.bmp): this screen shows one VM
    // at a time, so the previous run's log is not needed once a new one starts.
    private File logFile;
    private volatile Writer logWriter;
    // guards stopVmRunner() against running twice for the same process: onDestroy() (user closed the screen) and
    // the watchdog thread below (the native --seconds limit did not stop it) can both decide to stop the same
    // runner at roughly the same time; StopOnce.tryOnce() (tests/java/StopOnceTest.java covers it on a plain JVM)
    // is what actually decides which of the two, if either, proceeds. Re-made in startVmRunner() for each process.
    private volatile StopOnce stopOnce = new StopOnce();

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        logFile = new File(getExternalFilesDir(null), "vm/vm_display_last.log");
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        surface.setOnTouchListener((v, e) -> { handleTouch(e); return true; });
        root.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setBackgroundColor(0xAA000000);
        status.setTypeface(Typeface.MONOSPACE);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        status.setPadding(12, 8, 12, 8);
        status.setText("[A] VM display (real guest). Starting...");
        root.addView(status, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        final Button exportLogBtn = new Button(this);
        exportLogBtn.setText("로그 내보내기");
        exportLogBtn.setOnClickListener(v -> exportLog());
        root.addView(exportLogBtn, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END));
        setContentView(root);

        // The launcher's switch is checked FIRST, before nativeStart() or any file access: when VM execution
        // is blocked there is nothing to display and nothing to prepare, so neither should happen. An earlier
        // version called nativeStart() (and, once paths were given, prepared the shared-memory directory)
        // unconditionally; caught in review, fixed by moving every native/file step behind this check and the
        // path checks below, in that order.
        if (!VmRunnerLauncher.startEnabled()) {
            status.setText("vm display: " + VmRunnerLauncher.REFUSAL);
            return;
        }
        String romPath = getIntent().getStringExtra("rom");
        String diskPath = getIntent().getStringExtra("disk");
        String auxPath = getIntent().getStringExtra("aux");
        if (romPath == null || diskPath == null || auxPath == null) {
            status.setText("vm display: rom/disk/aux가 모두 Intent extra로 전달되지 않아 시작하지 않습니다");
            return;
        }
        File romFile = new File(romPath), diskFile = new File(diskPath), auxFile = new File(auxPath);
        if (!romFile.isFile() || !diskFile.isFile() || !auxFile.isFile()) {
            status.setText("vm display: rom/disk/aux 중 존재하지 않는 경로가 있어 시작하지 않습니다");
            return;
        }

        int r = nativeStart();
        if (!NativeStart.succeeded(r)) {
            // r == 1 ("already running") is a failure for THIS caller too: it means the other screen
            // (DisplayTestActivity) already owns the one native singleton -- this Activity built nothing of
            // its own and must not touch Surface/runner/native-destroy as if it had. See NativeStart's own
            // comment for the full reasoning (shared, since the symmetric bug existed in DisplayTestActivity).
            status.setText("nativeStart failed (" + r + NativeStart.refusalReason(r) + ")");
            return;
        }
        started = true;
        nativeUseRunner(true);   // this screen only ever shows the shared-file source; there is no synthetic fallback
        startVmRunner(romFile, diskFile, auxFile);
    }

    /** Reached only once VmRunnerLauncher.startEnabled() and the three paths above were already confirmed, and
     *  nativeStart() already succeeded. Prepares the shared-memory directory, asks VmRunnerLauncher to start the
     *  VM runner, and (if it actually did) reads its output and attaches to its shared file -- same pattern as
     *  DisplayTestActivity.startRunnerProcess(), but through VmRunnerLauncher (the only place allowed to create
     *  this process) instead of a direct process-start call.
     *
     *  Now that this build actually reaches this method (startEnabled() is true): the delete below guards against
     *  an earlier run's leftover shared-memory file being mistaken for this one's (by pid, once attached), the
     *  same pattern DisplayTestActivity's synthetic-runner path already uses -- that pattern, including the
     *  VM-restart/leftover-shm-file case specifically, is the one already exercised and confirmed by CI
     *  (tools/c1vm-test.sh); it does not additionally confirm no earlier process is still live, same as that
     *  already-reviewed path. */
    private void startVmRunner(File romFile, File diskFile, File auxFile) {
        final File nativeLibDir = new File(getApplicationInfo().nativeLibraryDir);
        final File dir = new File(getFilesDir(), "vm1");
        final File shm = new File(dir, "vm.shm");
        dir.mkdirs();
        shm.delete();   // a file left by an earlier run must not be mistaken for this one's
        VmRunnerLauncher.Result res = VmRunnerLauncher.start(nativeLibDir, shm, romFile, diskFile, auxFile);
        if (!res.started) {
            // nativeStart() already ran (started == true): give back what it took before leaving.
            runnerLine = "vm runner: " + res.reason;
            releaseNative();
            started = false;
            return;
        }
        vmStarted = true;
        final Process p = res.process;
        stopOnce = new StopOnce();   // fresh race winner-takes-one guard for this specific process
        runner = p;
        runnerPid = 0;
        openLogWriter();
        new Thread(() -> {
            try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String ln;
                while ((ln = br.readLine()) != null) {
                    synchronized (runnerLog) { runnerLog.addLast(ln); while (runnerLog.size() > 40) runnerLog.removeFirst(); }
                    if (ln.startsWith("ui_runner")) runnerLine = ln;   // covers both "ui_runner:" and "ui_runner_vm:" lines
                    if (ln.startsWith(STARTED_PID_PREFIX)) {
                        try { runnerPid = Long.parseLong(ln.substring(STARTED_PID_PREFIX.length()).split("[ ,]")[0]); } catch (NumberFormatException ignored) { }
                    }
                    Log.i(TAG, ln);
                    writeLogLine(ln);
                }
            } catch (java.io.IOException ignored) { }
            try { int code = p.waitFor(); runnerLine = "vm runner exited, code " + code + (code == 0 ? " (stopped, confirmed)" : code == 3 ? " (stop NOT confirmed by the runner)" : " (not a clean stop)") + "; last: " + runnerLine; Log.i(TAG, "vm runner exit code " + code); writeLogLine(runnerLine); }
            catch (InterruptedException ignored) { }
            closeLogWriter();
        }, "henge-vm-runner-out").start();
        new Thread(() -> {
            String path = shm.getAbsolutePath();
            long t0 = SystemClock.uptimeMillis();
            while (!dead && SystemClock.uptimeMillis() - t0 < 8000) {
                long pid = runnerPid;
                if (pid > 0) {
                    int rc = nativeAttachRunner(path, pid);
                    if (rc >= 0) { Log.i(TAG, "attached to the vm runner, pid " + pid); return; }
                    if (rc == -1) return;
                }
                try { Thread.sleep(50); } catch (InterruptedException e) { return; }
            }
            if (!dead) runnerLine = "vm runner: its file did not appear within 8 s (is it running?)";
        }, "henge-vm-runner-attach").start();
        new Thread(() -> {
            while (!dead && runner == p && p.isAlive()) {
                try { OutputStream os = p.getOutputStream(); os.write("status\n".getBytes()); os.flush(); } catch (java.io.IOException e) { return; }
                try { Thread.sleep(2000); } catch (InterruptedException e) { return; }
            }
        }, "henge-vm-runner-poll").start();
        startWatchdog(p);
    }

    /** Opens logFile fresh (truncating any previous run's log, same one-shared-path convention as vm/snapshot.bmp),
     *  called once right before the reader thread that will write to it starts. A failure to open (e.g. the
     *  directory could not be created) just means nothing is logged to disk this run -- the in-memory runnerLog/
     *  runnerLine status display above is unaffected either way, so this never blocks or fails the VM run itself. */
    private void openLogWriter() {
        try {
            File parent = logFile.getParentFile();
            if (parent != null) parent.mkdirs();
            logWriter = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(logFile, false)));
        } catch (IOException e) {
            logWriter = null;
            Log.w(TAG, "could not open vm display log file: " + e.getMessage());
        }
    }

    /** Appends one line and flushes immediately (not buffered across lines) so a hang or a forced kill still
     *  leaves everything printed so far on disk, not just whatever happened to be flushed already. Called from
     *  the single reader thread only, so no synchronization is needed for the write itself; logWriter is
     *  volatile only so closeLogWriter() (same thread, just later) and this always see the same reference. */
    private void writeLogLine(String ln) {
        Writer w = logWriter;
        if (w == null) return;
        try { w.write(ln); w.write('\n'); w.flush(); } catch (IOException ignored) { }
    }

    private void closeLogWriter() {
        Writer w = logWriter;
        logWriter = null;
        if (w != null) try { w.close(); } catch (IOException ignored) { }
    }

    /** Same ACTION_CREATE_DOCUMENT pattern as SnapshotViewActivity's exportBtn: hands a copy of logFile to a
     *  folder the user picks, without requesting any storage permission. */
    private void exportLog() {
        if (logFile == null || !logFile.isFile()) {
            Toast.makeText(this, "로그 파일이 아직 없습니다 (VM을 먼저 한 번 실행하세요)", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TITLE, "henge_vm_display_log.txt");
        startActivityForResult(i, REQ_EXPORT_LOG);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_EXPORT_LOG || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null || logFile == null) return;
        try (InputStream in = new FileInputStream(logFile); OutputStream out = getContentResolver().openOutputStream(uri)) {
            if (out == null) throw new IOException("출력 스트림을 열 수 없습니다");
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            Toast.makeText(this, "로그 내보내기 완료: " + uri, Toast.LENGTH_SHORT).show();
        } catch (IOException e) {
            Toast.makeText(this, "로그 내보내기 실패: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    /** Independent of onDestroy()/the user closing this screen: host/ui_runner_vm.c's own --seconds limit (see
     *  VmRunnerLauncher.RUN_SECONDS) only applies once its main loop is reached, so a hang before that -- most
     *  notably inside hv_vm_create() -- is not bounded by it at all. This thread is the backstop: it waits
     *  RUN_SECONDS plus a margin and, if the process given to it is still the current one and still alive, stops
     *  it through the same stopVmRunner() path onDestroy() uses (guarded by stopOnce, so whichever of the two
     *  gets there first is the only one that actually acts). A margin, not an exact match, because the native
     *  process also needs a moment after its own deadline to notice, log and exit on its own first. */
    private static final int WATCHDOG_MARGIN_SECONDS = 15;

    private void startWatchdog(Process p) {
        final long deadline = SystemClock.uptimeMillis() + (VmRunnerLauncher.RUN_SECONDS + WATCHDOG_MARGIN_SECONDS) * 1000L;
        new Thread(() -> {
            while (!dead && runner == p && p.isAlive() && SystemClock.uptimeMillis() < deadline) {
                try { Thread.sleep(500); } catch (InterruptedException e) { return; }
            }
            if (!dead && runner == p && p.isAlive()) {
                Log.w(TAG, "watchdog: still running " + (VmRunnerLauncher.RUN_SECONDS + WATCHDOG_MARGIN_SECONDS) + "s after start (its own --seconds limit did not stop it) -- forcing stop");
                stopVmRunner("watchdog: native time limit did not stop it");
            }
        }, "henge-vm-runner-watchdog").start();
    }

    /** Same stop sequence as DisplayTestActivity.stopRunnerProcess(): app-closing notice, "quit", wait up to 5 s,
     *  only then kill. Logged either way, together with `why` (which of the two callers -- onDestroy()'s "user
     *  closed the screen" or the watchdog's "native time limit did not stop it" -- is acting), so a later look at
     *  the log can tell a normal close apart from a forced one. A no-op when nothing was actually started (the
     *  launcher's switch is off here), and a no-op for whichever of the two callers loses the race: stopOnce
     *  (tests/java/StopOnceTest.java) gives the actual work below to exactly one of them per process. */
    private void stopVmRunner(String why) {
        final Process p = runner;
        if (p == null || !stopOnce.tryOnce()) return;
        runner = null;
        Log.i(TAG, "stopping the vm runner (" + why + ")");
        try { nativeRunnerAppClose(); } catch (Throwable ignored) { }
        new Thread(() -> {
            try { OutputStream os = p.getOutputStream(); os.write("quit\n".getBytes()); os.flush(); } catch (java.io.IOException ignored) { }
            boolean ended = false;
            try { ended = p.waitFor(5, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
            if (ended) Log.i(TAG, "vm runner stopped (" + why + "), exit code " + p.exitValue());
            else { Log.w(TAG, "vm runner did not stop within 5 s (" + why + "): FORCED kill"); p.destroyForcibly(); }
        }, "henge-vm-runner-stop").start();
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
            // Order matters: stopVmRunner() calls nativeRunnerAppClose() SYNCHRONOUSLY (the actual "quit"
            // send and exit wait run in their own thread afterward, but the app-closing notice itself does
            // not), so calling it before releaseNative() guarantees that notice is set on the shared file
            // while g_shm still exists. releaseNative() -> nativeDestroy() NULLs and closes g_shm; the
            // previous order let that happen first, so the notice silently found no g_shm to set it on.
            // This only fixes the notice's OWN delivery order -- it says nothing about whether the VM
            // process has actually read that flag or exited, which stays a background concern (A-4,
            // unchanged) and an actual-run question, not a memory-safety one (stopVmRunner()'s background
            // thread only touches the Java Process object, never g_shm or any other native resource).
            if (vmStarted) stopVmRunner("user closed the screen");
            releaseNative();
        }
        super.onDestroy();
    }

    /** nativeClose() (no more frames in), the render thread stopped AND CONFIRMED ended, then nativeDestroy()
     *  (host/render_gate.h: freeing is given to one caller only). Used both here and by startVmRunner() above,
     *  for the case where nativeStart() already ran but VmRunnerLauncher.start() then failed regardless (reachable
     *  in this build, since startEnabled() is true -- see startVmRunner() above). */
    private void releaseNative() {
        nativeClose();
        if (stopRender()) {
            nativeDestroy();
        } else {
            // The thread is still inside a native call. Nothing is freed and no new thread is started; the
            // thread frees when it ends (see startRender()'s own nativeDestroy() at its loop's end).
            Log.w(TAG, "the render thread did not end within 2 s; the native buffers are freed when it does");
        }
    }

    private void startRender() {
        Thread old = renderThread;
        if (old != null && !old.isAlive()) { renderThread = null; old = null; }
        if (!started || dead || old != null || !surfaceReady) return;
        stopRender = false;
        Thread t = new Thread(() -> {
            long lastStatus = 0;
            while (!stopRender) {
                long now = SystemClock.uptimeMillis();
                nativeTick(now);
                if (nativeRender() < 0) break;
                if (now - lastStatus >= 250 && !dead) {
                    lastStatus = now;
                    final String s = nativeStatus() + "\n" + runnerLine;
                    ui.post(() -> { if (!dead && status != null) status.setText(s); });
                    fitSurfaceToFrame();
                }
                long wait = 16 - (SystemClock.uptimeMillis() - now);
                if (wait > 0) try { Thread.sleep(wait); } catch (InterruptedException e) { break; }
            }
            if (dead) nativeDestroy();
        }, "henge-ui-vm-render");
        renderThread = t;
        t.start();
    }

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

    // [2026-11-22] surface's own LayoutParams starts as MATCH_PARENT x MATCH_PARENT (the whole screen) -- fine
    // for the phone's own, fixed-shape screen, but the guest's picture has its own aspect ratio (whatever the
    // guest reports through host/vm.h's HvBootDisplay), which does not generally match the phone's. host/ui_jni.c's
    // native renderer already sizes the ANativeWindow's BUFFER to the guest's own width/height (never stretched
    // there); the stretch instead happened one level up, in Android's own surface compositor, which fills
    // whatever rectangle the SurfaceView is LAID OUT at -- the full screen -- by scaling that fixed-size buffer
    // onto it independently on each axis. Re-sizing the SurfaceView itself to the guest's own ratio (letterboxed
    // within the available screen area, centered) removes that second, uncontrolled scale: Android then scales
    // the buffer uniformly to fill the correctly-shaped rectangle. Called off the render thread every ~250 ms
    // (piggybacking on the existing status-text throttle, not every frame); does nothing until nativeFrameSize()
    // reports a first frame, and does nothing more once this size has already been applied once.
    private void fitSurfaceToFrame() {
        long packed = nativeFrameSize();
        int w = (int) (packed >>> 32), h = (int) packed;
        if (w <= 0 || h <= 0 || (w == fitW && h == fitH)) return;
        fitW = w; fitH = h;
        ui.post(() -> {
            if (dead || surface == null) return;
            ViewGroup parent = (ViewGroup) surface.getParent();
            int pw = parent != null ? parent.getWidth() : 0, ph = parent != null ? parent.getHeight() : 0;
            if (pw <= 0 || ph <= 0) return;   // not laid out yet -- the next tick tries again
            // fit w x h inside pw x ph, preserving its ratio (letterboxed/pillarboxed), then center it
            int targetW, targetH;
            if ((long) w * ph > (long) h * pw) { targetW = pw; targetH = (int) ((long) h * pw / w); }
            else { targetH = ph; targetW = (int) ((long) w * ph / h); }
            if (targetW <= 0 || targetH <= 0) return;
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surface.getLayoutParams();
            if (lp.width == targetW && lp.height == targetH) return;
            lp.width = targetW; lp.height = targetH; lp.gravity = Gravity.CENTER;
            surface.setLayoutParams(lp);
        });
    }

    // Guarded by `started`, not by any assumption about Surface-vs-nativeStart() timing (nativeStart() runs
    // synchronously inside onCreate(), before any Surface callback can fire, in ordinary Android lifecycle
    // order -- see review discussion): when onCreate() left `started` false (VM execution blocked, paths
    // missing/invalid, or nativeStart() itself failed), there is no native pipeline to hand a Surface to, so
    // these callbacks must not call into it regardless of why. The Java-side state (surfaceReady, the render
    // thread) is still tracked unconditionally; only the native calls are skipped.
    @Override public void surfaceCreated(SurfaceHolder h) { surfaceReady = true; if (started) nativeSetSurface(h.getSurface()); startRender(); }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { if (started) nativeSetView(w, hh); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { surfaceReady = false; stopRender(); if (started) nativeSetSurface(null); }

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
        if (a == KeyEvent.ACTION_DOWN && e.getRepeatCount() > 0) return nativeKeyMapped(e.getKeyCode()) == 1 || super.dispatchKeyEvent(e);
        if (nativeKey(e.getKeyCode(), a == KeyEvent.ACTION_DOWN) == 1) return true;
        return super.dispatchKeyEvent(e);
    }
}
