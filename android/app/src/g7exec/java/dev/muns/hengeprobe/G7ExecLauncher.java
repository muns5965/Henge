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
    static final int CANCEL_GRACE_SECONDS = 90;
    static volatile int CANCEL_GRACE_SECONDS_FOR_TEST = 0;   /* 0 = use CANCEL_GRACE_SECONDS; only the plain-JVM test sets it */
    static int graceSeconds() { return CANCEL_GRACE_SECONDS_FOR_TEST > 0 ? CANCEL_GRACE_SECONDS_FOR_TEST : CANCEL_GRACE_SECONDS; }   /* the runner's own grace is 60 s */
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
        boolean pend = G7Isolation.pending(privVmDir);
        s.append("설치 파일: ").append(clean ? "없음(새로 만들 수 있음)" : "이미 있음 — 시작할 수 없습니다(자동 재개·덮어쓰기 없음). 보관하려면 '기존 설치 쌍 격리'를 쓰세요").append('\n'); ok &= clean;
        if (pend) s.append("격리: 끝나지 않았습니다 — 복구(마저 옮기기)가 필요합니다. 그 전에는 시작할 수 없습니다\n");
        ok &= !pend;
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
    static final Object START_LOCK = new Object();

    /** True while a run of this app process is going (what start() refuses on and G7Isolation checks). */
    static boolean hasLiveRun() { Run c = CURRENT; return c != null && !c.finished(); }

    /** After the runner has ended, how long the output pipe may stay open before the log is closed as incomplete (a child that kept the pipe must not keep the screen on "running"). */
    static volatile long drainMillis = 3000;
    /** After SIGKILL: how long to wait for the process to really end before saying so. */
    static volatile long forceConfirmSeconds = 10;

    /** A running (or finished) process of the execution build. "finished" means the process itself has ended (waitFor returned), never merely that its output stopped. */
    public static final class Run {
        private final Process proc;
        private final StringBuilder tail = new StringBuilder();
        private volatile int exit = Integer.MIN_VALUE;
        private volatile boolean cancelAsked;
        private volatile boolean killSent, killUnconfirmed, logIncomplete;
        final Object logLock = new Object();
        private OutputStream logOut;          // guarded by logLock; null once closed
        private long written; private boolean truncated; private String lastResult = "";   // guarded by logLock
        private final long ecid;
        volatile String logName = "";
        Run(Process p, long ecid) { this.proc = p; this.ecid = ecid; }
        public String logName() { return logName; }
        public boolean finished() { return exit != Integer.MIN_VALUE; }
        public int exitCode() { return exit; }
        public boolean killSent() { return killSent; }
        public boolean killUnconfirmed() { return killUnconfirmed; }
        public boolean logIncomplete() { return logIncomplete; }
        public synchronized String tail() { return tail.toString(); }
        synchronized void append(String s) { tail.append(s); if (tail.length() > TAIL_BYTES) tail.replace(0, tail.length() - TAIL_BYTES, ""); }
        /** A line written by this app (not by the runner): into the tail and, while the log is open and not full, into the file. */
        void note(String line) { synchronized (logLock) { try { put(line + "\n"); } catch (IOException ignored) { } } }
        /** Into the tail, and into the log while it is open and below the limit. Callers may be the reader, the watcher and the cancel thread. */
        void put(String text) throws IOException {
            synchronized (logLock) {
                if (text.isEmpty()) return;
                append(text);
                if (logOut == null || truncated) return;
                byte[] o = text.getBytes(StandardCharsets.UTF_8);
                int k = (int) Math.min(o.length, MAX_LOG_BYTES - written);
                logOut.write(o, 0, k); written += k;
                if (k < o.length) truncated = true;
            }
        }
        /** SIGTERM first (the runner cancels the restore and keeps the pair in_progress); SIGKILL after CANCEL_GRACE_SECONDS, then the end is checked and the outcome is said plainly. Safe to call twice. */
        public void cancel() {
            if (cancelAsked || finished()) return;
            cancelAsked = true;
            proc.destroy();
            Thread t = new Thread(() -> {
                boolean ended;
                try { ended = proc.waitFor(graceSeconds(), TimeUnit.SECONDS); } catch (InterruptedException e) { ended = false; }
                if (ended) return;
                killSent = true;
                note("g7-kill: SIGKILL sent after " + graceSeconds() + " s without an end");
                proc.destroyForcibly();
                boolean gone;
                try { gone = proc.waitFor(forceConfirmSeconds, TimeUnit.SECONDS); } catch (InterruptedException e) { gone = false; }
                if (!gone) { killUnconfirmed = true; note("g7-kill-unconfirmed: the process did not end within " + forceConfirmSeconds + " s after SIGKILL (no g7-exit is written for it)"); }
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
        String r = java.util.regex.Pattern.compile(java.util.regex.Pattern.quote(h), java.util.regex.Pattern.CASE_INSENSITIVE).matcher(text.replace("0x" + h, "(ECID)").replace("0X" + h.toUpperCase(), "(ECID)")).replaceAll("(ECID)");
        return r.replace(Long.toString(ecid), "(ECID)");
    }

    /** The one public way to start. The switch is the committed constant. */
    public static Start start(File nativeLibDir, File extVmDir, File privVmDir, File filesDir, boolean consent) {
        return startWith(REAL, G7ProbeLauncher::usableRomFile, START_ENABLED, nativeLibDir, extVmDir, privVmDir, filesDir, consent, IPSW_SHA256, privVmDir::getUsableSpace);
    }

    static Start startWith(Spawner spawner, RomFinder roms, boolean enabled, File nativeLibDir, File extVmDir, File privVmDir, File filesDir, boolean consent, String ipswSha256) {
        return startWith(spawner, roms, enabled, nativeLibDir, extVmDir, privVmDir, filesDir, consent, ipswSha256, () -> Long.MAX_VALUE);
    }

    static Start startWith(Spawner spawner, RomFinder roms, boolean enabled, File nativeLibDir, File extVmDir, File privVmDir, File filesDir, boolean consent, String ipswSha256, java.util.function.LongSupplier freeSpace) {
        if (!enabled) return new Start(false, "시작할 수 없습니다: 이 빌드의 시작 스위치가 꺼져 있습니다. 아무것도 만들거나 실행하지 않았습니다.", null);
        if (!isPackaged(nativeLibDir)) return new Start(false, "실행 파일(" + LIB_NAME + ")이 없어 시작하지 않았습니다.", null);
        if (!consent) return new Start(false, "동의가 체크되지 않아 시작하지 않았습니다.", null);
        if (!isSha256Hex(ipswSha256 == null ? null : ipswSha256.toLowerCase())) return new Start(false, "IPSW SHA-256(64자리 16진수)이 올바르지 않아 시작하지 않았습니다.", null);
        Preflight pf = preflightWith(roms, nativeLibDir, extVmDir, privVmDir);
        if (!pf.ready) return new Start(false, "사전 확인이 통과하지 않아 시작하지 않았습니다.\n" + pf.text, null);
        String lowSpace = G7Storage.enoughFor(freeSpace.getAsLong());   // before anything is created: the disk and AUX are written into this storage
        if (!lowSpace.isEmpty()) return new Start(false, lowSpace, null);
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

    /** Set once by the activity (android.os.Process is not available on a plain JVM): this app process's pid and start time (epoch ms); -1 until then. */
    static volatile int appPid = -1;
    static volatile long appStartMs = -1;
    static void setAppInfo(int pid, long startMs) { appPid = pid; appStartMs = startMs; }

    /** The child's pid when the platform gives it (Process.pid() by reflection: API 33+ on Android, JDK 9+); -1 otherwise. */
    static long pidOf(Process p) {
        try { return ((Number) Process.class.getMethod("pid").invoke(p)).longValue(); } catch (Throwable t) { return -1; }
    }

    private static void streamToLog(final Run run, final Process p, final OutputStream lo, final long ecid) {
        final InputStream in = p.getInputStream();
        synchronized (run.logLock) { run.logOut = lo; }
        // collector: only reads the output pipe into the log. It may never end (a child that kept the pipe), which must not decide whether the run is over.
        final Thread reader = new Thread(() -> {
            final DiagFilter filter = new DiagFilter();
            final ByteArrayOutputStream lb = new ByteArrayOutputStream();
            try {
                final long startMs = System.currentTimeMillis();
                run.put("g7-start: " + startMs + "\n");
                // for matching against ApplicationExitInfo (pid and time) after an app process that ended without a trace; unknown values are -1, never a guess
                run.put("g7-process: app_pid=" + appPid + " runner_pid=" + pidOf(p) + " app_start_ms=" + appStartMs + " run_start_ms=" + startMs + "\n");
                byte[] b = new byte[4096]; int n;
                while ((n = in.read(b)) > 0) {
                    for (int i = 0; i < n; i++) {
                        byte c = b[i];
                        if (c != '\n') lb.write(c);
                        if (c == '\n' || lb.size() >= MAX_LINE_BYTES) { keep(run, filter, lb, ecid); lb.reset(); }
                    }
                }
                if (lb.size() > 0) keep(run, filter, lb, ecid);
                run.put(filter.finish());
            } catch (IOException ignored) { }
        });
        reader.setDaemon(true);
        // watcher: the end of the process is the end of the run; the pipe only gets a short time to deliver the last output
        Thread watcher = new Thread(() -> {
            // the end is recorded only when the process has really ended: an interrupt is not an end, so wait again (the interrupt is passed on afterwards)
            int code = 0; boolean interrupted = false;
            while (true) {
                try { code = p.waitFor(); break; }
                catch (InterruptedException e) { interrupted = true; if (!p.isAlive()) { try { code = p.exitValue(); } catch (IllegalThreadStateException ex) { continue; } break; } }
            }
            if (interrupted) Thread.currentThread().interrupt();
            try { reader.join(drainMillis); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            final boolean complete = !reader.isAlive();
            synchronized (run.logLock) {
                run.logIncomplete = !complete;
                try {
                    if (run.logOut != null) {
                        if (run.truncated) {
                            run.logOut.write(("\n--- log limit reached: the output after this point is not kept here; the last result line and the tail follow ---\n"
                                    + (run.lastResult.isEmpty() ? "" : run.lastResult + "\n") + "--- tail ---\n" + run.tail()).getBytes(StandardCharsets.UTF_8));
                        }
                        String inc = complete ? "" : "\ng7-log: incomplete (the output pipe was still open " + drainMillis + " ms after the process ended; later output is not kept)";
                        String end = inc + "\ng7-exit: " + code + "\n";
                        run.logOut.write(end.getBytes(StandardCharsets.UTF_8)); run.logOut.flush(); run.logOut.close();
                    }
                } catch (IOException ignored) { }
                run.logOut = null;
                run.append((complete ? "" : "\ng7-log: incomplete") + "\ng7-exit: " + code + "\n");
            }
            run.exit = code;   // the confirmed end is published first; cleaning up the pipe afterwards may block and must not delay it
            if (!complete) {
                Thread closer = new Thread(() -> { try { in.close(); } catch (IOException | RuntimeException ignored) { } });
                closer.setDaemon(true);
                closer.start();
            }
        });
        watcher.setDaemon(true);
        reader.start();
        watcher.start();
    }

    /** One complete line: masked, filtered, shown in the tail and (until the limit) written to the log. */
    private static void keep(Run run, DiagFilter filter, ByteArrayOutputStream lb, long ecid) throws IOException {
        String line = mask(new String(lb.toByteArray(), StandardCharsets.UTF_8), ecid);
        if (line.startsWith("g7-result:")) { synchronized (run.logLock) { run.lastResult = line.length() > 1024 ? line.substring(0, 1024) : line; } }   // remembered even when the log is full
        run.put(filter.accept(line));
    }

    /** The only files the log provider may hand out: run logs of this app, by plain name. Returns null for anything else. */
    static File logFile(File filesDir, String name) {
        if (name == null || !(name.matches("g7_run_[0-9]{13}\\.txt") || name.matches("g7_net_[0-9]{13}\\.txt"))) return null;   // run logs and network-probe logs
        File f = new File(filesDir, name);
        return f.isFile() ? f : null;
    }

    /** For the top of the screen: what the newest log (or the live run) says, in a few lines. Reads only; never claims more than the log shows. */
    public static String summary(File filesDir) { return summary(filesDir, null); }

    /** The install pair's own state file (g7_state.txt in the vm dir), read only. Only the value of "state=" is shown, and only when it is one the runner writes; nothing else of the file reaches the screen
     *  (so no other field, now or later, can leak). "" when no vm dir is given. */
    static String pairState(File privVmDir) {
        if (privVmDir == null) return "";
        String iso = (G7Isolation.pending(privVmDir) ? "\n격리: 끝나지 않음(복구 필요)" : "") + (G7Isolation.archiveCount(privVmDir) > 0 ? "\n보관 폴더: " + G7Isolation.archiveCount(privVmDir) + "개" : "");
        File st = new File(privVmDir, "g7_state.txt");
        if (!st.isFile()) return "\n설치 쌍: 상태 파일(g7_state.txt) 없음" + iso;
        String first = readTail(st, 200).split("\n", 2)[0].trim();
        String v = first.startsWith("state=") ? first.substring(6) : "";
        return "\n설치 쌍: " + (v.equals("in_progress") || v.equals("done") ? v : "상태 확인 불가") + iso;
    }

    /** Looks (read only) for a process of this app's own user whose program is the runner. Three answers, never an "it has ended" claim: found (pid), not found (not proof that it ended), or cannot tell
     *  (the process list is not readable, or this app's own process is not in it, so the list is not trustworthy). Never signals, never touches a file. */
    public static String runnerCheck(File procRoot, int selfPid, File ownerRef) {
        int[] pid = new int[1];
        int st = runnerStatus(procRoot, selfPid, ownerRef, pid);
        if (st < 0) return procRoot == null || procRoot.listFiles() == null ? "실행기 프로세스: 확인 불가(/proc을 읽을 수 없음)" : "실행기 프로세스: 확인 불가(조회가 제한되어 결과를 믿을 수 없음)";
        return st == 1 ? "실행기 프로세스: 발견(pid " + pid[0] + ", " + describeState(procState(procRoot, pid[0])) + ")" : "실행기 프로세스: 미발견(종료의 증명은 아님)";
    }

    /** The one-letter state from /proc/&lt;pid&gt;/status ("State:" line), read only; '?' when it cannot be read or is not a letter. Only this letter ever leaves the file. */
    static char procState(File procRoot, int pid) {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(new java.io.FileInputStream(new File(new File(procRoot, Integer.toString(pid)), "status")), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) {
                if (line.startsWith("State:")) { String v = line.substring(6).trim(); return v.length() > 0 && "RSDZTtXIWK".indexOf(v.charAt(0)) >= 0 ? v.charAt(0) : '?'; }
            }
        } catch (IOException | RuntimeException e) { /* unreadable: unknown */ }
        return '?';
    }

    static String describeState(char c) {
        switch (c) {
            case 'R': return "상태 R: 실행 중";
            case 'S': return "상태 S: 대기(신호로 종료 가능)";
            case 'D': return "상태 D: 커널 대기(신호로 끝낼 수 없음)";
            case 'Z': return "상태 Z: 종료됐으나 아직 회수되지 않음";
            case 'T': case 't': return "상태 " + c + ": 정지됨";
            case '?': return "상태 확인 불가";
            default: return "상태 " + c;
        }
    }

    /** 1 = a runner process was found (pid in out[0]), 0 = the list is trustworthy and has none, -1 = cannot tell. */
    static int runnerStatus(File procRoot, int selfPid, File ownerRef, int[] out) {
        File[] dirs = procRoot == null ? null : procRoot.listFiles();
        if (dirs == null) return -1;
        String owner = ownerOf(ownerRef);
        boolean sawSelf = false;
        int found = -1;
        for (File d : dirs) {
            String n = d.getName();
            if (!n.matches("[0-9]{1,9}") || !d.isDirectory()) continue;
            int pid = Integer.parseInt(n);
            if (pid == selfPid) sawSelf = true;
            if (pid == selfPid || found >= 0) continue;
            boolean match = programEndsWithRunner(new File(d, "cmdline")) || exeEndsWithRunner(new File(d, "exe"));
            if (match && owner != null && owner.equals(ownerOf(d))) found = pid;
        }
        if (!sawSelf || owner == null) return -1;
        if (found >= 0) { if (out != null) out[0] = found; return 1; }
        return 0;
    }

    public static int runnerStatus(int selfPid, File filesDir) { return runnerStatus(new File("/proc"), selfPid, filesDir, null); }

    public static String runnerCheck(int selfPid, File filesDir) { return runnerCheck(new File("/proc"), selfPid, filesDir); }

    private static String ownerOf(File f) {
        try { return Files.getOwner(f.toPath()).getName(); } catch (IOException | RuntimeException e) { return null; }
    }

    private static boolean programEndsWithRunner(File cmdline) {
        try (java.io.FileInputStream in = new java.io.FileInputStream(cmdline)) {
            byte[] b = new byte[4096]; int n = in.read(b);
            if (n <= 0) return false;
            int e = 0; while (e < n && b[e] != 0) e++;
            return isRunnerPath(new String(b, 0, e, StandardCharsets.UTF_8));
        } catch (IOException e) { return false; }
    }

    private static boolean exeEndsWithRunner(File exe) {
        try { return isRunnerPath(Files.readSymbolicLink(exe.toPath()).toString()); } catch (IOException | RuntimeException e) { return false; }
    }

    static boolean isRunnerPath(String p) { return p.equals(LIB_NAME) || p.endsWith("/" + LIB_NAME); }

    public static String summary(File filesDir, File privVmDir) {
        String ps = pairState(privVmDir);
        Run r = CURRENT;
        File f = newestLog(filesDir);
        StringBuilder s = new StringBuilder();
        if (r != null && !r.finished()) {
            s.append("상태: 실행 중");
            if (r.killUnconfirmed) s.append(" (강제 종료(SIGKILL)를 보냈으나 종료가 확인되지 않음 — 앱을 강제 종료하고, 그래도 남으면 휴대폰을 재시작하세요)");
            else if (r.killSent) s.append(" (취소 후 강제 종료(SIGKILL)를 보냄 — 종료 확인 중)");
            else if (r.cancelAsked) s.append(" (취소 요청됨 — 아직 끝나지 않음)");
        } else if (r != null) {
            s.append("상태: 종료 — ").append(meaning(r.exitCode()));
            if (r.killSent) s.append("\n강제 종료(SIGKILL)로 끝났습니다. 정상 취소가 아니며 g7-result가 없을 수 있습니다.");
            if (r.logIncomplete) s.append("\n로그 수집 미완료: 프로세스가 끝난 뒤에도 출력 파이프가 닫히지 않았습니다.");
        }
        else if (f == null) return "상태: 실행 기록 없음" + ps;
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
            int g = t.lastIndexOf("G7RUN RESULT:");
            if (g >= 0) { int e = t.indexOf('\n', g); s.append("\n").append(t, g, e < 0 ? t.length() : e); }
            int x = t.lastIndexOf("g7-exit:");
            if (x >= 0) { int e = t.indexOf('\n', x); s.append("\n").append(t, x, e < 0 ? t.length() : e); }
        }
        return s.append(ps).toString();
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
