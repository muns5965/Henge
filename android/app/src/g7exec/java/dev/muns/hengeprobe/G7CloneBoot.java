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
    /** The run times a person may choose (seconds; 0 = no limit, the run then ends by the stop button, a guest end, a fatal engine error or the app closing). Anything else is refused. The stop, quiesce and flush path is the same for all of them. */
    public static final int[] RUN_SECONDS_CHOICES = {900, 1800, 3600, 0};
    static boolean validSeconds(int seconds) { for (int c : RUN_SECONDS_CHOICES) if (c == seconds) return true; return false; }
    static final String LOG_PREFIX = "g7_cboot_", LOG_SUFFIX = ".txt";
    static final int MAX_LOG_BYTES = 2 * 1024 * 1024;
    static final long STATUS_EVERY_MS = 30_000, STOP_WAIT_MS = 15_000;

    interface Spawner { Process spawn(List<String> command) throws IOException; }
    private static final Spawner REAL = c -> {
        ProcessBuilder pb = new ProcessBuilder(c).redirectErrorStream(true);
        lagfxEnv(pb.environment(), c);
        return pb.start();
    };

    static final String LAGFX_LOG_PREFIX = "g7_lagfx_";
    /** Two LAGFX_ variables for the lagfx build of the runner (libapplegfx-vulkan reads them; the normal runner ignores them, and the runner removes only HV_* ones): the log goes to its own file next to the run
     *  log, and at trace level (the guest's GPU commands). Not an HV_ name, so the runner's scrub does not touch it. The folder comes from the --shm path, which is always in the app's files folder. */
    /** Experimental GPU switches chosen on the start dialog (read when the runner is started; default off). gpuExec = LAGFX_ARM64_EXEC=1 (really execute the guest's Render/Blit commands on Vulkan); gpuDumpDir != null =
     *  LAGFX_ARM64_DUMP=1 plus LAGFX_ARM64_DUMP_DIR (the library saves the guest's pipeline/shader data there, size-capped, new files only). */
    static volatile boolean gpuExec = false;
    static volatile File gpuDumpDir = null;

    static void lagfxEnv(java.util.Map<String, String> env, List<String> cmd) {
        int i = cmd.indexOf("--shm");
        if (i < 0 || i + 1 >= cmd.size()) return;
        File dir = new File(cmd.get(i + 1)).getParentFile();
        if (dir == null) return;
        env.put("LAGFX_LOG_LEVEL", "trace");
        StringBuilder note = new StringBuilder();
        if (gpuExec) { env.put("LAGFX_ARM64_EXEC", "1"); note.append("GPU execution ON (LAGFX_ARM64_EXEC=1); "); } else note.append("GPU execution off; ");
        File dd = gpuDumpDir;
        if (dd == null) note.append("GPU data dump off");
        else {
            File runDir = new File(dd, "run-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(new java.util.Date()));   // one folder per run: no mixing, no name clashes
            try {
                java.nio.file.Files.createDirectories(runDir.toPath());
                env.put("LAGFX_ARM64_DUMP", "1");
                env.put("LAGFX_ARM64_DUMP_DIR", runDir.getPath());
                note.append("GPU data dump ON, folder ").append(runDir.getPath()).append(" (limits: the library's own defaults, set by LAGFX_ARM64_DUMP_MAX_FILE / _MAX_TOTAL)");
            } catch (java.io.IOException | RuntimeException e) {
                note.append("GPU data dump REQUESTED BUT NOT POSSIBLE: cannot create ").append(runDir.getPath()).append(" (").append(e.getClass().getSimpleName()).append(")");
            }
        }
        lastGpuNote = note.toString();
    }

    /** What lagfxEnv decided about the GPU switches, written at the top of the run log so a missing dump can be explained. */
    static volatile String lastGpuNote = "";

    /** The newest lagfx log on disk (g7_lagfx_*.txt), or null. */
    public static File newestLagfxLog(File filesDir) {
        File[] l = filesDir.listFiles((d, n) -> n.matches("g7_lagfx_[0-9]{13}\\.txt"));
        if (l == null || l.length == 0) return null;
        Arrays.sort(l);
        return l[l.length - 1];
    }

    public static final class Start {
        public final boolean started; public final String message; public final Run run;
        Start(boolean s, String m, Run r) { started = s; message = m; run = r; }
    }

    static volatile Run CURRENT;
    static boolean hasLive() { Run c = CURRENT; return c != null && !c.finished(); }

    /** The clone folders under vm/clones/ that pass G7Clone.validate() for this ECID (the list the person picks from). Looks only. */
    public static List<File> listValid(File privIn) {
        List<File> out = new ArrayList<>();
        File priv; try { priv = privIn.getCanonicalFile(); } catch (IOException e) { return out; }   // the app's folder is reached through a link on Android: validate() wants plain paths
        long ecid = G7ProbeLauncher.readEcid(priv);
        File[] k = new File(priv, "clones").listFiles();
        if (k == null || ecid <= 0) return out;
        Arrays.sort(k);
        for (File d : k) if (G7Clone.validate(d, priv, ecid) == null) out.add(d);
        return out;
    }

    /** One line per folder under vm/clones/: usable, or why it cannot be booted (so a clone that cannot boot is shown as such, not hidden). */
    public static List<String> describeAll(File privIn) {
        List<String> out = new ArrayList<>();
        File priv; try { priv = privIn.getCanonicalFile(); } catch (IOException e) { out.add("앱 폴더 경로를 확인하지 못했습니다"); return out; }
        long ecid = G7ProbeLauncher.readEcid(priv);
        File[] k = new File(priv, "clones").listFiles();
        if (k == null) return out;
        Arrays.sort(k);
        for (File d : k) { String bad = G7Clone.validate(d, priv, ecid); out.add(d.getName() + ": " + (bad == null ? "부팅 가능(검증됨)" : "부팅 불가 - " + bad)); }
        return out;
    }

    public static Run current() { return CURRENT; }

    /** The newest clone-boot log on disk (g7_cboot_*.txt), or null. Independent of any Run object, so it still works after the app was restarted. */
    public static File newestCloneLog(File filesDir) {
        File[] l = filesDir.listFiles((d, n) -> n.matches("g7_cboot_[0-9]{13}\\.txt"));
        if (l == null || l.length == 0) return null;
        Arrays.sort(l);
        return l[l.length - 1];
    }

    /** The saved clone-boot log text (up to the log limit), "" if none. */
    public static String readSavedCloneLog(File filesDir) {
        File f = newestCloneLog(filesDir);
        return f == null ? "" : G7ExecLauncher.readTail(f, MAX_LOG_BYTES + 8192);
    }

    /** Status text from the newest saved clone-boot log (no Run needed): ended with an exit line, or no exit record (process ended unseen). */
    public static String describeSaved(File filesDir) {
        File f = newestCloneLog(filesDir);
        if (f == null) return "";
        String t = readSavedCloneLog(filesDir);
        int exit = Integer.MIN_VALUE;
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(?m)^g7-cboot-exit: (-?\\d+)").matcher(t);
        if (m.find()) exit = Integer.parseInt(m.group(1));
        String verdict = null;
        m = java.util.regex.Pattern.compile("(?m)^g7-cboot-verdict: (.*)$").matcher(t);
        if (m.find()) verdict = m.group(1);
        return "[저장된 복제본 부팅 로그: " + f.getName() + "]\n" + G7CloneMonitor.replay(t).render(0, false, exit, "저장 로그의 g7-stage 줄 참고", verdict, "저장 로그에는 화면 이미지가 없습니다");
    }

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
        final G7CloneMonitor mon = new G7CloneMonitor();
        public G7CloneMonitor monitor() { return mon; }
        final Process proc; final String logName; final G7BootStage.Tracker stages = new G7BootStage.Tracker();
        private final StringBuilder tail = new StringBuilder();
        private volatile boolean finished; private volatile int exitCode = Integer.MIN_VALUE; private volatile boolean cancelled;
        private final long t0 = System.currentTimeMillis();
        private OutputStream log; private int written;
        Run(Process p, String logName) { this.proc = p; this.logName = logName; }
        public boolean finished() { return finished; }
        public boolean wasCancelled() { return cancelled; }
        /** The parsed VERDICT line, or null (no line, or one that does not parse). */
        public G7RunVerdict parsedVerdict() { return G7RunVerdict.parse(verdictLine); }
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
            G7RunVerdict rv = G7RunVerdict.parse(v);
            String more = "";
            if (rv != null && rv.structured) {
                String oc = rv.isRestart() ? "게스트가 재시작을 요청함(SYSTEM_RESET)" : rv.outcome.equals("shutdown") ? "게스트가 종료를 요청함(SYSTEM_OFF)" : rv.outcome.equals("halt-other") ? "PSCI 기록 없는 정지(정지 주소 등, 원인 미확인)" : rv.outcome.equals("host-stop") ? "호스트가 멈춤" : "종료 미확인";
                more = " 결과: " + oc + ", 가동 " + rv.uptimeS + "초, CPU 스레드·디스크 작업자 정리: " + (rv.quiesced == 1 ? "완료" : rv.quiesced == 0 ? "일부 미종료" : "미보고") + ", 디스크 쓰기 " + rv.diskWrites + "건 중 거부 " + rv.diskWriteRefused + "건.";
            }
            return "VM 종료 확인: " + (m.group(1).equals("1") ? "예" : "아니오(강제 종료)") + ", 디스크 flush: " + fl + "(종료 확인과 별개), 종료 계기: " + who + "." + more + " macOS 부팅 성공 여부는 판정하지 않았습니다(추정 단계: " + stages.summary() + ").";
        }
        private static final java.util.regex.Pattern PID = java.util.regex.Pattern.compile("g7-process: runner_pid=(\\d{1,10})");
        /** The runner's own pid (from its "g7-process: runner_pid=" line), 0 until that line has been seen. */
        volatile int runnerPid;
        /** One line of the app side (G7RunService) into the same log, so one file shows what the app did next to what the runner did. */
        public void note(String text) { put("g7-service: " + text + "\n", true); }
        synchronized void put(String text, boolean always) {
            java.util.regex.Matcher pm = PID.matcher(text);
            if (pm.find()) { try { runnerPid = Integer.parseInt(pm.group(1)); } catch (NumberFormatException ignored) { } }
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

    /** USB test modes of the runner's --usb-mode: 0 = A none (default), 1 = B virtual USB device port only, 2 = C port + non-destructive enumeration, 3 = D = C plus a read-only getenv of five recovery variables, 4 = E = D plus the fixed auto-boot true + saveenv sequence (clone-001 only). installer stays 0 in all of them. */
    public static final int USB_NONE = 0, USB_PORT = 1, USB_PORT_PROBE = 2, USB_PORT_GETENV = 3, USB_AUTOBOOT_FIX = 4;

    public static Start start(File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus) {
        return start(nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, USB_NONE);
    }

    public static Start start(File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode) {
        return start(nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, usbMode, RUN_SECONDS);
    }

    public static Start start(File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode, int seconds) {
        return startWith(REAL, G7ProbeLauncher::usableRomFile, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, usbMode, seconds);
    }

    static Start startWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus) {
        return startWith(sp, roms, enabled, g7Live, nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, USB_NONE);
    }

    static Start startWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode) {
        return startWith(sp, roms, enabled, g7Live, nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, usbMode, RUN_SECONDS);
    }

    static Start startWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode, int seconds) {
        if (!validSeconds(seconds)) return new Start(false, "허용되지 않은 실행 시간 제한이라 시작하지 않았습니다.", null);
        if (usbMode < USB_NONE || usbMode > USB_AUTOBOOT_FIX) return new Start(false, "알 수 없는 USB 모드라 시작하지 않았습니다.", null);
        if (usbMode == USB_AUTOBOOT_FIX && (cloneDir == null || !"clone-001".equals(cloneDir.getName()))) return new Start(false, "E 모드(auto-boot 켜기)는 clone-001에서만 허용됩니다. 시작하지 않았습니다.", null);
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
            cmd.add("--seconds"); cmd.add(String.valueOf(seconds));   // 0 = no limit (the runner then waits for the stop request, a guest end or a fatal error)
            cmd.add("--ram-mb"); cmd.add("2048"); cmd.add("--cpus"); cmd.add("2"); cmd.add("--jit-mb"); cmd.add("64");
            cmd.add("--usb-mode"); cmd.add(String.valueOf(usbMode));
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
                run.put("g7-gpu-options: " + lastGpuNote + "\n", true);
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
                    run.mon.feed(line, System.currentTimeMillis());
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
