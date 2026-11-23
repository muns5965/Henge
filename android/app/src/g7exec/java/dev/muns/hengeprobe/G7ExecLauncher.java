package dev.muns.hengeprobe;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
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
 *   - START_ENABLED is a literal false in the committed code. Turning it on is its own approved change; until then every start() is refused before anything is touched,
 *   - preflight() only looks at files (it never starts the binary, never creates anything),
 *   - even with the switch on, start() needs the exact approval phrase and the IPSW's SHA-256 typed by the person (the runner re-hashes the IPSW against it),
 *   - the ECID is NOT passed on the command line: the runner reads it from vm_ecid.txt itself,
 *   - cancel() asks the runner to stop (SIGTERM -> the adapter cancels the restore); after CANCEL_GRACE_SECONDS the process is killed. An unfinished pair stays in_progress either way.
 * Nothing here deletes, moves or resets an install file. No Android classes: tests/java/G7ExecLauncherTest.java runs on a plain JVM.
 */
public final class G7ExecLauncher {
    private G7ExecLauncher() {}

    /** Committed OFF. The change that turns it on is a separate approval (see docs/g7-device-run.md). */
    static final boolean START_ENABLED = false;

    static final String LIB_NAME = "libg7runner.so";
    static final String APPROVAL_PHRASE = "G7 복원 시작을 승인합니다";
    static final String IPSW_NAME = "UniversalMac_15.6.1_24G90_Restore.ipsw";
    static final long IPSW_BYTES = 16_814_137_790L;
    static final int CANCEL_GRACE_SECONDS = 90;   /* the runner's own grace is 60 s */
    static final int MAX_LOG_BYTES = 256 * 1024;
    static final int TAIL_BYTES = 6000;
    static final String LOG_NAME = "g7_run_log.txt";
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
        s.append("시작 스위치: ").append(START_ENABLED ? "켜짐" : "꺼짐(이 빌드는 시작할 수 없습니다)");
        return new Preflight(ok, s.toString());
    }

    /** The result of start(). */
    public static final class Start {
        public final boolean started;
        public final String message;
        public final Run run;
        Start(boolean started, String message, Run run) { this.started = started; this.message = message; this.run = run; }
    }

    /** A running (or finished) process of the execution build. */
    public static final class Run {
        private final Process proc;
        private final StringBuilder tail = new StringBuilder();
        private volatile int exit = Integer.MIN_VALUE;
        private volatile boolean cancelAsked;
        private final long ecid;
        Run(Process p, long ecid) { this.proc = p; this.ecid = ecid; }
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
    public static Start start(File nativeLibDir, File extVmDir, File privVmDir, File filesDir, String approvalText, String ipswSha256) {
        return startWith(REAL, G7ProbeLauncher::usableRomFile, START_ENABLED, nativeLibDir, extVmDir, privVmDir, filesDir, approvalText, ipswSha256);
    }

    static Start startWith(Spawner spawner, RomFinder roms, boolean enabled, File nativeLibDir, File extVmDir, File privVmDir, File filesDir, String approvalText, String ipswSha256) {
        if (!enabled) return new Start(false, "시작할 수 없습니다: 이 빌드의 시작 스위치가 꺼져 있습니다. 아무것도 만들거나 실행하지 않았습니다.", null);
        if (!isPackaged(nativeLibDir)) return new Start(false, "실행 파일(" + LIB_NAME + ")이 없어 시작하지 않았습니다.", null);
        if (!APPROVAL_PHRASE.equals(approvalText)) return new Start(false, "승인 문구가 일치하지 않아 시작하지 않았습니다.", null);
        if (!isSha256Hex(ipswSha256 == null ? null : ipswSha256.toLowerCase())) return new Start(false, "IPSW SHA-256(64자리 16진수)이 올바르지 않아 시작하지 않았습니다.", null);
        Preflight pf = preflightWith(roms, nativeLibDir, extVmDir, privVmDir);
        if (!pf.ready) return new Start(false, "사전 확인이 통과하지 않아 시작하지 않았습니다.\n" + pf.text, null);
        File rom = roms.find(extVmDir, privVmDir);
        long ecid = G7ProbeLauncher.readEcid(privVmDir);
        if (rom == null || ecid <= 0) return new Start(false, "ROM 또는 ECID를 확인하지 못해 시작하지 않았습니다.", null);
        File work = new File(filesDir, WORK_NAME);
        if (!work.isDirectory() && !work.mkdirs()) return new Start(false, "작업 디렉터리를 만들지 못해 시작하지 않았습니다.", null);
        File log = new File(filesDir, LOG_NAME);
        FileOutputStream logOut;
        try { logOut = new FileOutputStream(log, true); }
        catch (IOException e) { return new Start(false, "로그 파일을 열지 못해 시작하지 않았습니다.", null); }
        Process p;
        try { p = spawner.spawn(buildCommand(nativeLibDir, privVmDir, rom, work, ipswSha256.toLowerCase())); }
        catch (IOException e) { try { logOut.close(); } catch (IOException ignored) { } return new Start(false, "실행 파일을 시작하지 못했습니다: " + mask(String.valueOf(e.getMessage()), ecid), null); }
        final Run run = new Run(p, ecid);
        final InputStream in = p.getInputStream();
        final FileOutputStream lo = logOut;
        Thread t = new Thread(() -> {
            ByteArrayOutputStream line = new ByteArrayOutputStream();
            long written = 0;
            try {
                byte[] b = new byte[2048]; int n;
                while ((n = in.read(b)) > 0) {
                    String s = mask(new String(b, 0, n, StandardCharsets.UTF_8), ecid);
                    run.append(s);
                    if (written < MAX_LOG_BYTES) { byte[] o = s.getBytes(StandardCharsets.UTF_8); int k = (int) Math.min(o.length, MAX_LOG_BYTES - written); lo.write(o, 0, k); written += k; }
                }
            } catch (IOException ignored) { }
            int code;
            try { code = p.waitFor(); } catch (InterruptedException e) { code = -1; Thread.currentThread().interrupt(); }
            String end = "\ng7-exit: " + code + "\n";
            run.append(end);
            try { lo.write(end.getBytes(StandardCharsets.UTF_8)); lo.flush(); lo.close(); } catch (IOException ignored) { }
            run.exit = code;
        });
        t.setDaemon(true);
        t.start();
        return new Start(true, "시작했습니다. 화면을 켜 둔 채 이 앱을 앞에 두세요. 취소는 [취소] 버튼입니다.", run);
    }

    /** The last part of the run log, for a screen that was closed and opened again. Reads only. */
    public static String readLogTail(File filesDir, int maxBytes) {
        File f = new File(filesDir, LOG_NAME);
        if (!f.isFile()) return "";
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            long len = r.length(); int n = (int) Math.min(len, maxBytes);
            byte[] b = new byte[n]; r.seek(len - n); r.readFully(b);
            return new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) { return ""; }
    }
}
