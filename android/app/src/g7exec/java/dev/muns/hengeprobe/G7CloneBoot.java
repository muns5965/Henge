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

    /** One line per folder under vm/clones/: usable, or why it cannot be booted (so a clone that cannot boot is shown as such, not hidden). */
    public static List<String> describeAll(File priv) {
        List<String> out = new ArrayList<>();
        long ecid = G7ProbeLauncher.readEcid(priv);
        File[] k = new File(priv, "clones").listFiles();
        if (k == null) return out;
        Arrays.sort(k);
        for (File d : k) { String bad = G7Clone.validate(d, priv, ecid); out.add(d.getName() + ": " + (bad == null ? "부팅 가능(검증됨)" : "부팅 불가 - " + bad)); }
        return out;
    }

    public static Run current() { return CURRENT; }

    public static boolean isPackaged(File nativeLibDir) { return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile(); }

    /** One picture the runner printed (a downsampled copy of what the machine shows, at most 96x64, RGB) and its pixel-statistics class. The class is an ESTIMATE, not a decision about what is on the screen. */
    public static final class Frame {
        public final int w, h; public final byte[] rgb; public final String cls; public final int nonblackPct, colors;
        Frame(int w, int h, byte[] rgb, String cls, int pct, int colors) { this.w = w; this.h = h; this.rgb = rgb; this.cls = cls; this.nonblackPct = pct; this.colors = colors; }
    }
    private static final java.util.regex.Pattern FRAME = java.util.regex.Pattern.compile("^ui_runner_vm_clone FRAME src=\\d+x\\d+ out=(\\d{1,3})x(\\d{1,3}) class=([a-z-]{1,40}) nonblack_pct=(\\d{1,3}) colors=(\\d{1,5}) estimate=pixel-statistics-only data=([0-9a-f]+)$");
    /** The frame in a runner output line, or null (not a frame line, or a malformed one). */
    static Frame parseFrame(String line) {
        java.util.regex.Matcher m = FRAME.matcher(line);
        if (!m.matches()) return null;
        int w = Integer.parseInt(m.group(1)), h = Integer.parseInt(m.group(2));
        String hex = m.group(6);
        if (w < 1 || w > 96 || h < 1 || h > 64 || hex.length() != w * h * 6) return null;
        byte[] rgb = new byte[w * h * 3];
        for (int i = 0; i < rgb.length; i++) rgb[i] = (byte) Integer.parseInt(hex.substring(2 * i, 2 * i + 2), 16);
        return new Frame(w, h, rgb, m.group(3), Integer.parseInt(m.group(4)), Integer.parseInt(m.group(5)));
    }
    private static final java.util.regex.Pattern VERDICT = java.util.regex.Pattern.compile("^ui_runner_vm_clone VERDICT: vm_stop_confirmed=([01]) stop_kind=([a-z-]{1,24}) disk_flush=(ok|FAILED|not-attempted) macos_boot=not-determined.*$");

    public static final class Run {
        volatile Frame frame; volatile String verdictLine, frameNote = "아직 러너에 화면을 요청하지 않았습니다."; volatile long frameAnsweredMs;
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
        public Frame frame() { return frame; }
        /** What the runner last said about a picture: 'no picture yet' with the reason, or the class of the last one. */
        public String frameNote() { return frameNote; }
        /** Asks the runner for a picture now (it answers with one FRAME line). */
        public void requestFrame() {
            if (finished) return;
            try { OutputStream o = proc.getOutputStream(); o.write("frame\n".getBytes(StandardCharsets.US_ASCII)); o.flush(); } catch (IOException ignored) { }
        }
        /** What the end of the run says, with the two things kept apart: whether the machine's stop was confirmed (and who ended it), and macOS booting (never decided here). */
        public String verdict() {
            String v = verdictLine;
            if (v == null) return "러너가 결과 줄을 남기지 않고 끝났습니다(종료 코드 " + exitCode + "). VM 종료 확인 없음, 원인 미확인(비정상 종료 가능). macOS 부팅 성공 여부는 판정하지 않았습니다.";
            java.util.regex.Matcher m = VERDICT.matcher(v);
            if (!m.matches()) return "러너의 결과 줄을 해석하지 못했습니다. macOS 부팅 성공 여부는 판정하지 않았습니다.";
            String kind = m.group(2), who = kind.equals("guest-initiated") ? "게스트가 스스로 멈춤" : kind.equals("time-limit") ? "시험 시간 제한" : kind.equals("requested-quit") ? "사용자 취소/종료 요청" : kind.equals("signal") ? "신호로 종료" : kind.equals("app-closed") ? "앱이 닫음" : "입력 오류";
            String fl = m.group(3).equals("ok") ? "성공" : m.group(3).equals("FAILED") ? "실패(디스크에 모두 반영됐다고 볼 수 없음)" : "시도하지 않음";
            return "VM 종료 확인: " + (m.group(1).equals("1") ? "예" : "아니오(강제 종료)") + ", 디스크 flush: " + fl + "(종료 확인과 별개), 종료 계기: " + who + ". macOS 부팅 성공 여부는 판정하지 않았습니다(추정 단계: " + stages.summary() + ").";
        }
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
            File cdir, pdir;
            try { cdir = cloneDir.getCanonicalFile(); pdir = priv.getCanonicalFile(); } catch (IOException e) { return new Start(false, "경로를 확인하지 못해 시작하지 않았습니다.", null); }
            String bad = G7Clone.validate(cdir, pdir, ecid);
            if (bad != null) return new Start(false, "이 복제본으로 부팅하지 않았습니다: " + bad, null);
            File log = new File(filesDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + LOG_SUFFIX);
            OutputStream logOut;
            try { logOut = Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE); }
            catch (IOException e) { return new Start(false, "로그 파일을 열지 못해 시작하지 않았습니다.", null); }
            List<String> cmd = new ArrayList<>();
            cmd.add(new File(nativeLibDir, LIB_NAME).getPath());
            cmd.add("--shm"); cmd.add(new File(filesDir, "g7_cloneboot.shm").getPath());
            cmd.add("--rom"); cmd.add(rom.getPath());
            cmd.add("--clone-dir"); cmd.add(cdir.getPath());
            cmd.add("--ecid-dir"); cmd.add(pdir.getPath());   // plain canonical paths: the runner refuses anything else
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
                    if (line.startsWith("ui_runner_vm_clone FRAME ")) {   // the picture is kept in memory for the screen, never written; the log gets a short line without the pixels
                        Frame f = parseFrame(line);
                        run.frameAnsweredMs = System.currentTimeMillis();
                        if (f != null) { run.frameNote = "마지막 응답: 화면 있음(" + f.cls + ")"; run.frame = f; run.put("g7-frame: " + f.w + "x" + f.h + " class=" + f.cls + " nonblack_pct=" + f.nonblackPct + " colors=" + f.colors + " (estimate from pixel statistics only)\n", false); }
                        else { String rest = line.substring("ui_runner_vm_clone FRAME ".length()); run.frameNote = "러너 응답: 아직 보낼 화면이 없습니다(" + (rest.length() > 120 ? rest.substring(0, 120) : rest) + "). 게스트가 화면을 아직 내지 않았거나 프레임이 0개일 수 있습니다."; run.put("g7-frame: " + run.frameNote + "\n", false); }
                        continue;
                    }
                    if (line.startsWith("ui_runner_vm_clone VERDICT:")) run.verdictLine = line;
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
                    try { OutputStream o = p.getOutputStream(); o.write("status\nframe\n".getBytes(StandardCharsets.US_ASCII)); o.flush(); } catch (IOException e) { break; }
                }
            } catch (InterruptedException ignored) { }
        }, "g7-cboot-status");
        poller.setDaemon(true); poller.start();
        new Thread(() -> {
            int code;
            try { code = p.waitFor(); } catch (InterruptedException e) { code = -1; }
            try { reader.join(5000); } catch (InterruptedException ignored) { }
            run.exitCode = code;
            run.put("g7-cboot-exit: " + code + (run.cancelled ? " (cancelled)" : "") + "\n" + run.stages.summary() + "\ng7-cboot-verdict: " + run.verdict() + "\n", true);
            synchronized (run) { try { if (run.log != null) run.log.close(); } catch (IOException ignored) { } run.log = null; }
            run.finished = true;
            poller.interrupt();
        }, "g7-cboot-wait").start();
    }
}
