package dev.muns.hengeprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.IntSupplier;

/**
 * [G7-EXEC] Only in the g7exec source set, and the ONLY place that starts the clone-boot runner (libg7cloneboot.so = host/ui_runner_vm_clone.c): a normal (non-installer) boot of a VERIFIED CLONE of the
 * install pair, whose disk and AUX the runner may write. It is a boot test of the COPY: it never names the original pair's files.
 *
 * Before a process is created, in this order, each refusing on its own: the build's start switch (the same switch as the restore and the deletion: false in the g7exec build, true only in the start build);
 * a typed consent; no live G7 run and no live clone boot; no runner process found (or unknown); no unfinished isolation; the runner file packaged; a usable ROM; a valid ECID; and G7Clone.validate() of the
 * folder (finished marker, only known files, no links, sizes, ECID tag equal to the current ECID). The ECID is not on the command line: the runner reads vm_ecid.txt from --ecid-dir itself.
 * The run is bounded (RUN_SECONDS), writes its own log (g7_cboot_*.txt, CREATE_NEW) and records the stage estimate (G7BootStage). Cancel asks the runner to stop ("quit"), then ends it after a wait.
 * No Android classes: tests/java/G7CloneBootTest.java runs on a plain JVM with a fake process.
 */
public final class G7CloneBoot {
    private G7CloneBoot() {}

    static final String LIB_NAME = "libg7cloneboot.so";
    static final int RUN_SECONDS = 900;
    static final String LOG_PREFIX = "g7_cboot_", LOG_SUFFIX = ".txt";
    static final int MAX_LOG_BYTES = 2 * 1024 * 1024;
    static final long STATUS_EVERY_MS = 30_000, STOP_WAIT_MS = 15_000;

    interface Spawner { Process spawn(List<String> command) throws IOException; }
    private static final Spawner REAL = c -> new ProcessBuilder(c).redirectErrorStream(true).start();

    public static final class Start {
        public final boolean started; public final String message; public final Run run;
        Start(boolean s, String m, Run r) { started = s; message = m; run = r; }
    }

    static volatile Run CURRENT;
    static boolean hasLive() { Run c = CURRENT; return c != null && !c.finished(); }

    /** The clone folders under vm/clones/ that pass G7Clone.validate() for this ECID (the list the person picks from). Looks only. */
    public static List<File> listValid(File priv) {
        List<File> out = new ArrayList<>();
        long ecid = G7ProbeLauncher.readEcid(priv);
        File[] k = new File(priv, "clones").listFiles();
        if (k == null || ecid <= 0) return out;
        Arrays.sort(k);
        for (File d : k) if (G7Clone.validate(d, priv, ecid) == null) out.add(d);
        return out;
    }

    public static Run current() { return CURRENT; }

    public static boolean isPackaged(File nativeLibDir) { return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile(); }

    public static final class Run {
        final Process proc; final String logName; final G7BootStage.Tracker stages = new G7BootStage.Tracker();
        private final StringBuilder tail = new StringBuilder();
        private volatile boolean finished; private volatile int exitCode = Integer.MIN_VALUE; private volatile boolean cancelled;
        private final long t0 = System.currentTimeMillis();
        private OutputStream log; private int written;
        Run(Process p, String logName) { this.proc = p; this.logName = logName; }
        public boolean finished() { return finished; }
        public int exitCode() { return exitCode; }
        public String logName() { return logName; }
        public synchronized String tail() { return tail.toString(); }
        public String stageSummary() { return stages.summary(); }
        synchronized void put(String text, boolean always) {
            tail.append(text); if (tail.length() > 6000) tail.replace(0, tail.length() - 6000, "");
            if (log == null) return;
            byte[] b = text.getBytes(StandardCharsets.UTF_8);
            if (!always && written + b.length > MAX_LOG_BYTES) return;
            try { log.write(b); written += b.length; log.flush(); } catch (IOException ignored) { /* the screen tail still has it */ }
        }
        /** Asks the runner to stop on its own ("quit" on its input); if it is still there after STOP_WAIT_MS it is ended. */
        public void cancel() {
            cancelled = true;
            new Thread(() -> {
                try { OutputStream o = proc.getOutputStream(); o.write("quit\n".getBytes(StandardCharsets.US_ASCII)); o.flush(); } catch (IOException ignored) { }
                try { if (!proc.waitFor(STOP_WAIT_MS, TimeUnit.MILLISECONDS)) proc.destroy(); } catch (InterruptedException ignored) { }
            }, "g7-cboot-cancel").start();
        }
    }

    public static Start start(File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus) {
        return startWith(REAL, G7ProbeLauncher::usableRomFile, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus);
    }

    static Start startWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus) {
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = G7Isolation.gate(enabled, g7Live, runnerStatus);
            if (g != null) return new Start(false, g.replace("격리하지", "부팅하지").replace("아무것도 옮기지", "아무것도 시작하지"), null);
            if (!consent) return new Start(false, "동의가 체크되지 않아 복제본 부팅을 시작하지 않았습니다.", null);
            if (G7Isolation.pending(priv)) return new Start(false, "끝나지 않은 격리가 있어 부팅하지 않았습니다.", null);
            if (!isPackaged(nativeLibDir)) return new Start(false, "복제본 부팅 실행 파일(" + LIB_NAME + ")이 이 빌드에 없어 시작하지 않았습니다.", null);
            File rom = roms.find(extVmDir, priv);
            long ecid = G7ProbeLauncher.readEcid(priv);
            if (rom == null || ecid <= 0) return new Start(false, "ROM 또는 ECID를 확인하지 못해 시작하지 않았습니다.", null);
            String bad = G7Clone.validate(cloneDir, priv, ecid);
            if (bad != null) return new Start(false, "이 복제본으로 부팅하지 않았습니다: " + bad, null);
            File log = new File(filesDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + LOG_SUFFIX);
            OutputStream logOut;
            try { logOut = Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE); }
            catch (IOException e) { return new Start(false, "로그 파일을 열지 못해 시작하지 않았습니다.", null); }
            List<String> cmd = new ArrayList<>();
            cmd.add(new File(nativeLibDir, LIB_NAME).getPath());
            cmd.add("--shm"); cmd.add(new File(filesDir, "g7_cloneboot.shm").getPath());
            cmd.add("--rom"); cmd.add(rom.getPath());
            cmd.add("--clone-dir"); cmd.add(cloneDir.getAbsolutePath());
            cmd.add("--ecid-dir"); cmd.add(priv.getPath());
            cmd.add("--seconds"); cmd.add(String.valueOf(RUN_SECONDS));
            cmd.add("--ram-mb"); cmd.add("2048"); cmd.add("--cpus"); cmd.add("2"); cmd.add("--jit-mb"); cmd.add("64");
            Process p;
            try { p = sp.spawn(cmd); }
            catch (IOException e) { try { logOut.close(); } catch (IOException ignored) { } return new Start(false, "실행 파일을 시작하지 못했습니다: " + G7ExecLauncher.mask(String.valueOf(e.getMessage()), ecid), null); }
            final Run run = new Run(p, log.getName());
            run.log = logOut;
            G7ExecLauncher.OTHER_LIVE = G7CloneBoot::hasLive;
            CURRENT = run;
            watch(run, p, ecid);
            return new Start(true, "복제본(" + cloneDir.getName() + ") 부팅을 시작했습니다. 원본 설치 쌍은 사용하지 않습니다.", run);
        }
    }

    private static void watch(final Run run, final Process p, final long ecid) {
        final Thread reader = new Thread(() -> {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                run.put("g7-cboot-start: " + System.currentTimeMillis() + "\n", true);
                String line;
                while ((line = r.readLine()) != null) {
                    line = G7ExecLauncher.mask(line, ecid);
                    G7BootStage.Stage s = run.stages.feed(line, System.currentTimeMillis() - run.t0);
                    run.put(line + "\n", false);
                    if (s != null) run.put("g7-stage: " + s.name().toLowerCase() + " first seen at " + (System.currentTimeMillis() - run.t0) / 1000 + "s (estimate)\n", true);
                }
            } catch (IOException ignored) { }
        }, "g7-cboot-out");
        reader.start();
        final Thread poller = new Thread(() -> {   // one status line from the runner every STATUS_EVERY_MS (its frame and disk-write counters; the screen stage comes from it)
            try {
                while (!run.finished) {
                    Thread.sleep(STATUS_EVERY_MS);
                    if (run.finished) break;
                    try { OutputStream o = p.getOutputStream(); o.write("status\n".getBytes(StandardCharsets.US_ASCII)); o.flush(); } catch (IOException e) { break; }
                }
            } catch (InterruptedException ignored) { }
        }, "g7-cboot-status");
        poller.setDaemon(true); poller.start();
        new Thread(() -> {
            int code;
            try { code = p.waitFor(); } catch (InterruptedException e) { code = -1; }
            try { reader.join(5000); } catch (InterruptedException ignored) { }
            run.exitCode = code;
            run.put("g7-cboot-exit: " + code + (run.cancelled ? " (cancelled)" : "") + "\n" + run.stages.summary() + "\n", true);
            synchronized (run) { try { if (run.log != null) run.log.close(); } catch (IOException ignored) { } run.log = null; }
            run.finished = true;
            poller.interrupt();
        }, "g7-cboot-wait").start();
    }
}
