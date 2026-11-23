package dev.muns.hengeprobe;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * THE only place that can create the process of the G7 EXECUTION build (libg7runner.so = host/g7_runner.c built with -DHV_G7_REAL_EXECUTION). This class lives in the
 * "g7exec" source set (android/app/src/g7exec), which only the g7exec build type compiles: the general APK (assembleDebug) contains neither this class, nor the activity,
 * nor the binary (tools/check-g7exec-apk.sh checks both APKs).
 *
 * What starting it does, if it is ever enabled: creates the 128 GiB sparse install disk and the AUX in the app's private vm directory, starts the machine with WRITABLE storage,
 * and runs the restore against Apple's signing servers (gs.apple.com ...). That is why:
 *   - START_ENABLED comes from the build type (false in g7exec, true only in g7start); while it is false every start() is refused before anything is touched,
 *   - preflight() only looks at files (it never starts the binary, never creates anything),
 *   - even with the switch on, start() needs the person's consent (a checked box next to the list of what will happen); nothing is typed. The IPSW SHA-256 is the built-in reference IPSW_SHA256: the runner re-hashes
 *     the file and refuses to create anything on a mismatch,
 *   - the ECID is NOT passed on the command line: the runner reads it from vm_ecid.txt itself,
 *   - cancel() asks the runner to stop (SIGTERM -> the adapter cancels the restore); after CANCEL_GRACE_SECONDS the process is killed. An unfinished pair stays in_progress either way.
 * Nothing here deletes, moves or resets an install file. No Android classes: tests/java/G7ExecLauncherTest.java runs on a plain JVM.
 */
public final class G7ExecLauncher {
    private G7ExecLauncher() {}

    /** From the build type: false in g7exec (this APK cannot start the runner), true only in g7start (android/app/build.gradle). Turning on a published build is a separate approval (docs/g7-device-run.md). */
    static final boolean START_ENABLED = BuildConfig.G7_START_ENABLED;
    static final String BUILD_KIND = BuildConfig.G7_BUILD_KIND;

    static final String LIB_NAME = "libg7runner.so";
    /** The reference SHA-256 of UniversalMac_15.6.1_24G90_Restore.ipsw (macOS 15.6.1, 24G90). Taken from public firmware listings (ipsw.me, getIPSW, ipswdl agree), not from Apple. It is never trusted blindly:
     *  the runner hashes the stored file itself and refuses on a mismatch, printing both values. */
    static final String IPSW_SHA256 = "3d87686b691ac765eb6a6b3082b2334e2af9710096a00432dd519af89ff2ea78";
    static final String IPSW_NAME = "UniversalMac_15.6.1_24G90_Restore.ipsw";
    static final long IPSW_BYTES = 16_814_137_790L;
    static final int CANCEL_GRACE_SECONDS = 90;   /* the runner's own grace is 60 s */
    /** One log file PER RUN (never appended to, never overwritten: CREATE_NEW), at most MAX_LOG_BYTES of (filtered, see DiagFilter) output plus a closing block of a few KiB. Runs are only possible while no
     *  install file exists, so the total stays small. When the limit is reached the streamed output stops, but the last g7-result line, the tail and the exit code are still written. */
    static final int MAX_LOG_BYTES = 4 * 1024 * 1024;
    static final int TAIL_BYTES = 6000;
    static final String LOG_PREFIX = "g7_run_";
    static final String LOG_SUFFIX = ".txt";
    static final String WORK_NAME = "g7work";

    interface Spawner { Process spawn(List<String> command) throws IOException; }
    private static final Spawner REAL = command -> new ProcessBuilder(command).redirectErrorStream(true).start();

    public static boolean isPackaged(File nativeLibDir) { return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile(); }

    /** What can be said without starting anything. */
    public static final class Preflight {
        public final boolean ready;
        public final String text;
        Preflight(boolean ready, String text) { this.ready = ready; this.text = text; }
    }

    /** Looks at files only. The binary is never executed here; nothing is created. */
    public static Preflight preflight(File nativeLibDir, File extVmDir, File privVmDir) { return preflightWith(G7ProbeLauncher::usableRomFile, nativeLibDir, extVmDir, privVmDir); }

    interface RomFinder { File find(File extVmDir, File privVmDir); }

    static Preflight preflightWith(RomFinder roms, File nativeLibDir, File extVmDir, File privVmDir) {
        StringBuilder s = new StringBuilder();
        boolean ok = true;
        boolean pk = isPackaged(nativeLibDir);
        s.append("실행 파일 ").append(LIB_NAME).append(": ").append(pk ? "포함됨(실행하지 않음)" : "없음 (MISSING)").append('\n'); ok &= pk;
        File rom = roms.find(extVmDir, privVmDir);
        s.append("ROM: ").append(rom != null ? "정상" : "정상 ROM 없음").append('\n'); ok &= rom != null;
        boolean ec = G7ProbeLauncher.readEcid(privVmDir) > 0;
        s.append("ECID(vm_ecid.txt): ").append(ec ? "읽을 수 있음(표시하지 않음)" : "없거나 유효하지 않음 — 이 화면은 만들지 않습니다").append('\n'); ok &= ec;
        File ipsw = new File(privVmDir, IPSW_NAME);
        boolean ip = ipsw.isFile() && ipsw.length() == IPSW_BYTES;
        s.append("IPSW: ").append(ip ? "있음, 크기 일치(SHA-256은 실행기가 시작 전에 확인)" : "없거나 크기가 다름").append('\n'); ok &= ip;
        File disk = new File(privVmDir, "macos_disk.img"), aux = new File(privVmDir, "macos_aux.img"), st = new File(privVmDir, "g7_state.txt");
        boolean clean = !disk.exists() && !aux.exists() && !st.exists();
        s.append("설치 파일: ").append(clean ? "없음(새로 만들 수 있음)" : "이미 있음 — 시작할 수 없습니다(자동 재개·덮어쓰기 없음)").append('\n'); ok &= clean;
        s.append("시작 스위치: ").append(START_ENABLED ? "켜짐(시험 빌드: 동의 상자를 체크하면 실제로 시작합니다)" : "꺼짐(이 빌드는 시작할 수 없습니다)");
        return new Preflight(ok, s.toString());
    }

    /** The result of start(). */
    public static final class Start {
        public final boolean started;
        public final String message;
        public final Run run;
        Start(boolean started, String message, Run run) { this.started = started; this.message = message; this.run = run; }
    }

    /** The run of this app process, independent of any Activity: a recreated screen (rotation, back and forth) finds it here and can read it and cancel it. If Android ends the app process the
     *  run ends with it (it is a child of the app); the log then has "g7-start" and no "g7-exit". */
    private static volatile Run CURRENT;
    public static Run current() { return CURRENT; }
    private static final Object START_LOCK = new Object();

    /** A running (or finished) process of the execution build. */
    public static final class Run {
        private final Process proc;
        private final StringBuilder tail = new StringBuilder();
        private volatile int exit = Integer.MIN_VALUE;
        private volatile boolean cancelAsked;
        private final long ecid;
        volatile String logName = "";
        Run(Process p, long ecid) { this.proc = p; this.ecid = ecid; }
        public String logName() { return logName; }
        public boolean finished() { return exit != Integer.MIN_VALUE; }
        public int exitCode() { return exit; }
        public synchronized String tail() { return tail.toString(); }
        synchronized void append(String s) { tail.append(s); if (tail.length() > TAIL_BYTES) tail.replace(0, tail.length() - TAIL_BYTES, ""); }
        /** SIGTERM first (the runner cancels the restore and keeps the pair in_progress); killed after CANCEL_GRACE_SECONDS. Safe to call twice. */
        public void cancel() {
            if (cancelAsked || finished()) return;
            cancelAsked = true;
            proc.destroy();
            Thread t = new Thread(() -> {
                try { if (!proc.waitFor(CANCEL_GRACE_SECONDS, TimeUnit.SECONDS)) proc.destroyForcibly(); } catch (InterruptedException e) { proc.destroyForcibly(); }
            });
            t.setDaemon(true);
            t.start();
        }
    }

    /** The command: only the options the runner needs. The ECID is not an option: the runner reads vm_ecid.txt itself, so the value never travels on a command line. */
    static List<String> buildCommand(File nativeLibDir, File privVmDir, File rom, File workdir, String ipswSha256) {
        List<String> c = new ArrayList<>();
        c.add(new File(nativeLibDir, LIB_NAME).getPath());
        c.add("--dir"); c.add(privVmDir.getPath());
        c.add("--ipsw-dir"); c.add(privVmDir.getPath());
        c.add("--ecid-dir"); c.add(privVmDir.getPath());
        c.add("--rom"); c.add(rom.getPath());
        c.add("--workdir"); c.add(workdir.getPath());
        c.add("--ipsw-sha256"); c.add(ipswSha256);
        return c;
    }

    static boolean isSha256Hex(String s) {
        if (s == null || s.length() != 64) return false;
        for (int i = 0; i < 64; i++) if (Character.digit(s.charAt(i), 16) < 0 || s.charAt(i) > 'f') return false;
        return true;
    }

    static String meaning(int exit) {
        switch (exit) {
            case 0: return "종료 코드 0: 설치 쌍이 완료로 판정되었습니다(근거일 뿐 부팅 증명은 아닙니다).";
            case 2: return "종료 코드 2: 거부되었거나 실패했습니다. 시작된 쌍은 in_progress로 보존됩니다.";
            case 3: return "종료 코드 3: 실행 스위치에서 멈췄습니다(이 빌드에서는 나오지 않아야 함).";
            case 5: return "종료 코드 5: 복원이 끝나지 않아 프로세스를 종료했습니다. 쌍은 in_progress로 보존됩니다.";
            case 64: return "종료 코드 64: 인자 오류.";
            case 137: case 143: return "종료 코드 " + exit + ": 신호로 끝났습니다(취소 또는 시스템 종료). 쌍은 in_progress로 보존됩니다.";
            default: return "예상하지 못한 종료 코드 " + exit + ". 쌍은 in_progress로 보존됩니다.";
        }
    }

    static String mask(String text, long ecid) {
        if (text == null || ecid <= 0) return text;
        String h = Long.toHexString(ecid);
        return text.replace("0x" + h, "(ECID)").replace(h, "(ECID)").replace(h.toUpperCase(), "(ECID)");
    }

    /** The one public way to start. The switch is the committed constant. */
    public static Start start(File nativeLibDir, File extVmDir, File privVmDir, File filesDir, boolean consent) {
        return startWith(REAL, G7ProbeLauncher::usableRomFile, START_ENABLED, nativeLibDir, extVmDir, privVmDir, filesDir, consent, IPSW_SHA256);
    }

    static Start startWith(Spawner spawner, RomFinder roms, boolean enabled, File nativeLibDir, File extVmDir, File privVmDir, File filesDir, boolean consent, String ipswSha256) {
        if (!enabled) return new Start(false, "시작할 수 없습니다: 이 빌드의 시작 스위치가 꺼져 있습니다. 아무것도 만들거나 실행하지 않았습니다.", null);
        if (!isPackaged(nativeLibDir)) return new Start(false, "실행 파일(" + LIB_NAME + ")이 없어 시작하지 않았습니다.", null);
        if (!consent) return new Start(false, "동의가 체크되지 않아 시작하지 않았습니다.", null);
        if (!isSha256Hex(ipswSha256 == null ? null : ipswSha256.toLowerCase())) return new Start(false, "IPSW SHA-256(64자리 16진수)이 올바르지 않아 시작하지 않았습니다.", null);
        Preflight pf = preflightWith(roms, nativeLibDir, extVmDir, privVmDir);
        if (!pf.ready) return new Start(false, "사전 확인이 통과하지 않아 시작하지 않았습니다.\n" + pf.text, null);
        File rom = roms.find(extVmDir, privVmDir);
        long ecid = G7ProbeLauncher.readEcid(privVmDir);
        if (rom == null || ecid <= 0) return new Start(false, "ROM 또는 ECID를 확인하지 못해 시작하지 않았습니다.", null);
        File work = new File(filesDir, WORK_NAME);
        if (!work.isDirectory() && !work.mkdirs()) return new Start(false, "작업 디렉터리를 만들지 못해 시작하지 않았습니다.", null);
        synchronized (START_LOCK) {
            Run cur = CURRENT;
            if (cur != null && !cur.finished()) return new Start(false, "이미 실행 중인 G7 프로세스가 있어 시작하지 않았습니다.", null);
            File log = new File(filesDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + LOG_SUFFIX);
            OutputStream logOut;
            try { logOut = Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE); }   // never overwrites an earlier log
            catch (IOException e) { return new Start(false, "로그 파일을 열지 못해 시작하지 않았습니다.", null); }
            Process p;
            try { p = spawner.spawn(buildCommand(nativeLibDir, privVmDir, rom, work, ipswSha256.toLowerCase())); }
            catch (IOException e) { try { logOut.close(); } catch (IOException ignored) { } return new Start(false, "실행 파일을 시작하지 못했습니다: " + mask(String.valueOf(e.getMessage()), ecid), null); }
            final Run run = new Run(p, ecid);
            run.logName = log.getName();
            CURRENT = run;
            streamToLog(run, p, logOut, ecid);
            return new Start(true, "시작했습니다. 화면을 켜 둔 채 이 앱을 앞에 두세요. 취소는 [취소] 버튼입니다.", run);
        }
    }

    /** Keeps the log readable: the virtual USB device prints about a dozen "bdif usb diag:" lines per 2 KiB chunk, which would fill the log (and the 6 KB screen tail) within seconds. The first
     *  KEEP_FIRST such lines are kept, later ones are counted and replaced by a short summary (with the last suppressed line) every SUMMARY_EVERY lines and once at the end. Every other line is kept. */
    static final class DiagFilter {
        static final String PREFIX = "bdif usb diag:";
        static final int KEEP_FIRST = 300;
        static final int SUMMARY_EVERY = 5000;
        private int kept;
        private long suppressed;
        private String last = "";
        private long summarised;
        /** The text to keep for one complete line (without its line end); "" when the line is suppressed. */
        String accept(String line) {
            if (!line.startsWith(PREFIX)) return line + "\n";
            if (kept < KEEP_FIRST) { kept++; return line + "\n"; }
            suppressed++;
            last = line.length() > 200 ? line.substring(0, 200) : line;
            if (suppressed == 1 || suppressed % SUMMARY_EVERY == 0) { summarised = suppressed; return summary(); }
            return "";
        }
        /** The closing summary (only when something was suppressed since the last one). */
        String finish() { return suppressed > summarised ? summary() : ""; }
        private String summary() { return "[usb diag: " + suppressed + " line(s) not kept; last: " + last + "]\n"; }
    }

    static final int MAX_LINE_BYTES = 8192;

    private static void streamToLog(final Run run, final Process p, final OutputStream lo, final long ecid) {
        final InputStream in = p.getInputStream();
        Thread t = new Thread(() -> {
            final DiagFilter filter = new DiagFilter();
            final long[] written = {0};
            final boolean[] truncated = {false};
            final String[] lastResult = {""};
            final ByteArrayOutputStream lb = new ByteArrayOutputStream();
            try {
                lo.write(("g7-start: " + System.currentTimeMillis() + "\n").getBytes(StandardCharsets.UTF_8));
                byte[] b = new byte[4096]; int n;
                while ((n = in.read(b)) > 0) {
                    for (int i = 0; i < n; i++) {
                        byte c = b[i];
                        if (c != '\n') lb.write(c);
                        if (c == '\n' || lb.size() >= MAX_LINE_BYTES) { keep(run, lo, filter, lb, ecid, written, truncated, lastResult); lb.reset(); }
                    }
                }
                if (lb.size() > 0) keep(run, lo, filter, lb, ecid, written, truncated, lastResult);
                put(run, lo, filter.finish(), written, truncated);
            } catch (IOException ignored) { }
            int code;
            try { code = p.waitFor(); } catch (InterruptedException e) { code = -1; Thread.currentThread().interrupt(); }
            try {
                if (truncated[0]) {
                    lo.write(("\n--- log limit reached: the output after this point is not kept here; the last result line and the tail follow ---\n"
                            + (lastResult[0].isEmpty() ? "" : lastResult[0] + "\n") + "--- tail ---\n" + run.tail()).getBytes(StandardCharsets.UTF_8));
                }
                String end = "\ng7-exit: " + code + "\n";
                lo.write(end.getBytes(StandardCharsets.UTF_8)); lo.flush(); lo.close();
            } catch (IOException ignored) { }
            run.append("\ng7-exit: " + code + "\n");
            run.exit = code;
        });
        t.setDaemon(true);
        t.start();
    }

    /** One complete line: masked, filtered, shown in the tail and (until the limit) written to the log. */
    private static void keep(Run run, OutputStream lo, DiagFilter filter, ByteArrayOutputStream lb, long ecid, long[] written, boolean[] truncated, String[] lastResult) throws IOException {
        String line = mask(new String(lb.toByteArray(), StandardCharsets.UTF_8), ecid);
        if (line.startsWith("g7-result:")) lastResult[0] = line.length() > 1024 ? line.substring(0, 1024) : line;   // remembered even when the log is full
        put(run, lo, filter.accept(line), written, truncated);
    }

    private static void put(Run run, OutputStream lo, String text, long[] written, boolean[] truncated) throws IOException {
        if (text.isEmpty()) return;
        run.append(text);
        if (truncated[0]) return;
        byte[] o = text.getBytes(StandardCharsets.UTF_8);
        int k = (int) Math.min(o.length, MAX_LOG_BYTES - written[0]);
        lo.write(o, 0, k); written[0] += k;
        if (k < o.length) truncated[0] = true;
    }

    /** The only files the log provider may hand out: run logs of this app, by plain name. Returns null for anything else. */
    static File logFile(File filesDir, String name) {
        if (name == null || !name.matches("g7_run_[0-9]{13}\\.txt")) return null;
        File f = new File(filesDir, name);
        return f.isFile() ? f : null;
    }

    /** For the top of the screen: what the newest log (or the live run) says, in a few lines. Reads only; never claims more than the log shows. */
    public static String summary(File filesDir) {
        Run r = CURRENT;
        File f = newestLog(filesDir);
        StringBuilder s = new StringBuilder();
        if (r != null && !r.finished()) s.append("상태: 실행 중").append(r.cancelAsked ? " (취소 요청됨 — 아직 끝나지 않음)" : "");
        else if (r != null) s.append("상태: 종료 — ").append(meaning(r.exitCode()));
        else if (f == null) return "상태: 실행 기록 없음";
        else {
            String t = readTail(f, 400000);
            if (t.contains("g7-exit:")) s.append("상태: 종료됨(이전 실행) — 종료 줄은 로그에 있음");
            else s.append("상태: 이전 실행의 로그에 종료 기록(g7-exit)이 없음 — 앱이 끝났을 수 있으며 결과는 확인되지 않음");
        }
        if (f != null) {
            s.append("\n로그: ").append(f.getName()).append(" (").append(f.length()).append(" 바이트)");
            String t = readTail(f, 400000);
            int i = t.lastIndexOf("g7-result:");
            if (i >= 0) { int e = t.indexOf('\n', i); s.append("\n").append(t, i, e < 0 ? t.length() : e); }
            else s.append("\ng7-result: 로그에 아직 없음");
            int x = t.lastIndexOf("g7-exit:");
            if (x >= 0) { int e = t.indexOf('\n', x); s.append("\n").append(t, x, e < 0 ? t.length() : e); }
        }
        return s.toString();
    }

    /** The newest run log (by name), or null. */
    static File newestLog(File filesDir) {
        File[] l = filesDir.listFiles((d, n) -> n.startsWith(LOG_PREFIX) && n.endsWith(LOG_SUFFIX));
        if (l == null || l.length == 0) return null;
        java.util.Arrays.sort(l);
        return l[l.length - 1];
    }

    public static File newestLogFile(File filesDir) { return newestLog(filesDir); }

    /** What the newest log says about the last run when no run of this process is known: "" (no log), running-or-interrupted (g7-start without g7-exit), or the exit line. Reads only. */
    public static String lastRunNote(File filesDir) {
        Run r = CURRENT;
        if (r != null && !r.finished()) return "이 앱 프로세스에서 실행 중입니다.";
        File f = newestLog(filesDir);
        if (f == null) return "";
        String t = readTail(f, 400000);
        if (t.contains("g7-exit:")) return "마지막 실행이 끝났습니다(" + f.getName() + ").";
        if (t.contains("g7-start:")) return "마지막 실행 로그(" + f.getName() + ")에 종료 기록이 없어 정상 종료 여부를 확인할 수 없습니다(앱 프로세스가 끝났을 수 있음). 설치 쌍(g7_state.txt)을 다시 검사하세요.";
        return "";
    }

    static String readTail(File f, int maxBytes) {
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            long len = r.length(); int n = (int) Math.min(len, maxBytes);
            byte[] b = new byte[n]; r.seek(len - n); r.readFully(b);
            return new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) { return ""; }
    }

    /** The last part of the newest run log, for a screen that was closed and opened again. Reads only. */
    public static String readLogTail(File filesDir, int maxBytes) {
        File f = newestLog(filesDir);
        return f == null ? "" : readTail(f, maxBytes);
    }
}
