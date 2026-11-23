package dev.muns.hengeprobe;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * The ONLY place in the app that creates the process of g7_probe (libg7probe.so = host/g7_probe.c): the G7 preflight as a program of its own. That program
 * links no virtual machine, no restore tool, no network and no disk I/O, only reads the files it is pointed at and exits 3 (valid request, stopped by the
 * execution switch), 2 (refused) or 64 (bad arguments); it never exits 0 and never starts a restore. Because of that there is no start switch here:
 * nothing this class can start does anything but look at files. tools/check-g7-probe-launcher-source.sh keeps it that way (one ProcessBuilder, only this
 * library, no other runner named, no file writes).
 *
 * The ROM follows MainActivity.findLocalRom()'s rule (external vm/rom.bin if genuine, else the app's internal copy if genuine) through VmRomCheck; nothing is
 * downloaded. The ECID is READ from vm_ecid.txt (never created) and handed over on the command line like the installed-disk runner's; it is never shown or
 * logged: mask() removes it from anything the process printed before the text reaches a screen.
 * No Android classes: tests/java/G7ProbeLauncherTest.java runs on a plain JVM.
 */
public final class G7ProbeLauncher {
    private G7ProbeLauncher() {}

    static final String LIB_NAME = "libg7probe.so";
    /** The probe only stats files and hashes a 215 KB ROM; this is a bound for a hung process, not an expected duration. */
    static final int TIMEOUT_SECONDS = 30;
    static final int MAX_OUTPUT_BYTES = 16384;

    public static final class Result {
        public final boolean ran;
        public final int exit;
        public final String output;
        public final String meaning;
        Result(boolean ran, int exit, String output, String meaning) { this.ran = ran; this.exit = exit; this.output = output; this.meaning = meaning; }
    }

    interface Spawner { Process spawn(List<String> command) throws IOException; }

    private static final Spawner REAL = command -> new ProcessBuilder(command).redirectErrorStream(true).start();

    public static boolean isPackaged(File nativeLibDir) { return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile(); }

    /** The ROM the app would use: external vm/rom.bin if genuine, else the internal saved copy if genuine, else null. Reads only. */
    static File usableRomFile(File extVmDir, File privVmDir) { return usableRomFile(extVmDir, privVmDir, VmRomCheck.LENGTH, VmRomCheck.SHA256); }

    static File usableRomFile(File extVmDir, File privVmDir, int length, String sha256) {
        File ext = new File(extVmDir, "rom.bin"), internal = new File(privVmDir, "AVPBooter.vmapple2.bin");
        if (VmRomCheck.isGenuine(ext, length, sha256)) return ext;
        if (VmRomCheck.isGenuine(internal, length, sha256)) return internal;
        return null;
    }

    /** vm_ecid.txt read as VmEcid writes it, without creating anything. -1 when missing or not a valid in-range value. */
    static long readEcid(File privVmDir) {
        File f = new File(privVmDir, VmEcid.FILE);
        if (!f.isFile()) return -1;
        try {
            String t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
            long v = t.startsWith("ecid=") ? VmRunnerInstalledLauncher.parseHex(t.substring(5)) : -1;
            return v >= 0 && VmEcid.inRange(v) ? v : -1;
        } catch (IOException e) { return -1; }
    }

    /** Exactly the options host/g7_probe.c requires; nothing else is ever added. */
    static List<String> buildCommand(File nativeLibDir, File dir, File ipswDir, File ecidDir, File rom, long ecid) {
        List<String> c = new ArrayList<>();
        c.add(new File(nativeLibDir, LIB_NAME).getPath());
        c.add("--dir"); c.add(dir.getPath());
        c.add("--ipsw-dir"); c.add(ipswDir.getPath());
        c.add("--ecid-dir"); c.add(ecidDir.getPath());
        c.add("--rom"); c.add(rom.getPath());
        c.add("--ecid"); c.add(Long.toHexString(ecid));
        return c;
    }

    static String meaning(int exit) {
        switch (exit) {
            case 3: return "종료 코드 3: 요청이 유효합니다. 실행 스위치에서 정지했습니다(정상). 이 빌드는 복원을 시작하지 않습니다.";
            case 2: return "종료 코드 2: 사전검사가 거부했습니다. 아래 BLOCK 항목이 이유입니다.";
            case 64: return "종료 코드 64: 인자 오류(앱이 넘긴 값이 올바르지 않음).";
            default: return "예상하지 못한 종료 코드 " + exit + " (정상은 3, 거부는 2, 인자 오류는 64).";
        }
    }

    /** Removes the ECID (as the hex the process was given, with or without 0x) from text that is about to be shown. */
    static String mask(String text, long ecid) {
        if (text == null || ecid <= 0) return text;
        String h = Long.toHexString(ecid);
        return text.replace("0x" + h, "(ECID)").replace(h, "(ECID)").replace(h.toUpperCase(), "(ECID)");
    }

    /** Runs the probe. dir is where the pair would be (an existing private directory), ipswDir where the IPSW is. */
    public static Result run(File nativeLibDir, File dir, File ipswDir, File ecidDir, File rom, long ecid) {
        return runWith(REAL, nativeLibDir, dir, ipswDir, ecidDir, rom, ecid);
    }

    static Result runWith(Spawner spawner, File nativeLibDir, File dir, File ipswDir, File ecidDir, File rom, long ecid) {
        if (!isPackaged(nativeLibDir)) return new Result(false, -1, "", "g7_probe(" + LIB_NAME + ")가 이 APK에 없습니다(MISSING). 실행하지 않았습니다.");
        if (rom == null || !rom.isFile()) return new Result(false, -1, "", "정상 ROM이 없어 실행하지 않았습니다(외부 vm/rom.bin도 내부 저장본도 정상 ROM이 아님).");
        if (ecid <= 0 || !VmEcid.inRange(ecid)) return new Result(false, -1, "", "유효한 ECID(vm_ecid.txt)가 없어 실행하지 않았습니다. 이 화면은 ECID를 만들지 않습니다.");
        List<String> cmd = buildCommand(nativeLibDir, dir, ipswDir, ecidDir, rom, ecid);
        Process p;
        try { p = spawner.spawn(cmd); }
        catch (IOException e) { return new Result(false, -1, "", "실행 파일을 시작하지 못했습니다: " + mask(String.valueOf(e.getMessage()), ecid)); }
        final ByteArrayOutputStream buf = new ByteArrayOutputStream();
        final InputStream in = p.getInputStream();
        Thread reader = new Thread(() -> {
            byte[] b = new byte[2048];
            try { int n; while ((n = in.read(b)) > 0) { if (buf.size() < MAX_OUTPUT_BYTES) buf.write(b, 0, Math.min(n, MAX_OUTPUT_BYTES - buf.size())); } }
            catch (IOException ignored) { }
        });
        reader.setDaemon(true);
        reader.start();
        int exit;
        try {
            if (!p.waitFor(TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                p.destroyForcibly();
                return new Result(true, -1, mask(buf.toString(), ecid), TIMEOUT_SECONDS + "초 안에 끝나지 않아 중단했습니다.");
            }
            exit = p.exitValue();
            reader.join(2000);
        } catch (InterruptedException e) {
            p.destroyForcibly();
            Thread.currentThread().interrupt();
            return new Result(true, -1, "", "중단되었습니다.");
        }
        String out = mask(new String(buf.toByteArray(), StandardCharsets.UTF_8), ecid);
        return new Result(true, exit, out, meaning(exit));
    }
}
