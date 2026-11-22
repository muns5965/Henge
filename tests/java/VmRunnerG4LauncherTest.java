package dev.muns.hengeprobe;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.List;

/** Plain-java test: javac -d out android/.../VmRunnerG4Launcher.java tests/java/VmRunnerG4LauncherTest.java &&
 *  java -cp out dev.muns.hengeprobe.VmRunnerG4LauncherTest
 *
 *  Written for START_ENABLED=false (Phase 2: plumbing only, real execution stays off). Every valid call must be
 *  refused with REFUSAL and reach the spawner zero times, whatever the arguments are -- there is no "observation
 *  only" mode for this binary (see VmRunnerG4Launcher's own header comment), so unlike VmRunnerLauncherTest this
 *  file does not need a START_ENABLED-conditional branch: the disabled-path assertions are unconditional. What
 *  VmRunnerLauncherTest already covers for the shared shape (null/missing paths refused before any spawn, a
 *  failing real exec handled without a crash or a stray file, isPackaged) is covered here too, plus the three
 *  G4-specific required options (--ipsw --ecid --workdir) and the fact that START_ENABLED being false must never
 *  depend on anything computed at runtime. */
public class VmRunnerG4LauncherTest {
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
        File dir = Files.createTempDirectory("g4ui").toFile();
        File shm = new File(dir, "shm"), rom = new File(dir, "rom"), disk = new File(dir, "disk"), aux = new File(dir, "aux");
        File ipsw = new File(dir, "fw.ipsw"), workdir = new File(dir, "work");
        String ecid = "0x1122334455";
        VmRunnerG4Launcher.Spawner counting = (List<String> c) -> { spawned++; throw new java.io.IOException("must not be called"); };

        check("this build ships with real execution off", !VmRunnerG4Launcher.START_ENABLED);
        check("START_ENABLED and startEnabled() agree with each other", VmRunnerG4Launcher.START_ENABLED == VmRunnerG4Launcher.startEnabled());

        // null/missing arguments are refused before any spawn is attempted
        VmRunnerG4Launcher.Result r = VmRunnerG4Launcher.startWith(counting, null, null, null, null, null, null, null, null);
        check("null arguments refused before any spawn", !r.started && r.process == null && spawned == 0);
        r = VmRunnerG4Launcher.startWith(counting, dir, shm, rom, disk, aux, ipsw, null, workdir);
        check("a missing ecid is refused before any spawn", !r.started && r.process == null && spawned == 0);
        r = VmRunnerG4Launcher.startWith(counting, dir, shm, rom, disk, aux, null, ecid, workdir);
        check("a missing ipsw path is refused before any spawn", !r.started && r.process == null && spawned == 0);
        r = VmRunnerG4Launcher.startWith(counting, dir, shm, rom, disk, aux, ipsw, "", workdir);
        check("an empty ecid string is refused before any spawn", !r.started && r.process == null && spawned == 0);

        // even a fully valid, well-formed call never reaches the spawner while the switch is off
        FakeProcess fake = new FakeProcess();
        VmRunnerG4Launcher.Spawner succeeding = (List<String> c) -> fake;
        r = VmRunnerG4Launcher.startWith(succeeding, dir, shm, rom, disk, aux, ipsw, ecid, workdir);
        check("fully valid arguments are still refused with REFUSAL while START_ENABLED is false",
                !r.started && r.process == null && r.reason.equals(VmRunnerG4Launcher.REFUSAL));

        // a real exec of a nonexistent program must fail safely -- no crash, no process, no stray file
        String[] before = dir.list(); java.util.Arrays.sort(before);
        VmRunnerG4Launcher.Result real = VmRunnerG4Launcher.start(dir, new File(dir, "x1"), new File(dir, "x2"), new File(dir, "x3"), new File(dir, "x4"), ipsw, ecid, workdir);
        String[] after = dir.list(); java.util.Arrays.sort(after);
        check("a real start() call (disabled) does not start and creates no file", !real.started && java.util.Arrays.equals(before, after));

        check("isPackaged false when the library is absent", !VmRunnerG4Launcher.isPackaged(dir) && !VmRunnerG4Launcher.isPackaged(null));
        Files.write(new File(dir, VmRunnerG4Launcher.LIB_NAME).toPath(), new byte[] {0});
        check("isPackaged true when the file is there", VmRunnerG4Launcher.isPackaged(dir));

        // buildCommand() itself is checkable independently of the switch -- it is the Phase-2 contract this test
        // pins down, so a later change that drops a required option or adds an unapproved one is caught here.
        List<String> cmd = VmRunnerG4Launcher.buildCommand(dir, shm, rom, disk, aux, ipsw, ecid, workdir);
        List<String> want = java.util.Arrays.asList(new File(dir, VmRunnerG4Launcher.LIB_NAME).getPath(),
                "--shm", shm.getPath(), "--rom", rom.getPath(), "--disk", disk.getPath(), "--aux", aux.getPath(),
                "--ipsw", ipsw.getPath(), "--ecid", ecid, "--workdir", workdir.getPath(),
                "--seconds", String.valueOf(VmRunnerG4Launcher.RUN_SECONDS));
        check("command is exactly program + --shm/--rom/--disk/--aux/--ipsw/--ecid/--workdir/--seconds with their values", cmd.equals(want));
        check("RUN_SECONDS is at least the proven G4 floor MainActivity.java's [G4] button already uses (460s)",
                VmRunnerG4Launcher.RUN_SECONDS >= 460);
        boolean noLoosen = true;
        for (String s : cmd) if (s.matches("--(installer|net|seed|readonly|no-readonly|write|rw|dfu|stop-wait-ms|ram-mb|cpus|jit-mb)")) noLoosen = false;
        check("no extra setting-loosening option in the command", noLoosen);

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
