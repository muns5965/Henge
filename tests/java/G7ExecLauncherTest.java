package dev.muns.hengeprobe;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Plain java (see tools/g7-exec-launcher-test.sh). No real process is ever started: every start goes through a fake Spawner. */
public class G7ExecLauncherTest {
    static int fails, spawned;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }

    static final class FakeProcess extends Process {
        final byte[] out; final int exit; volatile boolean destroyed, forced; final boolean hang;
        FakeProcess(String out, int exit, boolean hang) { this.out = out.getBytes(StandardCharsets.UTF_8); this.exit = exit; this.hang = hang; }
        @Override public OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
        @Override public InputStream getInputStream() {
            if (!hang) return new ByteArrayInputStream(out);
            return new InputStream() { @Override public int read() { while (!destroyed) { try { Thread.sleep(5); } catch (InterruptedException e) { return -1; } } return -1; } @Override public int read(byte[] b, int o, int l) { return read(); } };
        }
        @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public int waitFor() { while (hang && !destroyed) { try { Thread.sleep(5); } catch (InterruptedException e) { break; } } return exit; }
        @Override public boolean waitFor(long t, TimeUnit u) { if (!hang) return true; for (int i = 0; i < 400 && !destroyed; i++) { try { Thread.sleep(5); } catch (InterruptedException e) { break; } } return destroyed; }
        @Override public int exitValue() { return exit; }
        @Override public void destroy() { destroyed = true; }
        @Override public Process destroyForcibly() { forced = true; destroyed = true; return this; }
    }

    static List<String> lastCmd;
    static G7ExecLauncher.Spawner spawner(FakeProcess p) { return c -> { spawned++; lastCmd = c; return p; }; }
    static File touch(File d, String n, byte[] b) throws IOException { File f = new File(d, n); Files.write(f.toPath(), b); return f; }
    static File rom;
    static final G7ExecLauncher.RomFinder ROMS = (e, p) -> rom;
    static final String SHA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    public static void main(String[] a) throws Exception {
        File lib = Files.createTempDirectory("lib").toFile(), priv = Files.createTempDirectory("vm").toFile(), ext = Files.createTempDirectory("ext").toFile(), files = Files.createTempDirectory("files").toFile();
        rom = touch(priv, "AVPBooter.vmapple2.bin", new byte[] {1, 2, 3});
        long ecid = 0x0013579bdf024681L; String hex = Long.toHexString(ecid);

        check("the committed switch is OFF", !G7ExecLauncher.START_ENABLED);
        G7ExecLauncher.Start s = G7ExecLauncher.start(lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        check("the real entry point refuses while the switch is off (nothing created)", !s.started && s.message.contains("꺼져") && new File(files, G7ExecLauncher.WORK_NAME).list() == null && !new File(files, G7ExecLauncher.LOG_NAME).exists());

        // preflight looks only (never spawns, never creates)
        G7ExecLauncher.Preflight pf = G7ExecLauncher.preflightWith(ROMS, lib, ext, priv);
        check("preflight: not ready (no binary, no ECID, no IPSW) and nothing spawned", !pf.ready && spawned == 0 && pf.text.contains("MISSING"));
        touch(lib, G7ExecLauncher.LIB_NAME, new byte[] {1});
        touch(priv, "vm_ecid.txt", ("ecid=0x" + hex + "\n").getBytes());
        File ip = new File(priv, G7ExecLauncher.IPSW_NAME);
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(ip, "rw")) { r.setLength(G7ExecLauncher.IPSW_BYTES - 1); }
        check("preflight: an IPSW of the wrong size is not accepted", !G7ExecLauncher.preflightWith(ROMS, lib, ext, priv).ready);
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(ip, "rw")) { r.setLength(G7ExecLauncher.IPSW_BYTES); }
        pf = G7ExecLauncher.preflightWith(ROMS, lib, ext, priv);
        check("preflight: ready when binary, ROM, ECID, IPSW are there and no install files exist", pf.ready && spawned == 0 && !pf.text.contains(hex));
        File disk = touch(priv, "macos_disk.img", new byte[] {1});
        check("preflight: an existing install file blocks (no auto-resume / overwrite)", !G7ExecLauncher.preflightWith(ROMS, lib, ext, priv).ready);
        disk.delete();

        // switch off -> never reaches the spawner even when everything else is right
        s = G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, false, lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        check("switch off: refused, spawner not reached, nothing created", !s.started && spawned == 0 && !new File(files, G7ExecLauncher.WORK_NAME).exists());
        // each missing condition (switch ON) refuses before the spawner
        check("wrong approval phrase refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files, "승인", SHA).started && spawned == 0);
        check("no phrase refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files, null, SHA).started && spawned == 0);
        for (String bad : new String[] {null, "", "abc", SHA.substring(1), SHA + "0", SHA.replace('0', 'g'), SHA.replace('0', ' ')})
            check("SHA-256 '" + bad + "' refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, bad).started && spawned == 0);
        check("no ROM refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), (e, p) -> null, true, lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA).started && spawned == 0);
        check("no binary refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, files, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA).started && spawned == 0);
        new File(priv, "vm_ecid.txt").delete();
        check("no ECID refused (this screen never makes one)", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA).started && spawned == 0);
        touch(priv, "vm_ecid.txt", ("ecid=0x" + hex + "\n").getBytes());

        // a start (only possible here because the test turns the switch on for a fake process)
        FakeProcess fp = new FakeProcess("g7: step\nleak 0x" + hex + " and " + hex + " and " + hex.toUpperCase() + "\ng7-result: outcome=FAILED\n", 2, false);
        s = G7ExecLauncher.startWith(spawner(fp), ROMS, true, lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA.toUpperCase());
        check("start with a fake process", s.started && spawned == 1 && s.run != null);
        for (int i = 0; i < 100 && !s.run.finished(); i++) Thread.sleep(20);
        check("the run finished with the fake's exit code", s.run.finished() && s.run.exitCode() == 2);
        boolean exact = lastCmd.size() == 13 && lastCmd.get(0).endsWith(G7ExecLauncher.LIB_NAME) && lastCmd.get(1).equals("--dir") && lastCmd.get(3).equals("--ipsw-dir") && lastCmd.get(5).equals("--ecid-dir")
                && lastCmd.get(7).equals("--rom") && lastCmd.get(9).equals("--workdir") && lastCmd.get(11).equals("--ipsw-sha256") && lastCmd.get(12).equals(SHA);
        check("command: program and exactly --dir --ipsw-dir --ecid-dir --rom --workdir --ipsw-sha256 (lower-cased)", exact);
        boolean noEcid = true; for (String x : lastCmd) if (x.contains(hex) || x.equals("--ecid")) noEcid = false;
        check("the ECID is not on the command line", noEcid);
        String log = G7ExecLauncher.readLogTail(files, 100000);
        check("the log has the output, the exit line, and no ECID", log.contains("outcome=FAILED") && log.contains("g7-exit: 2") && !log.toLowerCase().contains(hex) && log.contains("(ECID)"));
        check("the on-screen tail has no ECID either", !s.run.tail().toLowerCase().contains(hex));
        check("the work directory was made only now", new File(files, G7ExecLauncher.WORK_NAME).isDirectory());
        check("nothing in the private vm dir was created or removed by the launcher", Arrays.asList(priv.list()).containsAll(Arrays.asList("AVPBooter.vmapple2.bin", "vm_ecid.txt", G7ExecLauncher.IPSW_NAME)) && priv.list().length == 3);
        for (int c : new int[] {0, 2, 3, 5, 64, 137, 143, 77}) check("meaning(" + c + ") says something", G7ExecLauncher.meaning(c).length() > 10);
        check("exit 77 is called unexpected, exit 5 keeps the pair", G7ExecLauncher.meaning(77).contains("예상하지 못한") && G7ExecLauncher.meaning(5).contains("in_progress"));

        // cancel: SIGTERM first (destroy), kill only after the grace
        FakeProcess hp = new FakeProcess("", 143, true);
        s = G7ExecLauncher.startWith(spawner(hp), ROMS, true, lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        s.run.cancel(); s.run.cancel();
        check("cancel asks the process to stop (destroy) and is safe twice", hp.destroyed && !hp.forced);
        for (int i = 0; i < 100 && !s.run.finished(); i++) Thread.sleep(20);
        check("after the stop the run ends with the signal exit and the pair text says in_progress", s.run.finished() && s.run.exitCode() == 143 && G7ExecLauncher.meaning(s.run.exitCode()).contains("in_progress"));

        // output bound
        StringBuilder big = new StringBuilder(); for (int i = 0; i < 40000; i++) big.append("0123456789abcdef");
        File files2 = Files.createTempDirectory("files2").toFile();
        s = G7ExecLauncher.startWith(spawner(new FakeProcess(big.toString(), 2, false)), ROMS, true, lib, ext, priv, files2, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        check("the log is bounded", new File(files2, G7ExecLauncher.LOG_NAME).length() <= G7ExecLauncher.MAX_LOG_BYTES + 64 && s.run.tail().length() <= G7ExecLauncher.TAIL_BYTES + 64);

        // a spawner failure leaves no run
        s = G7ExecLauncher.startWith(c -> { throw new IOException("boom 0x" + hex); }, ROMS, true, lib, ext, priv, Files.createTempDirectory("files3").toFile(), G7ExecLauncher.APPROVAL_PHRASE, SHA);
        check("spawn failure: not started, ECID masked in the message", !s.started && !s.message.contains(hex));

        System.out.println(fails == 0 ? "G7EXEC LAUNCHER OK (0 failed)" : "G7EXEC LAUNCHER FAILED (" + fails + ")");
        System.exit(fails == 0 ? 0 : 1);
    }
}
