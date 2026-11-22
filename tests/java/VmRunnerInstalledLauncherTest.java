package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

/** Plain-java test: javac -encoding UTF-8 -d out android/.../VmRunnerInstalledLauncher.java android/.../VmInstalledDiag.java
 *  android/.../VmEcid.java tests/java/VmRunnerInstalledLauncherTest.java && java -cp out dev.muns.hengeprobe.VmRunnerInstalledLauncherTest
 *  Proves, for the switch as built (false): no call -- valid or not, spawner or the real start() -- creates a process.
 *  Also checks buildCommand, the argument validation (blank/missing files, ECID/seed), and the read-only readiness report. */
public class VmRunnerInstalledLauncherTest {
    static int fails, spawned;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }

    static final class FakeProcess extends Process {
        @Override public java.io.OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
        @Override public java.io.InputStream getInputStream() { return new java.io.ByteArrayInputStream(new byte[0]); }
        @Override public java.io.InputStream getErrorStream() { return new java.io.ByteArrayInputStream(new byte[0]); }
        @Override public int waitFor() { return 0; }
        @Override public int exitValue() { return 0; }
        @Override public void destroy() { }
    }

    static File touch(File d, String n) throws IOException { File f = new File(d, n); Files.write(f.toPath(), new byte[] {1}); return f; }
    static String snapshot(File d) { String[] l = d.list(); Arrays.sort(l); return String.join(",", l); }

    public static void main(String[] a) throws Exception {
        File dir = Files.createTempDirectory("inst").toFile();
        File shm = new File(dir, "shm"), rom = touch(dir, "rom.bin"), disk = touch(dir, "disk.img"), aux = touch(dir, "aux.img");
        String ecid = "0x1122334455";
        VmRunnerInstalledLauncher.Spawner counting = (List<String> c) -> { spawned++; return new FakeProcess(); };

        check("the shipped switch is false and startEnabled() agrees", !VmRunnerInstalledLauncher.START_ENABLED && !VmRunnerInstalledLauncher.startEnabled());

        // switch off: even a fully valid call (existing files, valid ecid) is refused with REFUSAL and never reaches the spawner
        VmRunnerInstalledLauncher.Result r = VmRunnerInstalledLauncher.startWith(counting, dir, shm, rom, disk, aux, ecid, null);
        check("valid call refused with REFUSAL, spawner reached zero times", !r.started && r.process == null && spawned == 0 && VmRunnerInstalledLauncher.REFUSAL.equals(r.reason));
        r = VmRunnerInstalledLauncher.startWith(counting, dir, shm, rom, disk, aux, ecid, "0x5");
        check("valid call with seed also refused, zero spawns", !r.started && spawned == 0);
        r = VmRunnerInstalledLauncher.startWith(counting, null, null, null, null, null, null, null);
        check("null arguments refused, zero spawns", !r.started && spawned == 0);
        // the real entry point (the only one other screens could call), with a real directory of existing files
        String before = snapshot(dir);
        r = VmRunnerInstalledLauncher.start(dir, shm, rom, disk, aux, ecid, null);
        check("the real start() creates no process and no file while the switch is off", !r.started && r.process == null && before.equals(snapshot(dir)));

        // argument validation, independent of the switch
        check("valid arguments pass validate()", VmRunnerInstalledLauncher.validate(dir, shm, rom, disk, aux, ecid, null) == null);
        check("a missing disk file is refused", VmRunnerInstalledLauncher.validate(dir, shm, rom, new File(dir, "none.img"), aux, ecid, null) != null);
        check("a missing aux file is refused", VmRunnerInstalledLauncher.validate(dir, shm, rom, disk, new File(dir, "none.img"), ecid, null) != null);
        check("a missing rom file is refused", VmRunnerInstalledLauncher.validate(dir, shm, new File(dir, "none.bin"), disk, aux, ecid, null) != null);
        File bd = touch(dir, "blank_disk.img"), ba = touch(dir, "blank_aux.img");
        check("a blank_ disk image is refused even though the file exists", VmRunnerInstalledLauncher.validate(dir, shm, rom, bd, aux, ecid, null) != null);
        check("a blank_ aux image is refused even though the file exists", VmRunnerInstalledLauncher.validate(dir, shm, rom, disk, ba, ecid, null) != null);
        check("a directory is not accepted as a disk", VmRunnerInstalledLauncher.validate(dir, shm, rom, dir, aux, ecid, null) != null);
        for (String bad : new String[] { null, "", "0x0", "0", "1234", "0xzz", "0x", "-1", "0x1ffffffffffffffff", "0xffffffffffffffff" })
            check("ecid '" + bad + "' is refused", VmRunnerInstalledLauncher.validate(dir, shm, rom, disk, aux, bad, null) != null);
        check("ecid 0x1 is the smallest accepted nonzero value", VmRunnerInstalledLauncher.validate(dir, shm, rom, disk, aux, "0x1", null) == null);
        check("seed 0x0 is accepted (default value)", VmRunnerInstalledLauncher.validate(dir, shm, rom, disk, aux, ecid, "0x0") == null);
        check("a malformed seed is refused", VmRunnerInstalledLauncher.validate(dir, shm, rom, disk, aux, ecid, "12") != null);

        // the command itself
        List<String> cmd = VmRunnerInstalledLauncher.buildCommand(dir, shm, rom, disk, aux, ecid, null);
        List<String> want = Arrays.asList(new File(dir, VmRunnerInstalledLauncher.LIB_NAME).getPath(), "--shm", shm.getPath(), "--rom", rom.getPath(),
                "--disk", disk.getPath(), "--aux", aux.getPath(), "--ecid", ecid, "--seconds", String.valueOf(VmRunnerInstalledLauncher.RUN_SECONDS));
        check("command is exactly program + --shm/--rom/--disk/--aux/--ecid/--seconds", cmd.equals(want));
        List<String> cmd2 = VmRunnerInstalledLauncher.buildCommand(dir, shm, rom, disk, aux, ecid, "0x7");
        check("a given seed adds exactly --seed <value>", cmd2.size() == want.size() + 2 && cmd2.contains("--seed") && cmd2.get(cmd2.indexOf("--seed") + 1).equals("0x7"));
        boolean noLoosen = true;
        for (String s : cmd2) if (s.matches("--(installer|net|readonly|no-readonly|write|rw|dfu|ipsw|workdir)")) noLoosen = false;
        check("no setting-loosening or installer option in the command", noLoosen);
        check("RUN_SECONDS is a bounded positive value", VmRunnerInstalledLauncher.RUN_SECONDS > 0 && VmRunnerInstalledLauncher.RUN_SECONDS <= 600);
        check("LIB_NAME is the packaged name", "libuirunnervmi.so".equals(VmRunnerInstalledLauncher.LIB_NAME));

        check("isPackaged false when absent/null", !VmRunnerInstalledLauncher.isPackaged(dir) && !VmRunnerInstalledLauncher.isPackaged(null));
        touch(dir, VmRunnerInstalledLauncher.LIB_NAME);
        check("isPackaged true when the file is there", VmRunnerInstalledLauncher.isPackaged(dir));

        // readiness report: read-only, no blank fallback, states what it cannot know
        File ext = Files.createTempDirectory("ext").toFile(), priv = Files.createTempDirectory("priv").toFile();
        touch(priv, "blank_disk.img");
        String b4e = snapshot(ext), b4p = snapshot(priv);
        String rep = VmInstalledDiag.report(ext, priv, dir);
        check("report writes nothing and creates no ECID file", b4e.equals(snapshot(ext)) && b4p.equals(snapshot(priv)));
        check("report says files are missing and does not mention the private blank image", rep.contains("disk.img: 없음") && !rep.contains("blank_disk"));
        check("report states it cannot tell whether macOS is installed", rep.contains("확인할 수 없음"));
        check("report says the run switch is off", rep.contains("비활성"));
        touch(ext, "disk.img"); touch(ext, "aux.img"); touch(ext, "rom.bin");
        rep = VmInstalledDiag.report(ext, priv, dir);
        check("report shows file presence and size, still without claiming an install", rep.contains("disk.img: 있음, 1 바이트") && rep.contains("확인할 수 없음"));
        Files.write(new File(priv, VmEcid.FILE).toPath(), "ecid=0x20000000000\n".getBytes("UTF-8"));
        rep = VmInstalledDiag.report(ext, priv, dir);
        check("report notes a stored ECID cannot be tied to the disk", rep.contains("같은지는 확인할 수 없음"));

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
