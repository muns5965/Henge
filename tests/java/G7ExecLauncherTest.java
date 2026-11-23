package dev.muns.hengeprobe;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.lang.reflect.Field;
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

        check("the g7exec build (stand-in BuildConfig) has the switch OFF and says so", !G7ExecLauncher.START_ENABLED && "G7-BUILD-KIND:EXEC-OFF".equals(G7ExecLauncher.BUILD_KIND));
        G7ExecLauncher.Start s = G7ExecLauncher.start(lib, ext, priv, files, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        check("the real entry point refuses while the switch is off (nothing created)", !s.started && s.message.contains("꺼져") && new File(files, G7ExecLauncher.WORK_NAME).list() == null && G7ExecLauncher.newestLog(files) == null);

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
        check("the log is bounded", G7ExecLauncher.newestLog(files2).length() <= G7ExecLauncher.MAX_LOG_BYTES + 3 * G7ExecLauncher.TAIL_BYTES && s.run.tail().length() <= G7ExecLauncher.TAIL_BYTES + 64);

        // the final result survives a full log; every run has its own file, nothing is appended to or overwritten
        File files4 = Files.createTempDirectory("files4").toFile();
        s = G7ExecLauncher.startWith(spawner(new FakeProcess(big + "\ng7-result: outcome=FAILED last_step=7 pair=in_progress\n", 2, false)), ROMS, true, lib, ext, priv, files4, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        String full = new String(Files.readAllBytes(G7ExecLauncher.newestLog(files4).toPath()), StandardCharsets.UTF_8);
        check("log limit reached: the marker, the last g7-result line and the exit code are still in the file", full.contains("log limit reached") && full.contains("g7-result: outcome=FAILED last_step=7") && full.contains("g7-exit: 2"));
        check("the file starts with g7-start", full.startsWith("g7-start: "));
        String first = s.run.logName();
        Thread.sleep(5);
        s = G7ExecLauncher.startWith(spawner(new FakeProcess("second\n", 2, false)), ROMS, true, lib, ext, priv, files4, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        File[] logs = files4.listFiles((d, n) -> n.startsWith(G7ExecLauncher.LOG_PREFIX));
        check("a second run writes a second file; the first one is untouched", logs.length == 2 && !s.run.logName().equals(first) && new File(files4, first).length() > G7ExecLauncher.MAX_LOG_BYTES / 2);
        check("readLogTail reads the newest run", G7ExecLauncher.readLogTail(files4, 1000).contains("second"));

        // the run state is process-wide, not the screen's: a "recreated" screen gets the same run, can cancel it, and a second start is refused while it runs
        File files5 = Files.createTempDirectory("files5").toFile();
        FakeProcess live = new FakeProcess("", 143, true);
        s = G7ExecLauncher.startWith(spawner(live), ROMS, true, lib, ext, priv, files5, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        check("current() returns the started run (what a recreated screen would read)", s.started && G7ExecLauncher.current() == s.run && !G7ExecLauncher.current().finished());
        int before = spawned;
        G7ExecLauncher.Start again = G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files5, G7ExecLauncher.APPROVAL_PHRASE, SHA);
        check("a second start while one runs is refused, nothing spawned", !again.started && spawned == before);
        check("lastRunNote says it is running", G7ExecLauncher.lastRunNote(files5).contains("실행 중"));
        G7ExecLauncher.current().cancel();
        for (int i = 0; i < 100 && !G7ExecLauncher.current().finished(); i++) Thread.sleep(20);
        check("cancel through current() ends it", live.destroyed && G7ExecLauncher.current().finished() && G7ExecLauncher.lastRunNote(files5).contains("끝났"));
        // a log with g7-start and no g7-exit (the app process was killed) is reported as interrupted
        File files6 = Files.createTempDirectory("files6").toFile();
        touch(files6, G7ExecLauncher.LOG_PREFIX + "0000000000001" + G7ExecLauncher.LOG_SUFFIX, "g7-start: 1\nsome output\n".getBytes());
        Field cur = G7ExecLauncher.class.getDeclaredField("CURRENT"); cur.setAccessible(true); cur.set(null, null);
        check("a log without g7-exit does not claim an outcome: says it cannot be confirmed and to re-inspect the pair", G7ExecLauncher.lastRunNote(files6).contains("확인할 수 없") && G7ExecLauncher.lastRunNote(files6).contains("다시 검사") && !G7ExecLauncher.lastRunNote(files6).contains("끊겼"));

        // a spawner failure leaves no run
        s = G7ExecLauncher.startWith(c -> { throw new IOException("boom 0x" + hex); }, ROMS, true, lib, ext, priv, Files.createTempDirectory("files3").toFile(), G7ExecLauncher.APPROVAL_PHRASE, SHA);
        check("spawn failure: not started, ECID masked in the message", !s.started && !s.message.contains(hex));

        System.out.println(fails == 0 ? "G7EXEC LAUNCHER OK (0 failed)" : "G7EXEC LAUNCHER FAILED (" + fails + ")");
        System.exit(fails == 0 ? 0 : 1);
    }
}
