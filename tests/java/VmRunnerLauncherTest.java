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
        File shm = new File(dir, "shm"), rom = new File(dir, "rom"), disk = new File(dir, "disk");
        VmRunnerLauncher.Spawner counting = (List<String> c) -> { spawned++; throw new java.io.IOException("must not be called"); };

        check("start is switched off in this build", !VmRunnerLauncher.START_ENABLED && !VmRunnerLauncher.startEnabled());
        VmRunnerLauncher.Result r = VmRunnerLauncher.startWith(counting, dir, shm, rom, disk);
        check("blocked: not started, reason given, no process", !r.started && r.process == null && r.reason == VmRunnerLauncher.REFUSAL && spawned == 0);
        r = VmRunnerLauncher.startWith(counting, null, null, null, null);
        check("blocked with null arguments too", !r.started && r.process == null && spawned == 0);
        check("public start() blocked: no process", !VmRunnerLauncher.start(dir, shm, rom, disk).started);

        // existing or missing ROM / disk files change nothing, and nothing is created
        Files.write(rom.toPath(), new byte[] {1}); Files.write(disk.toPath(), new byte[] {2});
        r = VmRunnerLauncher.startWith(counting, dir, shm, rom, disk);
        check("ROM and disk present: still blocked, nothing spawned", !r.started && spawned == 0);
        String[] before = dir.list(); java.util.Arrays.sort(before);
        VmRunnerLauncher.start(dir, new File(dir, "x1"), new File(dir, "x2"), new File(dir, "x3"));
        String[] after = dir.list(); java.util.Arrays.sort(after);
        check("no file created by a blocked start", java.util.Arrays.equals(before, after));

        check("isPackaged false when the library is absent", !VmRunnerLauncher.isPackaged(dir) && !VmRunnerLauncher.isPackaged(null));
        Files.write(new File(dir, VmRunnerLauncher.LIB_NAME).toPath(), new byte[] {0});
        check("isPackaged true when the file is there", VmRunnerLauncher.isPackaged(dir));

        List<String> cmd = VmRunnerLauncher.buildCommand(dir, shm, rom, disk);
        boolean noLoosen = true;
        for (String s : cmd) if (s.matches("--(installer|net|ipsw|ecid|seed|readonly|no-readonly|write|rw|dfu)")) noLoosen = false;
        check("command has only the typed paths, no setting-loosening option", cmd.size() == 7 && noLoosen && cmd.get(1).equals("--shm"));

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
