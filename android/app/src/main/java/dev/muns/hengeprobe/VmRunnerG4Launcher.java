package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The ONLY place in the app that may create the process of the G4 UI diagnostic (libuirunnervmg4.so =
 * host/ui_runner_vm_g4.c). Mirrors VmRunnerLauncher's own structure and its one-switch discipline exactly
 * (that class's own header comment explains why: a single compile-time literal, checked first, before any
 * command is even built, with its own CI source check).
 *
 * START_ENABLED is true in this build, by a fresh, explicit approval given for exactly this change (the same
 * kind of approval this project has already given, and already exercised, for the G0-G6 restore-diagnostic
 * buttons in MainActivity.java -- this is the same category of real Apple TSS request and real guest restore
 * progress, just reached through this UI-rendering binary instead of those text-log diagnostics). Running
 * host/ui_runner_vm_g4.c makes a REAL request to Apple's signing server and drives the virtual guest through
 * DFU->Recovery->RecoveryOS (see that file's own header comment) the moment it is actually started -- there is
 * no way to invoke this binary "for observation only". A later change to turn it back off is, symmetrically,
 * its own fresh, explicit approval too.
 *
 * No Android classes are used, so tests/java/VmRunnerG4LauncherTest.java runs on a plain JVM.
 */
public final class VmRunnerG4Launcher {
    private VmRunnerG4Launcher() {}

    /** The one switch. tools/check-g4ui-launcher-source.sh requires exactly this declaration and a literal
     *  true/false, exactly like tools/check-c13-source.sh does for VmRunnerLauncher.START_ENABLED. */
    static final boolean START_ENABLED = true;

    static final String LIB_NAME = "libuirunnervmg4.so";
    static final String REFUSAL = "이 빌드에서는 G4 UI 실행이 비활성입니다(START_ENABLED=" + START_ENABLED + ")";

    /** host/ui_runner_vm_g4.c's own --seconds: the overall process budget, timed from when its control thread is
     *  created. Must exceed the worst case of hv_g4_start()'s own up-to-60s USB-online wait plus hv_g4_wait()'s
     *  own up-to-380s wait plus its own further up-to-20s cancel grace (~460s total) -- the same floor
     *  MainActivity.java's existing [G4] button already uses for vmrun_g4 (`if (g4 && secs < 460) secs = 460`).
     *  Kept equal to that proven floor, not derived from it at runtime, so tools/check-g4ui-launcher-source.sh can
     *  check it the same way tools/check-c13-source.sh checks RUN_SECONDS. */
    static final int RUN_SECONDS = 460;

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

    /** The command the runner would get: every option host/ui_runner_vm_g4.c's own "are required" message lists
     *  (--shm --rom --disk --aux --ipsw --ecid), from typed arguments only, plus --workdir (required here, even
     *  though the native side defaults it to ".", so the caller always states where restore work files land) and
     *  the one optional option this launcher adds on its own (--seconds, RUN_SECONDS above) -- never anything
     *  else. The safety settings host/ui_runner_vm_g4.c enforces itself (read-only storage, no guest network,
     *  installer=1, a nonzero ecid) are not options; they are inside the runner's own g4ui_config_ok(). ecid is
     *  taken as the exact hex string host/ui_runner_vm_g4.c's --ecid parses (strtoull base 0): this class never
     *  formats or reinterprets it. */
    static List<String> buildCommand(File nativeLibDir, File shm, File rom, File disk, File aux, File ipsw, String ecidHex, File workdir) {
        List<String> c = new ArrayList<>();
        c.add(new File(nativeLibDir, LIB_NAME).getPath());
        c.add("--shm"); c.add(shm.getPath());
        c.add("--rom"); c.add(rom.getPath());
        c.add("--disk"); c.add(disk.getPath());
        c.add("--aux"); c.add(aux.getPath());
        c.add("--ipsw"); c.add(ipsw.getPath());
        c.add("--ecid"); c.add(ecidHex);
        c.add("--workdir"); c.add(workdir.getPath());
        c.add("--seconds"); c.add(String.valueOf(RUN_SECONDS));
        return c;
    }

    public static Result start(File nativeLibDir, File shm, File rom, File disk, File aux, File ipsw, String ecidHex, File workdir) {
        return startWith(REAL, nativeLibDir, shm, rom, disk, aux, ipsw, ecidHex, workdir);
    }

    /** The switch is looked at first, immediately before any process could be made -- exactly like
     *  VmRunnerLauncher.startWith(), and for the same reason (that class's own comment: "immediately before any
     *  process could be made"). */
    static Result startWith(Spawner spawner, File nativeLibDir, File shm, File rom, File disk, File aux, File ipsw, String ecidHex, File workdir) {
        if (!START_ENABLED) return new Result(false, REFUSAL, null);
        if (nativeLibDir == null || shm == null || rom == null || disk == null || aux == null || ipsw == null || workdir == null
                || ecidHex == null || ecidHex.isEmpty()) return new Result(false, "경로 또는 ECID가 지정되지 않았습니다", null);
        try {
            return new Result(true, "started", spawner.spawn(buildCommand(nativeLibDir, shm, rom, disk, aux, ipsw, ecidHex, workdir)));
        } catch (IOException e) {
            return new Result(false, "프로세스 시작 실패: " + e.getMessage(), null);
        }
    }
}
