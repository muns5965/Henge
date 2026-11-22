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

/** [A-G4] The G4-track counterpart of the app's other real-VM screen: same shared-memory protocol
 *  (host/frame_shm.h), same native pipeline and the same JNI entry points (android/app/src/main/cpp/ui_jni.c's
 *  impl_* functions, exported here under THIS class's own JNI names -- JNI binds by the caller's fully-qualified
 *  class name, so a shared native implementation still needs one export block per Java class), pointed at the
 *  G4 diagnostic runner (named only in VmRunnerG4Launcher) instead of the other real-VM screen's runner. The
 *  three screens that back onto this one native singleton never run at once, which is fine: they are
 *  alternative screens, not simultaneous ones.
 *
 *  onCreate() checks VmRunnerG4Launcher.startEnabled() FIRST -- before nativeStart(), before reading or
 *  validating any of the six paths below, before touching any file -- exactly the same order the other real-VM
 *  screen already uses for its own switch, and for the same reason: when G4 execution is blocked there is
 *  nothing to display and nothing to prepare, so neither should happen. VmRunnerG4Launcher's own switch has no
 *  "observation only" mode (see that class's own header comment: starting the G4 runner always makes a real
 *  Apple TSS request and drives the guest through DFU->Recovery->RecoveryOS the moment it starts, whichever way
 *  the switch currently reads) -- in THIS build the switch is a literal true, by a fresh, explicit approval
 *  given for exactly this, so once the six paths below validate, this screen actually starts the G4 runner for
 *  real. A later change back to false, or any other change to this behavior, needs its own fresh approval.
 *
 *  rom/disk/aux/ipsw/ecid/workdir: this screen does not default, download or create any of the first five. All
 *  five must be given as explicit Intent extra values ("rom", "disk", "aux", "ipsw", "ecid", "workdir") and the
 *  first four must already exist as files; only workdir (where G4's own restore work files land) is a directory
 *  the caller is expected to have already created, same as the app's existing G4 install-diagnostic button does
 *  for its own, separate workdir. If any is missing, VmRunnerG4Launcher.start() is never called and nativeStart()
 *  never runs.
 *
 *  Kept structurally separate from the OTHER real-VM screen on purpose (a review raised the risk of a shared
 *  mode-flag accidentally starting that other screen's runner instead): this class never names that other
 *  screen's launcher anywhere in its source (checked by tools/check-g4ui-launcher-source.sh, the same way that
 *  script already guards the [G4 UI] information screen), uses its own shared-memory directory/file distinct
 *  from that other screen's, and keeps its own separate process/state fields -- its own `runner`, `runnerPid`,
 *  `started`, `vmStarted` and `stopOnce`, never the other screen's.
 *
 *  Shutdown mirrors the other real-VM screen's: nativeClose (no more frames in), the render thread stopped AND
 *  CONFIRMED ended, then nativeDestroy; the G4 runner process (if one was actually started) is told the app is
 *  closing, sent "quit", given 5 s, and only then killed -- logged either way. */
public class VmDisplayG4Activity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "henge-ui-vm-g4";
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

    // the same numbers as host/touch_input.h
    private static final int T_DOWN = 0, T_UP = 1, T_MOVE = 2, T_CANCEL = 3;

    // host/ui_runner_vm_g4.c's own startup line
    private static final String STARTED_PID_PREFIX = "ui_runner_vm_g4: started pid ";

    private SurfaceView surface;
    private TextView status;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile Thread renderThread;
    private volatile boolean stopRender;
    private volatile boolean dead;          // onDestroy ran: nothing may touch the views any more; the render thread cleans up when it ends
    private boolean surfaceReady;
    private boolean started;                // nativeStart() succeeded
    private boolean vmStarted;               // VmRunnerG4Launcher actually started a process for this screen instance

    private volatile Process runner;
    private volatile long runnerPid;
    private volatile String runnerLine = "g4 vm runner: not started";
    private final java.util.ArrayDeque<String> runnerLog = new java.util.ArrayDeque<>();
    // Same one-fixed-path-per-app convention as the other real-VM screen's own log, but its own separate file
    // (not shared with it), overwritten at the start of each run.
    private File logFile;
    private volatile Writer logWriter;
    // guards stopVmRunner() against running twice for the same process (onDestroy() vs the watchdog thread
    // below); re-made in startVmRunner() for each process. Its own instance, never shared with any other screen.
    private volatile StopOnce stopOnce = new StopOnce();

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        logFile = new File(getExternalFilesDir(null), "vm_g4/vm_display_g4_last.log");
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
        status.setText("[A-G4] G4 VM display. Starting...");
        root.addView(status, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        final Button exportLogBtn = new Button(this);
        exportLogBtn.setText("로그 내보내기");
        exportLogBtn.setOnClickListener(v -> exportLog());
        root.addView(exportLogBtn, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END));
        setContentView(root);

        // The launcher's switch is checked FIRST, before nativeStart() or any file access -- same discipline as
        // the other real-VM screen, and for the same reason: when G4 execution is blocked there is nothing to
        // display and nothing to prepare, so neither should happen.
        if (!VmRunnerG4Launcher.startEnabled()) {
            status.setText("g4 vm display: " + VmRunnerG4Launcher.REFUSAL);
            return;
        }
        String romPath = getIntent().getStringExtra("rom");
        String diskPath = getIntent().getStringExtra("disk");
        String auxPath = getIntent().getStringExtra("aux");
        String ipswPath = getIntent().getStringExtra("ipsw");
        String ecidHex = getIntent().getStringExtra("ecid");
        String workdirPath = getIntent().getStringExtra("workdir");
        if (romPath == null || diskPath == null || auxPath == null || ipswPath == null || ecidHex == null || ecidHex.isEmpty() || workdirPath == null) {
            status.setText("g4 vm display: rom/disk/aux/ipsw/ecid/workdir이 모두 Intent extra로 전달되지 않아 시작하지 않습니다");
            return;
        }
        File romFile = new File(romPath), diskFile = new File(diskPath), auxFile = new File(auxPath), ipswFile = new File(ipswPath), workdirFile = new File(workdirPath);
        if (!romFile.isFile() || !diskFile.isFile() || !auxFile.isFile() || !ipswFile.isFile() || !workdirFile.isDirectory()) {
            status.setText("g4 vm display: rom/disk/aux/ipsw 중 존재하지 않는 파일 또는 workdir 디렉터리가 없어 시작하지 않습니다");
            return;
        }

        int r = nativeStart();
        if (!NativeStart.succeeded(r)) {
            // r == 1 ("already running") is a failure for THIS caller too: it means another one of the three
            // screens already owns the one native singleton -- this Activity built nothing of its own and must
            // not touch Surface/runner/native-destroy as if it had.
            status.setText("nativeStart failed (" + r + NativeStart.refusalReason(r) + ")");
            return;
        }
        started = true;
        nativeUseRunner(true);   // this screen only ever shows the shared-file source; there is no synthetic fallback
        startVmRunner(romFile, diskFile, auxFile, ipswFile, ecidHex, workdirFile);
    }

    /** Reached only once VmRunnerG4Launcher.startEnabled() and the paths above were already confirmed, and
     *  nativeStart() already succeeded. Prepares ITS OWN shared-memory directory (never the other real-VM
     *  screen's), asks VmRunnerG4Launcher to start the G4 runner, and (if it actually did) reads its output and
     *  attaches to its shared file -- same pattern as the other real-VM screen's own startVmRunner(), but
     *  through VmRunnerG4Launcher (the only place allowed to create this specific process) instead. */
    private void startVmRunner(File romFile, File diskFile, File auxFile, File ipswFile, String ecidHex, File workdirFile) {
        final File nativeLibDir = new File(getApplicationInfo().nativeLibraryDir);
        final File dir = new File(getFilesDir(), "vm2");   // vm2, not vm1: never the other real-VM screen's shared-memory directory
        final File shm = new File(dir, "vm_g4.shm");
        dir.mkdirs();
        shm.delete();   // a file left by an earlier run must not be mistaken for this one's
        VmRunnerG4Launcher.Result res = VmRunnerG4Launcher.start(nativeLibDir, shm, romFile, diskFile, auxFile, ipswFile, ecidHex, workdirFile);
        if (!res.started) {
            // nativeStart() already ran (started == true): give back what it took before leaving.
            runnerLine = "g4 vm runner: " + res.reason;
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
                    if (ln.startsWith("ui_runner")) runnerLine = ln;   // covers both "ui_runner_vm_g4:" lines and its RESULT line
                    if (ln.startsWith(STARTED_PID_PREFIX)) {
                        try { runnerPid = Long.parseLong(ln.substring(STARTED_PID_PREFIX.length()).split("[ ,]")[0]); } catch (NumberFormatException ignored) { }
                    }
                    Log.i(TAG, ln);
                    writeLogLine(ln);
                }
            } catch (java.io.IOException ignored) { }
            try { int code = p.waitFor(); runnerLine = "g4 vm runner exited, code " + code + (code == 0 ? " (stopped, confirmed)" : code == 3 ? " (stop NOT confirmed by the runner)" : code == 4 ? " (hv_g4_wait did not end even after cancellation)" : " (not a clean stop)") + "; last: " + runnerLine; Log.i(TAG, "g4 vm runner exit code " + code); writeLogLine(runnerLine); }
            catch (InterruptedException ignored) { }
            closeLogWriter();
        }, "henge-vm-runner-g4-ui-out").start();
        new Thread(() -> {
            String path = shm.getAbsolutePath();
            long t0 = SystemClock.uptimeMillis();
            while (!dead && SystemClock.uptimeMillis() - t0 < 8000) {
                long pid = runnerPid;
                if (pid > 0) {
                    int rc = nativeAttachRunner(path, pid);
                    if (rc >= 0) { Log.i(TAG, "attached to the g4 vm runner, pid " + pid); return; }
                    if (rc == -1) return;
                }
                try { Thread.sleep(50); } catch (InterruptedException e) { return; }
            }
            if (!dead) runnerLine = "g4 vm runner: its file did not appear within 8 s (is it running?)";
        }, "henge-vm-runner-g4-ui-attach").start();
        new Thread(() -> {
            while (!dead && runner == p && p.isAlive()) {
                try { OutputStream os = p.getOutputStream(); os.write("status\n".getBytes()); os.flush(); } catch (java.io.IOException e) { return; }
                try { Thread.sleep(2000); } catch (InterruptedException e) { return; }
            }
        }, "henge-vm-runner-g4-ui-poll").start();
        startWatchdog(p);
    }

    /** Opens logFile fresh (truncating any previous run's log), called once right before the reader thread that
     *  will write to it starts. A failure to open just means nothing is logged to disk this run -- the in-memory
     *  runnerLog/runnerLine status display above is unaffected either way, so this never blocks or fails the run. */
    private void openLogWriter() {
        try {
            File parent = logFile.getParentFile();
            if (parent != null) parent.mkdirs();
            logWriter = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(logFile, false)));
        } catch (IOException e) {
            logWriter = null;
            Log.w(TAG, "could not open g4 vm display log file: " + e.getMessage());
        }
    }

    /** Appends one line and flushes immediately so a hang or a forced kill still leaves everything printed so
     *  far on disk. Called from the single reader thread only, so no synchronization is needed for the write
     *  itself; logWriter is volatile only so closeLogWriter() (same thread, just later) and this always see the
     *  same reference. */
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

    /** Same ACTION_CREATE_DOCUMENT pattern as the other real-VM screen's own exportLogBtn: hands a copy of
     *  logFile to a folder the user picks, without requesting any storage permission. */
    private void exportLog() {
        if (logFile == null || !logFile.isFile()) {
            Toast.makeText(this, "로그 파일이 아직 없습니다 (G4 VM을 먼저 한 번 실행하세요)", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TITLE, "henge_vm_display_g4_log.txt");
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

    /** Independent of onDestroy()/the user closing this screen: host/ui_runner_vm_g4.c's own --seconds limit
     *  (see VmRunnerG4Launcher.RUN_SECONDS) only applies once its main loop is reached, so a hang before that is
     *  not bounded by it at all. This thread is the backstop, same role as the other real-VM screen's own
     *  watchdog: it waits RUN_SECONDS plus a margin and, if the process given to it is still the current one and
     *  still alive, stops it through the same stopVmRunner() path onDestroy() uses (guarded by stopOnce). */
    private static final int WATCHDOG_MARGIN_SECONDS = 15;

    private void startWatchdog(Process p) {
        final long deadline = SystemClock.uptimeMillis() + (VmRunnerG4Launcher.RUN_SECONDS + WATCHDOG_MARGIN_SECONDS) * 1000L;
        new Thread(() -> {
            while (!dead && runner == p && p.isAlive() && SystemClock.uptimeMillis() < deadline) {
                try { Thread.sleep(500); } catch (InterruptedException e) { return; }
            }
            if (!dead && runner == p && p.isAlive()) {
                Log.w(TAG, "g4 watchdog: still running " + (VmRunnerG4Launcher.RUN_SECONDS + WATCHDOG_MARGIN_SECONDS) + "s after start (its own --seconds limit did not stop it) -- forcing stop");
                stopVmRunner("watchdog: native time limit did not stop it");
            }
        }, "henge-vm-runner-g4-ui-watchdog").start();
    }

    /** Same stop sequence as the other real-VM screen's own stopVmRunner(): app-closing notice, "quit", wait up
     *  to 5 s, only then kill. A no-op when nothing was actually started (the launcher's switch is off here), and
     *  a no-op for whichever of the two callers (onDestroy() vs the watchdog) loses the race: stopOnce gives the
     *  actual work below to exactly one of them per process. */
    private void stopVmRunner(String why) {
        final Process p = runner;
        if (p == null || !stopOnce.tryOnce()) return;
        runner = null;
        Log.i(TAG, "stopping the g4 vm runner (" + why + ")");
        try { nativeRunnerAppClose(); } catch (Throwable ignored) { }
        new Thread(() -> {
            try { OutputStream os = p.getOutputStream(); os.write("quit\n".getBytes()); os.flush(); } catch (java.io.IOException ignored) { }
            boolean ended = false;
            try { ended = p.waitFor(5, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
            if (ended) Log.i(TAG, "g4 vm runner stopped (" + why + "), exit code " + p.exitValue());
            else { Log.w(TAG, "g4 vm runner did not stop within 5 s (" + why + "): FORCED kill"); p.destroyForcibly(); }
        }, "henge-vm-runner-g4-ui-stop").start();
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
            // Order matters, same reasoning as the other real-VM screen's own onDestroy(): stopVmRunner() calls
            // nativeRunnerAppClose() SYNCHRONOUSLY before releaseNative() -> nativeDestroy() NULLs and closes
            // g_shm, so the notice is set on the shared file while it still exists.
            if (vmStarted) stopVmRunner("user closed the screen");
            releaseNative();
        }
        super.onDestroy();
    }

    /** nativeClose() (no more frames in), the render thread stopped AND CONFIRMED ended, then nativeDestroy()
     *  (host/render_gate.h: freeing is given to one caller only). Used both here and by startVmRunner() above,
     *  for the case where nativeStart() already ran but VmRunnerG4Launcher.start() then failed regardless. */
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
                }
                long wait = 16 - (SystemClock.uptimeMillis() - now);
                if (wait > 0) try { Thread.sleep(wait); } catch (InterruptedException e) { break; }
            }
            if (dead) nativeDestroy();
        }, "henge-ui-vm-g4-render");
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

    // Guarded by `started`, not by any assumption about Surface-vs-nativeStart() timing, same reasoning as the
    // other real-VM screen: when onCreate() left `started` false (G4 execution blocked, paths missing/invalid,
    // or nativeStart() itself failed), there is no native pipeline to hand a Surface to.
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
