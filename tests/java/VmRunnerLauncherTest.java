package dev.muns.hengeprobe;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.List;

/** Plain-java test: javac -d out android/.../VmRunnerLauncher.java tests/java/VmRunnerLauncherTest.java && java -cp out dev.muns.hengeprobe.VmRunnerLauncherTest
 *
 *  Written for START_ENABLED=true (real execution on in this build). The checks that only made sense while the
 *  switch was off ("blocked: not started, reason given" with REFUSAL, "spawned == 0" for every valid call) are
 *  gone: with the switch on, a valid call DOES reach the spawner. What stays true regardless of the switch -- a
 *  missing/null path is still refused before any spawn, a failing real exec is still handled without a crash or a
 *  stray file, the command line is exactly the required options plus --seconds -- is still checked here. */
public class VmRunnerLauncherTest {
    static int fails;
    static int spawned;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    /** A Process that does nothing and was never really started -- enough to prove startWith() handed back
     *  exactly the object its Spawner returned, without needing a real OS process. */
    static final class FakeProcess extends Process {
        @Override public OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
        @Override public InputStream getInputStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public int waitFor() { return 0; }
        @Override public int exitValue() { return 0; }
        @Override public void destroy() { }
    }

    public static void main(String[] a) throws Exception {
        File dir = Files.createTempDirectory("c13").toFile();
        File shm = new File(dir, "shm"), rom = new File(dir, "rom"), disk = new File(dir, "disk"), aux = new File(dir, "aux");
        VmRunnerLauncher.Spawner counting = (List<String> c) -> { spawned++; throw new java.io.IOException("must not be called"); };

        check("START_ENABLED and startEnabled() agree with each other", VmRunnerLauncher.START_ENABLED == VmRunnerLauncher.startEnabled());
        System.out.println("(this build: VmRunnerLauncher.START_ENABLED = " + VmRunnerLauncher.START_ENABLED + ")");

        // null/missing paths are refused before any spawn is attempted, regardless of the switch
        VmRunnerLauncher.Result r = VmRunnerLauncher.startWith(counting, null, null, null, null, null);
        check("null arguments refused before any spawn", !r.started && r.process == null && spawned == 0);
        r = VmRunnerLauncher.startWith(counting, dir, shm, rom, disk, null);
        check("a missing aux path is refused before any spawn", !r.started && r.process == null && spawned == 0);

        // a valid call, once the switch is on, actually reaches the spawner and hands back exactly what it returns
        FakeProcess fake = new FakeProcess();
        VmRunnerLauncher.Spawner succeeding = (List<String> c) -> fake;
        r = VmRunnerLauncher.startWith(succeeding, dir, shm, rom, disk, aux);
        check("valid arguments: start() reports started and returns the spawner's process",
                VmRunnerLauncher.START_ENABLED ? (r.started && r.process == fake) : (!r.started && r.process == null));

        // existing or missing ROM / disk files change nothing about whether startWith() itself refuses or spawns --
        // that distinction belongs to VmDisplayActivity's own path checks, not to this class
        Files.write(rom.toPath(), new byte[] {1}); Files.write(disk.toPath(), new byte[] {2});
        r = VmRunnerLauncher.startWith(succeeding, dir, shm, rom, disk, aux);
        check("ROM and disk present: still just forwards to the spawner, nothing extra required here",
                VmRunnerLauncher.START_ENABLED ? (r.started && r.process == fake) : !r.started);

        // a real exec of a nonexistent program must fail safely -- no crash, no process, no stray file -- whatever
        // the switch is (when the switch is off this is refused before exec is even attempted)
        String[] before = dir.list(); java.util.Arrays.sort(before);
        VmRunnerLauncher.Result real = VmRunnerLauncher.start(dir, new File(dir, "x1"), new File(dir, "x2"), new File(dir, "x3"), new File(dir, "x4"));
        String[] after = dir.list(); java.util.Arrays.sort(after);
        check("a failing real start (nonexistent program) does not start and creates no file", !real.started && java.util.Arrays.equals(before, after));

        check("isPackaged false when the library is absent", !VmRunnerLauncher.isPackaged(dir) && !VmRunnerLauncher.isPackaged(null));
        Files.write(new File(dir, VmRunnerLauncher.LIB_NAME).toPath(), new byte[] {0});
        check("isPackaged true when the file is there", VmRunnerLauncher.isPackaged(dir));

        List<String> cmd = VmRunnerLauncher.buildCommand(dir, shm, rom, disk, aux);
        // written out by hand (not taken from the launcher): the four options host/ui_runner_vm.c requires, each with
        // its own path, plus the one optional option this launcher adds on its own (--seconds)
        List<String> want = java.util.Arrays.asList(new File(dir, VmRunnerLauncher.LIB_NAME).getPath(),
                "--shm", shm.getPath(), "--rom", rom.getPath(), "--disk", disk.getPath(), "--aux", aux.getPath(),
                "--seconds", String.valueOf(VmRunnerLauncher.RUN_SECONDS));
        check("command is exactly program + --shm/--rom/--disk/--aux/--seconds with their values", cmd.equals(want));
        check("RUN_SECONDS is positive and bounded (0 would disable ui_runner_vm's own time limit entirely)",
                VmRunnerLauncher.RUN_SECONDS > 0 && VmRunnerLauncher.RUN_SECONDS <= 120);
        boolean noLoosen = true;
        for (String s : cmd) if (s.matches("--(installer|net|ipsw|ecid|seed|readonly|no-readonly|write|rw|dfu)")) noLoosen = false;
        check("no setting-loosening option in the command", noLoosen);

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
