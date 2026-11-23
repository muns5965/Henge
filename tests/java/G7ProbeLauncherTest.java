package dev.muns.hengeprobe;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Plain java: javac -encoding UTF-8 -d out G7ProbeLauncher.java VmRomCheck.java RomSlice.java VmEcid.java VmRunnerInstalledLauncher.java tests/java/G7ProbeLauncherTest.java && java -cp out dev.muns.hengeprobe.G7ProbeLauncherTest
 *  No real process is ever started here: every call goes through a fake Spawner. */
public class G7ProbeLauncherTest {
    static int fails, spawned;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }

    static final class FakeProcess extends Process {
        final byte[] out; final int exit; final boolean hang; boolean destroyed;
        FakeProcess(String out, int exit, boolean hang) { this.out = out.getBytes(StandardCharsets.UTF_8); this.exit = exit; this.hang = hang; }
        @Override public OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
        @Override public InputStream getInputStream() { return new ByteArrayInputStream(out); }
        @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public int waitFor() { return exit; }
        @Override public boolean waitFor(long t, TimeUnit u) { return !hang; }
        @Override public int exitValue() { return exit; }
        @Override public void destroy() { destroyed = true; }
        @Override public Process destroyForcibly() { destroyed = true; return this; }
    }

    static List<String> lastCmd;
    static G7ProbeLauncher.Spawner spawner(FakeProcess p) { return c -> { spawned++; lastCmd = c; return p; }; }
    static File touch(File d, String n, byte[] b) throws IOException { File f = new File(d, n); Files.write(f.toPath(), b); return f; }

    public static void main(String[] a) throws Exception {
        File lib = Files.createTempDirectory("lib").toFile(), d = Files.createTempDirectory("vm").toFile();
        File rom = touch(d, "AVPBooter.vmapple2.bin", new byte[] {1, 2, 3});
        long ecid = 0x13579bdf024681L;
        String hex = Long.toHexString(ecid);

        // not packaged: nothing is started
        G7ProbeLauncher.Result r = G7ProbeLauncher.runWith(spawner(new FakeProcess("", 3, false)), lib, d, d, d, rom, ecid);
        check("no libg7probe.so: not run, says MISSING, spawner not reached", !r.ran && spawned == 0 && r.meaning.contains("MISSING"));
        touch(lib, G7ProbeLauncher.LIB_NAME, new byte[] {1});
        check("isPackaged is true once the file is there", G7ProbeLauncher.isPackaged(lib) && !G7ProbeLauncher.isPackaged(null));

        // missing inputs
        r = G7ProbeLauncher.runWith(spawner(new FakeProcess("", 3, false)), lib, d, d, d, null, ecid);
        check("no ROM: not run", !r.ran && spawned == 0);
        r = G7ProbeLauncher.runWith(spawner(new FakeProcess("", 3, false)), lib, d, d, d, new File(d, "nope"), ecid);
        check("a ROM path that is not a file: not run", !r.ran && spawned == 0);
        for (long bad : new long[] {-1, 0, 1, 1L << 62}) {
            r = G7ProbeLauncher.runWith(spawner(new FakeProcess("", 3, false)), lib, d, d, d, rom, bad);
            check("ECID " + bad + " (missing or out of range): not run", !r.ran && spawned == 0);
        }

        // the command: exactly the options the native side requires, nothing else
        r = G7ProbeLauncher.runWith(spawner(new FakeProcess("PASS x\nG7PROBE RESULT: BLOCKED_BY_SWITCH stage=2: ok\n", 3, false)), lib, d, d, d, rom, ecid);
        check("exit 3: ran, meaning says stopped at the switch", r.ran && r.exit == 3 && r.meaning.contains("스위치") && spawned == 1);
        check("command: program, then exactly --dir --ipsw-dir --ecid-dir --rom --ecid", lastCmd.size() == 11 && lastCmd.get(0).endsWith(G7ProbeLauncher.LIB_NAME)
                && lastCmd.get(1).equals("--dir") && lastCmd.get(3).equals("--ipsw-dir") && lastCmd.get(5).equals("--ecid-dir") && lastCmd.get(7).equals("--rom") && lastCmd.get(9).equals("--ecid")
                && lastCmd.get(10).equals(hex));
        boolean noExtra = true;
        for (String s : lastCmd) if (s.startsWith("--") && !Arrays.asList("--dir", "--ipsw-dir", "--ecid-dir", "--rom", "--ecid").contains(s)) noExtra = false;
        check("no other option (no --installer, no write switch, no seconds)", noExtra);

        // exit codes
        check("exit 2 -> refused text", G7ProbeLauncher.runWith(spawner(new FakeProcess("BLOCK space 1\n", 2, false)), lib, d, d, d, rom, ecid).meaning.contains("거부"));
        check("exit 64 -> argument error text", G7ProbeLauncher.runWith(spawner(new FakeProcess("", 64, false)), lib, d, d, d, rom, ecid).meaning.contains("인자"));
        check("exit 0 is not a valid outcome and is reported as unexpected", G7ProbeLauncher.runWith(spawner(new FakeProcess("", 0, false)), lib, d, d, d, rom, ecid).meaning.contains("예상하지 못한"));
        check("a signal-like exit 139 is unexpected", G7ProbeLauncher.runWith(spawner(new FakeProcess("", 139, false)), lib, d, d, d, rom, ecid).meaning.contains("예상하지 못한"));

        // the ECID never reaches the screen text
        r = G7ProbeLauncher.runWith(spawner(new FakeProcess("leak 0x" + hex + " and " + hex + " and " + hex.toUpperCase() + "\n", 2, false)), lib, d, d, d, rom, ecid);
        check("the ECID is masked in the output (0x, plain, upper case)", !r.output.toLowerCase().contains(hex) && r.output.contains("(ECID)"));

        // timeout: the process is destroyed, not waited for
        FakeProcess hang = new FakeProcess("partial", 3, true);
        r = G7ProbeLauncher.runWith(spawner(hang), lib, d, d, d, rom, ecid);
        check("a hung process is destroyed and reported", hang.destroyed && r.ran && r.exit == -1 && r.meaning.contains("중단"));

        // output cap
        StringBuilder big = new StringBuilder(); for (int i = 0; i < 4000; i++) big.append("0123456789\n");
        r = G7ProbeLauncher.runWith(spawner(new FakeProcess(big.toString(), 2, false)), lib, d, d, d, rom, ecid);
        check("output is capped at " + G7ProbeLauncher.MAX_OUTPUT_BYTES + " bytes", r.output.length() <= G7ProbeLauncher.MAX_OUTPUT_BYTES);

        // spawn failure
        r = G7ProbeLauncher.runWith(c -> { throw new IOException("exec failed for " + c.get(c.size() - 1)); }, lib, d, d, d, rom, ecid);
        check("a spawn failure is reported and the ECID in the message is masked", !r.ran && !r.meaning.contains(hex));

        // ROM selection: same order as MainActivity.findLocalRom
        byte[] good = new byte[64]; Arrays.fill(good, (byte) 7);
        String sha = hexOf(MessageDigest.getInstance("SHA-256").digest(good));
        File ext = Files.createTempDirectory("ext").toFile(), priv = Files.createTempDirectory("priv").toFile();
        check("no ROM anywhere -> null", G7ProbeLauncher.usableRomFile(ext, priv, 64, sha) == null);
        File in = touch(priv, "AVPBooter.vmapple2.bin", good);
        check("only the internal copy is genuine -> internal", in.equals(G7ProbeLauncher.usableRomFile(ext, priv, 64, sha)));
        File ex = touch(ext, "rom.bin", good);
        check("both genuine -> external first", ex.equals(G7ProbeLauncher.usableRomFile(ext, priv, 64, sha)));
        touch(ext, "rom.bin", new byte[64]);
        check("external is not genuine -> falls back to internal, nothing deleted", in.equals(G7ProbeLauncher.usableRomFile(ext, priv, 64, sha)) && new File(ext, "rom.bin").exists());

        // reading the ECID never creates the file
        File p2 = Files.createTempDirectory("e").toFile();
        check("readEcid with no file: -1 and nothing created", G7ProbeLauncher.readEcid(p2) == -1 && p2.list().length == 0);
        touch(p2, VmEcid.FILE, ("ecid=0x" + hex + "\n").getBytes(StandardCharsets.UTF_8));
        check("readEcid reads the value", G7ProbeLauncher.readEcid(p2) == ecid);
        touch(p2, VmEcid.FILE, "ecid=0x1\n".getBytes(StandardCharsets.UTF_8));
        check("readEcid refuses an out-of-range value and does not replace the file", G7ProbeLauncher.readEcid(p2) == -1 && p2.list().length == 1);

        System.out.println(fails == 0 ? "ALL PASSED" : "FAILED " + fails);
        System.exit(fails == 0 ? 0 : 1);
    }

    static String hexOf(byte[] b) { StringBuilder s = new StringBuilder(); for (byte x : b) s.append(String.format("%02x", x)); return s.toString(); }
}
