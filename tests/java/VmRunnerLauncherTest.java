package dev.muns.hengeprobe;

import java.io.File;
import java.nio.file.Files;
import java.util.List;

/** Plain-java test: javac -d out android/.../VmRunnerLauncher.java tests/java/VmRunnerLauncherTest.java && java -cp out dev.muns.hengeprobe.VmRunnerLauncherTest */
public class VmRunnerLauncherTest {
    static int fails;
    static int spawned;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    public static void main(String[] a) throws Exception {
        File dir = Files.createTempDirectory("c13").toFile();
        File shm = new File(dir, "shm"), rom = new File(dir, "rom"), disk = new File(dir, "disk"), aux = new File(dir, "aux");
        VmRunnerLauncher.Spawner counting = (List<String> c) -> { spawned++; throw new java.io.IOException("must not be called"); };

        check("start is switched off in this build", !VmRunnerLauncher.START_ENABLED && !VmRunnerLauncher.startEnabled());
        VmRunnerLauncher.Result r = VmRunnerLauncher.startWith(counting, dir, shm, rom, disk, aux);
        check("blocked: not started, reason given, no process", !r.started && r.process == null && r.reason == VmRunnerLauncher.REFUSAL && spawned == 0);
        r = VmRunnerLauncher.startWith(counting, null, null, null, null, null);
        check("blocked with null arguments too", !r.started && r.process == null && spawned == 0);
        check("public start() blocked: no process", !VmRunnerLauncher.start(dir, shm, rom, disk, aux).started);

        // existing or missing ROM / disk files change nothing, and nothing is created
        Files.write(rom.toPath(), new byte[] {1}); Files.write(disk.toPath(), new byte[] {2});
        r = VmRunnerLauncher.startWith(counting, dir, shm, rom, disk, aux);
        check("ROM and disk present: still blocked, nothing spawned", !r.started && spawned == 0);
        String[] before = dir.list(); java.util.Arrays.sort(before);
        VmRunnerLauncher.start(dir, new File(dir, "x1"), new File(dir, "x2"), new File(dir, "x3"), new File(dir, "x4"));
        String[] after = dir.list(); java.util.Arrays.sort(after);
        check("no file created by a blocked start", java.util.Arrays.equals(before, after));

        check("isPackaged false when the library is absent", !VmRunnerLauncher.isPackaged(dir) && !VmRunnerLauncher.isPackaged(null));
        Files.write(new File(dir, VmRunnerLauncher.LIB_NAME).toPath(), new byte[] {0});
        check("isPackaged true when the file is there", VmRunnerLauncher.isPackaged(dir));

        List<String> cmd = VmRunnerLauncher.buildCommand(dir, shm, rom, disk, aux);
        // written out by hand (not taken from the launcher): the four options host/ui_runner_vm.c requires, each with its own path
        List<String> want = java.util.Arrays.asList(new File(dir, VmRunnerLauncher.LIB_NAME).getPath(),
                "--shm", shm.getPath(), "--rom", rom.getPath(), "--disk", disk.getPath(), "--aux", aux.getPath());
        check("command is exactly program + --shm/--rom/--disk/--aux with their paths", cmd.equals(want));
        boolean noLoosen = true;
        for (String s : cmd) if (s.matches("--(installer|net|ipsw|ecid|seed|readonly|no-readonly|write|rw|dfu)")) noLoosen = false;
        check("no setting-loosening option in the command", noLoosen);
        check("a missing aux path is refused before anything else", !VmRunnerLauncher.startWith(counting, dir, shm, rom, disk, null).started && spawned == 0);

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
