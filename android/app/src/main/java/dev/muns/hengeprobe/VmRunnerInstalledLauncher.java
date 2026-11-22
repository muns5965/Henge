package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The ONLY place in the app that may create the process of the installed-disk VM runner (libuirunnervmi.so =
 * host/ui_runner_vm_installed.c): a normal, non-installer boot, storage read-only, no guest network, that is allowed a
 * nonzero machine identity (--ecid) so an already-personalized macOS disk can be booted. Mirrors VmRunnerG4Launcher's
 * structure and its one-switch discipline (a single compile-time literal, checked before anything else, with its own
 * CI source check: tools/check-installed-launcher-source.sh).
 *
 * START_ENABLED is FALSE in this build. This class is plumbing: with the switch false, no call -- from any screen or
 * Intent -- can create a process; startWith() returns REFUSAL before it even looks at its arguments. Turning it on is a
 * separate change that needs its own fresh, explicit approval (and a real installed disk to boot, which this app does
 * not have today; whether one exists on the phone is unverified).
 *
 * Even with the switch on, this launcher never substitutes a blank image: rom/disk/aux must be existing regular files,
 * and a file whose name starts with "blank_" is refused. A file merely existing does not mean macOS is installed on it;
 * this class cannot tell, and says so (see VmInstalledDiag). The ECID is passed through exactly as given and must parse
 * to a nonzero value; whether it equals the ECID the disk was installed with is not checked here (it cannot be).
 *
 * No Android classes are used, so tests/java/VmRunnerInstalledLauncherTest.java runs on a plain JVM.
 */
public final class VmRunnerInstalledLauncher {
    private VmRunnerInstalledLauncher() {}

    /** The one switch. tools/check-installed-launcher-source.sh requires exactly this declaration and the literal false. */
    static final boolean START_ENABLED = false;

    static final String LIB_NAME = "libuirunnervmi.so";
    static final String REFUSAL = "이 빌드에서는 설치 디스크 부팅 실행이 비활성입니다(START_ENABLED=" + START_ENABLED + ")";

    /** host/ui_runner_vm_installed.c's own --seconds: the overall process budget. A bound for a normal boot attempt,
     *  not a proven value (no run has happened with this binary); changing it needs the same approval as the switch. */
    static final int RUN_SECONDS = 300;

    /** What start() did. process is null unless started. */
    public static final class Result {
        public final boolean started;
        public final String reason;
        public final Process process;
        Result(boolean started, String reason, Process process) { this.started = started; this.reason = reason; this.process = process; }
    }

    /** How a process is made; only the real one calls ProcessBuilder. Tests pass their own to prove nothing is spawned. */
    interface Spawner { Process spawn(List<String> command) throws IOException; }

    private static final Spawner REAL = command -> new ProcessBuilder(command).redirectErrorStream(true).start();

    /** True when the runner file is in the app's native library directory (the file is only looked at, never run). */
    public static boolean isPackaged(File nativeLibDir) {
        return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile();
    }

    public static boolean startEnabled() { return START_ENABLED; }

    /** Parses an --ecid/--seed hex string the way the native side does (strtoull base 0: 0x.. hex, 0.. octal, else decimal
     *  is NOT accepted here -- only 0x-prefixed hex, so there is exactly one reading). Returns -1 for anything else. */
    static long parseHex(String s) {
        if (s == null || !s.startsWith("0x") || s.length() < 3 || s.length() > 18) return -1;
        try { long v = Long.parseUnsignedLong(s.substring(2), 16); return v < 0 ? -1 : v; }
        catch (NumberFormatException e) { return -1; }
    }

    /** The command the runner would get: --shm --rom --disk --aux --ecid (all five required by the native side), the
     *  optional --seed (only when given) and --seconds (RUN_SECONDS) -- never anything else. The safety settings
     *  (read-only storage, no network, installer=0) are not options; they are inside the runner's own config check. */
    static List<String> buildCommand(File nativeLibDir, File shm, File rom, File disk, File aux, String ecidHex, String seedHex) {
        List<String> c = new ArrayList<>();
        c.add(new File(nativeLibDir, LIB_NAME).getPath());
        c.add("--shm"); c.add(shm.getPath());
        c.add("--rom"); c.add(rom.getPath());
        c.add("--disk"); c.add(disk.getPath());
        c.add("--aux"); c.add(aux.getPath());
        c.add("--ecid"); c.add(ecidHex);
        if (seedHex != null) { c.add("--seed"); c.add(seedHex); }
        c.add("--seconds"); c.add(String.valueOf(RUN_SECONDS));
        return c;
    }

    public static Result start(File nativeLibDir, File shm, File rom, File disk, File aux, String ecidHex, String seedHex) {
        return startWith(REAL, nativeLibDir, shm, rom, disk, aux, ecidHex, seedHex);
    }

    /** The argument checks that apply once the switch is on, kept separate so a test can exercise them while the switch
     *  is off. Returns the refusal reason, or null when the arguments are acceptable. */
    static String validate(File nativeLibDir, File shm, File rom, File disk, File aux, String ecidHex, String seedHex) {
        if (nativeLibDir == null || shm == null || rom == null || disk == null || aux == null)
            return "경로가 지정되지 않았습니다";
        if (!rom.isFile() || !disk.isFile() || !aux.isFile())
            return "rom/disk/aux 중 실제 파일이 아닌 것이 있습니다(없는 파일을 만들거나 blank 이미지로 바꾸지 않습니다)";
        if (disk.getName().startsWith("blank_") || aux.getName().startsWith("blank_"))
            return "blank 이미지는 설치 디스크로 쓰지 않습니다";
        if (parseHex(ecidHex) <= 0) return "ECID는 0이 아닌 0x 16진수여야 합니다";
        if (seedHex != null && parseHex(seedHex) < 0) return "seed는 0x 16진수여야 합니다";
        return null;
    }

    /** The switch is looked at first, immediately before any other check and before any process could be made. */
    static Result startWith(Spawner spawner, File nativeLibDir, File shm, File rom, File disk, File aux, String ecidHex, String seedHex) {
        if (!START_ENABLED) return new Result(false, REFUSAL, null);
        String why = validate(nativeLibDir, shm, rom, disk, aux, ecidHex, seedHex);
        if (why != null) return new Result(false, why, null);
        try {
            return new Result(true, "started", spawner.spawn(buildCommand(nativeLibDir, shm, rom, disk, aux, ecidHex, seedHex)));
        } catch (IOException e) {
            return new Result(false, "프로세스 시작 실패: " + e.getMessage(), null);
        }
    }
}
