package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * [C1-3] The ONLY place in the app that may create the process of the VM runner (libuirunnervm.so = host/ui_runner_vm).
 * In this build starting it is switched off: START_ENABLED is a compile-time constant that is false, start() refuses before it builds
 * a command or touches a file, and no screen, Intent extra or setting can change that. Turning it on is a source change that needs its own approval.
 * Whether the ROM or disk files exist is never a condition for starting (and nothing here creates them).
 * No Android classes are used, so tests/java/VmRunnerLauncherTest.java runs on a plain JVM.
 */
public final class VmRunnerLauncher {
    private VmRunnerLauncher() {}

    /** The one switch. CI (tools/check-c13-source.sh) requires exactly this declaration and the value false. */
    static final boolean START_ENABLED = false;

    static final String LIB_NAME = "libuirunnervm.so";
    static final String REFUSAL = "이 빌드에서는 VM 실행이 비활성입니다(START_ENABLED=false)";

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

    /** The command the runner would get: all four options host/ui_runner_vm.c requires (--shm --rom --disk --aux), from typed arguments only; the safety settings are not options (they are inside the runner). */
    static List<String> buildCommand(File nativeLibDir, File shm, File rom, File disk, File aux) {
        List<String> c = new ArrayList<>();
        c.add(new File(nativeLibDir, LIB_NAME).getPath());
        c.add("--shm"); c.add(shm.getPath());
        c.add("--rom"); c.add(rom.getPath());
        c.add("--disk"); c.add(disk.getPath());
        c.add("--aux"); c.add(aux.getPath());
        return c;
    }

    public static Result start(File nativeLibDir, File shm, File rom, File disk, File aux) {
        return startWith(REAL, nativeLibDir, shm, rom, disk, aux);
    }

    /** The switch is looked at first, immediately before any process could be made. */
    static Result startWith(Spawner spawner, File nativeLibDir, File shm, File rom, File disk, File aux) {
        if (!START_ENABLED) return new Result(false, REFUSAL, null);
        if (nativeLibDir == null || shm == null || rom == null || disk == null || aux == null) return new Result(false, "경로가 지정되지 않았습니다", null);
        try {
            return new Result(true, "started", spawner.spawn(buildCommand(nativeLibDir, shm, rom, disk, aux)));
        } catch (IOException e) {
            return new Result(false, "프로세스 시작 실패: " + e.getMessage(), null);
        }
    }
}
