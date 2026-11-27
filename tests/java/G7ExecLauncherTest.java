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
    static G7ExecLauncher.Spawner spawner0(Process p) { return c -> { spawned++; lastCmd = c; return p; }; }
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
        G7ExecLauncher.Start s = G7ExecLauncher.start(lib, ext, priv, files, true);
        check("the built-in IPSW SHA-256 is 64 lower-case hex digits and is the documented macOS 15.6.1 (24G90) reference", G7ExecLauncher.isSha256Hex(G7ExecLauncher.IPSW_SHA256) && G7ExecLauncher.IPSW_SHA256.equals(G7ExecLauncher.IPSW_SHA256.toLowerCase()) && G7ExecLauncher.IPSW_SHA256.equals("3d87686b691ac765eb6a6b3082b2334e2af9710096a00432dd519af89ff2ea78") && G7ExecLauncher.IPSW_NAME.contains("15.6.1_24G90"));
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
        s = G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, false, lib, ext, priv, files, true, SHA);
        check("switch off: refused, spawner not reached, nothing created", !s.started && spawned == 0 && !new File(files, G7ExecLauncher.WORK_NAME).exists());
        // each missing condition (switch ON) refuses before the spawner
        G7ExecLauncher.Start nc = G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files, false, SHA);
        check("no consent refused (nothing typed any more: an unchecked box is a refusal), spawner not reached", !nc.started && spawned == 0 && nc.message.contains("동의"));
        for (String bad : new String[] {null, "", "abc", SHA.substring(1), SHA + "0", SHA.replace('0', 'g'), SHA.replace('0', ' ')})
            check("SHA-256 '" + bad + "' refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files, true, bad).started && spawned == 0);
        check("no ROM refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), (e, p) -> null, true, lib, ext, priv, files, true, SHA).started && spawned == 0);
        check("no binary refused", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, files, ext, priv, files, true, SHA).started && spawned == 0);
        new File(priv, "vm_ecid.txt").delete();
        check("no ECID refused (this screen never makes one)", !G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files, true, SHA).started && spawned == 0);
        touch(priv, "vm_ecid.txt", ("ecid=0x" + hex + "\n").getBytes());

        // a start (only possible here because the test turns the switch on for a fake process)
        FakeProcess fp = new FakeProcess("g7: step\nleak 0x" + hex + " and " + hex + " and " + hex.toUpperCase() + "\ng7-result: outcome=FAILED\n", 2, false);
        s = G7ExecLauncher.startWith(spawner(fp), ROMS, true, lib, ext, priv, files, true, SHA.toUpperCase());
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
        s = G7ExecLauncher.startWith(spawner(hp), ROMS, true, lib, ext, priv, files, true, SHA);
        s.run.cancel(); s.run.cancel();
        check("cancel asks the process to stop (destroy) and is safe twice", hp.destroyed && !hp.forced);
        for (int i = 0; i < 100 && !s.run.finished(); i++) Thread.sleep(20);
        check("after the stop the run ends with the signal exit and the pair text says in_progress", s.run.finished() && s.run.exitCode() == 143 && G7ExecLauncher.meaning(s.run.exitCode()).contains("in_progress"));

        // output bound
        StringBuilder big = new StringBuilder(); for (int i = 0; i < 280000; i++) big.append("0123456789abcdef");   // > MAX_LOG_BYTES (4 MiB), one long line: cut into lines of at most MAX_LINE_BYTES
        File files2 = Files.createTempDirectory("files2").toFile();
        s = G7ExecLauncher.startWith(spawner(new FakeProcess(big.toString(), 2, false)), ROMS, true, lib, ext, priv, files2, true, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        check("the log is bounded", G7ExecLauncher.newestLog(files2).length() <= G7ExecLauncher.MAX_LOG_BYTES + 3 * G7ExecLauncher.TAIL_BYTES && s.run.tail().length() <= G7ExecLauncher.TAIL_BYTES + 64);

        // the final result survives a full log; every run has its own file, nothing is appended to or overwritten
        File files4 = Files.createTempDirectory("files4").toFile();
        s = G7ExecLauncher.startWith(spawner(new FakeProcess(big + "\ng7-result: outcome=FAILED last_step=7 pair=in_progress\n", 2, false)), ROMS, true, lib, ext, priv, files4, true, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        String full = new String(Files.readAllBytes(G7ExecLauncher.newestLog(files4).toPath()), StandardCharsets.UTF_8);
        check("log limit reached: the marker, the last g7-result line and the exit code are still in the file", full.contains("log limit reached") && full.contains("g7-result: outcome=FAILED last_step=7") && full.contains("g7-exit: 2"));
        check("the file starts with g7-start", full.startsWith("g7-start: "));
        check("g7-process follows with app pid, runner pid, and both start times (-1 when unknown)", full.contains("g7-process: app_pid=") && full.contains(" runner_pid=") && full.contains(" app_start_ms=") && full.contains(" run_start_ms="));
        String first = s.run.logName();
        Thread.sleep(5);
        s = G7ExecLauncher.startWith(spawner(new FakeProcess("second\n", 2, false)), ROMS, true, lib, ext, priv, files4, true, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        File[] logs = files4.listFiles((d, n) -> n.startsWith(G7ExecLauncher.LOG_PREFIX));
        check("a second run writes a second file; the first one is untouched", logs.length == 2 && !s.run.logName().equals(first) && new File(files4, first).length() > G7ExecLauncher.MAX_LOG_BYTES / 2);
        check("readLogTail reads the newest run", G7ExecLauncher.readLogTail(files4, 1000).contains("second"));

        // the run state is process-wide, not the screen's: a "recreated" screen gets the same run, can cancel it, and a second start is refused while it runs
        File files5 = Files.createTempDirectory("files5").toFile();
        FakeProcess live = new FakeProcess("", 143, true);
        s = G7ExecLauncher.startWith(spawner(live), ROMS, true, lib, ext, priv, files5, true, SHA);
        check("current() returns the started run (what a recreated screen would read)", s.started && G7ExecLauncher.current() == s.run && !G7ExecLauncher.current().finished());
        int before = spawned;
        G7ExecLauncher.Start again = G7ExecLauncher.startWith(spawner(new FakeProcess("", 0, false)), ROMS, true, lib, ext, priv, files5, true, SHA);
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

        // the USB diagnostic flood is thinned (first lines kept, the rest counted); every other line and the result stay
        G7ExecLauncher.DiagFilter df = new G7ExecLauncher.DiagFilter();
        int outLines = 0, summaries = 0, keptDiag = 0;
        for (int i = 0; i < 12000; i++) { String o = df.accept("bdif usb diag: REG_BUSY read " + i); if (!o.isEmpty()) { outLines++; if (o.startsWith("[usb diag:")) summaries++; else keptDiag++; } }
        String fin = df.finish(); if (!fin.isEmpty()) summaries++;
        check("DiagFilter keeps the first " + G7ExecLauncher.DiagFilter.KEEP_FIRST + " diag lines and summarises the rest (first, every 5000th, and the end)", keptDiag == G7ExecLauncher.DiagFilter.KEEP_FIRST && summaries == 4 && fin.contains("11700 line(s) not kept") && fin.contains("REG_BUSY read 11999"));
        check("DiagFilter never drops a non-diag line", new G7ExecLauncher.DiagFilter().accept("Uploading:  47.2%").equals("Uploading:  47.2%\n") && df.accept("g7-result: outcome=X").equals("g7-result: outcome=X\n"));
        StringBuilder flood = new StringBuilder("g7: start\n");
        for (int i = 0; i < 2000; i++) flood.append("bdif usb diag: REG_QSEL write -> ").append(i % 2).append('\n');
        flood.append("Uploading:  47.2%\nleak ").append(hex).append("\ng7-result: outcome=FAILED last_step=7\n");
        File files7 = Files.createTempDirectory("files7").toFile();
        s = G7ExecLauncher.startWith(spawner(new FakeProcess(flood.toString(), 2, false)), ROMS, true, lib, ext, priv, files7, true, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        String fl = new String(Files.readAllBytes(G7ExecLauncher.newestLog(files7).toPath()), StandardCharsets.UTF_8);
        int diagInFile = 0; for (String ln : fl.split("\n")) if (ln.startsWith("bdif usb diag:")) diagInFile++;
        check("flood: only " + G7ExecLauncher.DiagFilter.KEEP_FIRST + " diag lines in the file, a summary with the count, the other lines and the result kept", diagInFile == 300 && fl.contains("[usb diag: 1700 line(s) not kept") && fl.contains("Uploading:  47.2%") && fl.contains("g7-result: outcome=FAILED last_step=7") && fl.contains("g7-exit: 2") && !fl.toLowerCase().contains(hex));
        check("flood: the screen tail keeps the useful lines (not only diag lines)", s.run.tail().contains("Uploading:  47.2%") && s.run.tail().contains("g7-result:"));
        String sm = G7ExecLauncher.summary(files7);
        check("summary of a finished run: state with the meaning of the exit code, log name, the g7-result line and the g7-exit line", sm.contains("상태: 종료") && sm.contains(s.run.logName()) && sm.contains("g7-result: outcome=FAILED last_step=7") && sm.contains("g7-exit: 2"));
        File pv = Files.createTempDirectory("pv").toFile();
        check("summary shows no pair state file", G7ExecLauncher.summary(files7, pv).contains("상태 파일(g7_state.txt) 없음"));
        touch(pv, "g7_state.txt", "state=in_progress\necid_tag=eb5a591ec138b9b0\n".getBytes());
        touch(files7, G7ExecLauncher.LOG_PREFIX + "9999999999999" + G7ExecLauncher.LOG_SUFFIX, "g7-start: 1\nG7RUN RESULT: FAILED stage=4: why\ng7-result: outcome=FAILED\n".getBytes());
        String sp = G7ExecLauncher.summary(files7, pv);
        check("summary shows the pair state and the G7RUN RESULT line (read only)", sp.contains("설치 쌍: in_progress") && !sp.contains("ecid_tag") && !sp.contains("eb5a591ec138b9b0") && sp.contains("G7RUN RESULT: FAILED stage=4: why") && new String(Files.readAllBytes(new File(pv, "g7_state.txt").toPath())).equals("state=in_progress\necid_tag=eb5a591ec138b9b0\n"));
        touch(pv, "g7_state.txt", "state=something_else\necid_tag=eb5a591ec138b9b0\n".getBytes());
        check("an unknown state value is not shown (only 'cannot tell'), nothing else of the file either", G7ExecLauncher.summary(files7, pv).contains("설치 쌍: 상태 확인 불가") && !G7ExecLauncher.summary(files7, pv).contains("something_else") && !G7ExecLauncher.summary(files7, pv).contains("eb5a"));
        touch(pv, "g7_state.txt", "garbage\n".getBytes());
        check("a state file that does not start with state= is 'cannot tell'", G7ExecLauncher.summary(files7, pv).contains("설치 쌍: 상태 확인 불가"));
        touch(pv, "g7_state.txt", "state=done\n".getBytes());
        check("state=done is shown", G7ExecLauncher.summary(files7, pv).contains("설치 쌍: done"));
        // the runner process check: found / not found / cannot tell (a fake /proc)
        File proc = Files.createTempDirectory("proc").toFile();
        new File(proc, "100").mkdir(); touch(new File(proc, "100"), "cmdline", "dev.muns.hengeprobe\0".getBytes());
        File rp = new File(proc, "200"); rp.mkdir(); touch(rp, "cmdline", ("/data/app/x/lib/arm64/" + G7ExecLauncher.LIB_NAME + "\0--dir\0/x\0").getBytes());
        new File(proc, "abc").mkdir(); touch(proc, "cpuinfo", new byte[] {1});
        String found = G7ExecLauncher.runnerCheck(proc, 100, files7);
        check("runner check: a process whose program is the runner is found (pid shown)", found.contains("발견(pid 200,"));
        new File(rp, "cmdline").delete(); rp.delete();
        File ep = new File(proc, "300"); ep.mkdir(); touch(ep, "cmdline", "x\0".getBytes());
        Files.createSymbolicLink(new File(ep, "exe").toPath(), new File("/data/app/y/lib/" + G7ExecLauncher.LIB_NAME).toPath());
        check("runner check: found through the exe link too", G7ExecLauncher.runnerCheck(proc, 100, files7).contains("발견(pid 300,"));
        new File(ep, "exe").delete(); new File(ep, "cmdline").delete(); ep.delete();
        File bp = new File(proc, "400"); bp.mkdir(); touch(bp, "cmdline", ("/x/" + G7ExecLauncher.LIB_NAME + ".bak\0").getBytes());
        String nf = G7ExecLauncher.runnerCheck(proc, 100, files7);
        check("runner check: a similar name is not the runner; 'not found' says it is no proof that it ended", nf.contains("미발견") && nf.contains("종료의 증명은 아님") && !nf.contains("발견(pid"));
        check("runner check: if this app's own process is not in the list the list is not trusted", G7ExecLauncher.runnerCheck(proc, 999, files7).contains("확인 불가"));
        check("runner check: an unreadable /proc is 'cannot tell'", G7ExecLauncher.runnerCheck(new File(proc, "nope"), 100, files7).contains("확인 불가") && G7ExecLauncher.runnerCheck(null, 100, files7).contains("확인 불가"));
        check("runner check changed nothing in the fake /proc", new File(bp, "cmdline").isFile() && new File(proc, "100").isDirectory());
        cur.set(null, null);   // a screen opened again after the app process was recreated: no run of this process
        check("summary with no log at all says there is no record", G7ExecLauncher.summary(Files.createTempDirectory("empty").toFile()).contains("실행 기록 없음"));
        check("summary of an interrupted log (g7-start, no g7-exit) does not claim an outcome", G7ExecLauncher.summary(files6).contains("종료 기록(g7-exit)이 없음") && G7ExecLauncher.summary(files6).contains("확인되지 않음") && G7ExecLauncher.summary(files6).contains("로그에 아직 없음"));
        // only run logs of the app can be handed out (by plain name)
        File okLog = G7ExecLauncher.newestLog(files7);
        check("logFile hands out a run log by its plain name", G7ExecLauncher.logFile(files7, okLog.getName()) != null);
        File lg = new File(files7, "g7_lagfx_1795613674355.txt"); Files.write(lg.toPath(), "x".getBytes());
        check("logFile hands out the lagfx log by its plain name", G7ExecLauncher.logFile(files7, lg.getName()) != null);
        for (String badName : new String[] {"g7_lagfx_1.txt", "g7_lagfx_1795613674355.txt.bak", "lagfx.log", "../g7_lagfx_1795613674355.txt"})
            check("logFile refuses '" + badName + "'", G7ExecLauncher.logFile(files7, badName) == null);
        for (String badName : new String[] {null, "", "../x", "g7_run_1.txt", okLog.getName() + "/../x", "g7_run_0000000000001.txt", "vm_ecid.txt", "g7_run_" + "0000000000001" + ".txt.bak"})
            check("logFile refuses '" + badName + "'", G7ExecLauncher.logFile(files7, badName) == null);


        // --- end detection is the process, not the pipe; SIGKILL is confirmed; states; ECID in decimal ---
        // 1) the process ends but a child keeps the output pipe open: the run must still finish, the log says incomplete, g7-exit is written
        G7ExecLauncher.drainMillis = 200;
        final java.util.concurrent.atomic.AtomicBoolean release = new java.util.concurrent.atomic.AtomicBoolean(false);
        Process holder = new Process() {
            @Override public OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
            @Override public InputStream getInputStream() { return new InputStream() { boolean first = true;
                @Override public int read() { return -1; }
                @Override public int read(byte[] b, int o, int l) { if (first) { first = false; byte[] t = "last words\n".getBytes(StandardCharsets.UTF_8); System.arraycopy(t, 0, b, o, t.length); return t.length; }
                    while (!release.get()) { try { Thread.sleep(5); } catch (InterruptedException e) { return -1; } } return -1; } }; }
            @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
            @Override public int waitFor() { return 7; }
            @Override public boolean waitFor(long t, TimeUnit u) { return true; }
            @Override public int exitValue() { return 7; }
            @Override public void destroy() { }
        };
        File files8 = Files.createTempDirectory("files8").toFile();
        s = G7ExecLauncher.startWith(spawner0(holder), ROMS, true, lib, ext, priv, files8, true, SHA);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        check("pipe held open after the process ended: the run still finishes with the process exit code", s.run.finished() && s.run.exitCode() == 7);
        String l8 = G7ExecLauncher.readLogTail(files8, 100000);
        check("...the last output is kept, the log says incomplete, and g7-exit is written", l8.contains("last words") && l8.contains("g7-log: incomplete") && l8.contains("g7-exit: 7") && s.run.logIncomplete());
        check("...the screen says the log collection is incomplete (not a normal end)", G7ExecLauncher.summary(files8).contains("로그 수집 미완료"));
        release.set(true);

        // 2) SIGKILL is sent after the grace and is confirmed; a process that survives it is reported and gets no g7-exit
        G7ExecLauncher.forceConfirmSeconds = 1;
        final java.util.concurrent.atomic.AtomicBoolean killed = new java.util.concurrent.atomic.AtomicBoolean(false);
        final java.util.concurrent.atomic.AtomicBoolean unkillable = new java.util.concurrent.atomic.AtomicBoolean(true);
        Process stubborn = new Process() {
            @Override public OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
            @Override public InputStream getInputStream() { return new InputStream() { @Override public int read() { while (true) { try { Thread.sleep(50); } catch (InterruptedException e) { return -1; } } } @Override public int read(byte[] b, int o, int l) { return read(); } }; }
            @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
            @Override public int waitFor() { while (!(killed.get() && !unkillable.get())) { try { Thread.sleep(20); } catch (InterruptedException e) { return -1; } } return 137; }
            @Override public boolean waitFor(long t, TimeUnit u) { try { u.sleep(Math.min(t, 1)); } catch (InterruptedException e) { return false; } return killed.get() && !unkillable.get(); }
            @Override public int exitValue() { return 137; }
            @Override public void destroy() { }
            @Override public Process destroyForcibly() { killed.set(true); return this; }
        };
        File files9 = Files.createTempDirectory("files9").toFile();
        G7ExecLauncher.CANCEL_GRACE_SECONDS_FOR_TEST = 1;
        s = G7ExecLauncher.startWith(spawner0(stubborn), ROMS, true, lib, ext, priv, files9, true, SHA);
        s.run.cancel();
        for (int i = 0; i < 300 && !s.run.killUnconfirmed(); i++) Thread.sleep(20);
        check("a process that survives SIGKILL is reported as not confirmed ended; the run is NOT finished", killed.get() && s.run.killRequested() && s.run.killCalled() && !s.run.killConfirmed() && s.run.killUnconfirmed() && !s.run.finished());
        String l9 = G7ExecLauncher.readLogTail(files9, 100000);
        check("...the log has the request, the call that returned and the unconfirmed end, in that order, no 'sent'/'confirmed' wording and no g7-exit", l9.contains("g7-kill: SIGKILL requested after") && l9.contains("g7-kill: destroyForcibly returned") && l9.contains("g7-kill: process exit unconfirmed")
              && l9.indexOf("SIGKILL requested") < l9.indexOf("destroyForcibly returned") && l9.indexOf("destroyForcibly returned") < l9.indexOf("process exit unconfirmed") && !l9.contains("g7-kill-unconfirmed") && !l9.contains("SIGKILL sent") && !l9.contains("exit confirmed") && !l9.contains("g7-exit:"));
        check("...the screen tells the user what to do", G7ExecLauncher.summary(files9).contains("종료가 확인되지 않음") && G7ExecLauncher.summary(files9).contains("재시작"));

        unkillable.set(false);   // the process finally goes away
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        check("when the process finally ends the run finishes with the kill exit code", s.run.finished() && s.run.exitCode() == 137 && G7ExecLauncher.readLogTail(files9, 100000).contains("g7-exit: 137"));

        // 2b) integ7b: names match actions. Confirmed path, the call that throws, and an injected delay inside destroyForcibly() with a poller that reads the log the moment each flag turns true (log line must already be there).
        for (int variant = 0; variant < 3; variant++) {   // 0: dies promptly after the kill, 1: destroyForcibly throws, 2: destroyForcibly is slow (injected delay) and the process survives
            final int vv = variant;
            final java.util.concurrent.atomic.AtomicBoolean kreq = new java.util.concurrent.atomic.AtomicBoolean(false), dead = new java.util.concurrent.atomic.AtomicBoolean(false);
            Process pr = new Process() {
                @Override public OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
                @Override public InputStream getInputStream() { return new InputStream() { @Override public int read() { while (!dead.get()) { try { Thread.sleep(20); } catch (InterruptedException e) { return -1; } } return -1; } @Override public int read(byte[] b, int o, int l) { return read(); } }; }
                @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
                @Override public int waitFor() { while (!dead.get()) { try { Thread.sleep(20); } catch (InterruptedException e) { return -1; } } return 137; }
                @Override public boolean waitFor(long t, TimeUnit u) { try { u.sleep(Math.min(t, 1)); } catch (InterruptedException e) { return false; } return dead.get(); }
                @Override public int exitValue() { return 137; }
                @Override public void destroy() { }
                @Override public Process destroyForcibly() {
                    kreq.set(true);
                    if (vv == 1) throw new IllegalStateException("injected");
                    if (vv == 2) { try { Thread.sleep(400); } catch (InterruptedException ignored) { } } else dead.set(true);
                    return this;
                }
            };
            File filesK = Files.createTempDirectory("filesK" + variant).toFile();
            G7ExecLauncher.CANCEL_GRACE_SECONDS_FOR_TEST = 1; G7ExecLauncher.forceConfirmSeconds = 1;
            final G7ExecLauncher.Start ks = G7ExecLauncher.startWith(spawner0(pr), ROMS, true, lib, ext, priv, filesK, true, SHA);
            if (!ks.started) System.out.println("INFO kill variant " + variant + " did not start: " + ks.message);
            final boolean[] order = {true, true, true, true}; final java.util.concurrent.atomic.AtomicBoolean stopPoll = new java.util.concurrent.atomic.AtomicBoolean(false);
            Thread poll = new Thread(() -> {
                boolean r = false, c = false, ok = false, un = false;
                while (!stopPoll.get()) {
                    if (!r && ks.run.killRequested()) { r = true; String l = G7ExecLauncher.readLogTail(filesK, 100000); order[0] = l.contains("g7-kill: SIGKILL requested"); }
                    if (!c && ks.run.killCalled()) { c = true; String l = G7ExecLauncher.readLogTail(filesK, 100000); order[1] = l.contains("g7-kill: destroyForcibly returned"); }
                    if (!ok && ks.run.killConfirmed()) { ok = true; String l = G7ExecLauncher.readLogTail(filesK, 100000); order[2] = l.contains("g7-kill: process exit confirmed"); }
                    if (!un && ks.run.killUnconfirmed()) { un = true; String l = G7ExecLauncher.readLogTail(filesK, 100000); order[3] = l.contains("g7-kill: process exit unconfirmed"); }
                    try { Thread.sleep(1); } catch (InterruptedException e) { return; }
                }
            });
            poll.start();
            ks.run.cancel();
            for (int i = 0; i < 400 && !(ks.run.killConfirmed() || ks.run.killUnconfirmed()); i++) Thread.sleep(10);
            if (variant == 2) { Thread.sleep(100); dead.set(true); }   // let the stubborn one go so the run ends
            for (int i = 0; i < 200 && !ks.run.finished(); i++) Thread.sleep(20);
            stopPoll.set(true); poll.join();
            String lk = G7ExecLauncher.readLogTail(filesK, 100000);
            if (variant == 0) check("kill variant 0 (process ends after the kill): requested, call returned, exit confirmed; not unconfirmed; each line was already in the log when its flag was seen", ks.run.killRequested() && ks.run.killCalled() && ks.run.killConfirmed() && !ks.run.killUnconfirmed() && order[0] && order[1] && order[2] && lk.contains("g7-kill: process exit confirmed") && !lk.contains("exit unconfirmed"));
            if (variant == 1) check("kill variant 1 (destroyForcibly throws): requested is set, killCalled is NOT, and the log says the call threw instead of 'returned'/'sent'", ks.run.killRequested() && !ks.run.killCalled() && lk.contains("g7-kill: destroyForcibly threw IllegalStateException") && !lk.contains("destroyForcibly returned") && order[0]);
            if (variant == 1) { dead.set(true); for (int i = 0; i < 200 && !ks.run.finished(); i++) Thread.sleep(20); }   // release it so the next variant may start (one live run at a time)
            if (variant == 2) check("kill variant 2 (slow destroyForcibly, process survives the wait): flags in order, unconfirmed (never 'confirmed'), lines present when each flag was observed", ks.run.killRequested() && ks.run.killCalled() && !ks.run.killConfirmed() && ks.run.killUnconfirmed() && order[0] && order[1] && order[3] && lk.indexOf("SIGKILL requested") < lk.indexOf("destroyForcibly returned") && lk.indexOf("destroyForcibly returned") < lk.indexOf("exit unconfirmed"));
        }

        // 3) process state letter from /proc/<pid>/status (read only; anything else is '?')
        File proc2 = Files.createTempDirectory("proc2").toFile();
        for (String[] st : new String[][] {{"11", "R (running)"}, {"12", "S (sleeping)"}, {"13", "D (disk sleep)"}, {"14", "Z (zombie)"}, {"15", "weird"}}) {
            File d = new File(proc2, st[0]); d.mkdirs(); Files.write(new File(d, "status").toPath(), ("Name:\tx\nState:\t" + st[1] + "\nUid:\t1\n").getBytes());
        }
        check("state letters R S D Z are read", G7ExecLauncher.procState(proc2, 11) == 'R' && G7ExecLauncher.procState(proc2, 12) == 'S' && G7ExecLauncher.procState(proc2, 13) == 'D' && G7ExecLauncher.procState(proc2, 14) == 'Z');
        check("an unknown letter or a missing file is '?'", G7ExecLauncher.procState(proc2, 15) == '?' && G7ExecLauncher.procState(proc2, 99) == '?' && G7ExecLauncher.procState(null, 1) == '?');
        check("the state text says what each state means (Z = ended but not collected, D = kernel wait)", G7ExecLauncher.describeState('Z').contains("회수") && G7ExecLauncher.describeState('D').contains("커널") && G7ExecLauncher.describeState('?').contains("확인 불가"));

        // 4) ECID in decimal, hex (any case, 0x, padded) is masked before it is stored
        String dec = Long.toString(ecid);
        String mixed = "ECID: " + dec + " UniqueChipID " + dec + " 0x" + hex + " " + hex.toUpperCase() + " 0000FE00-" + hex.toUpperCase() + " " + hex.substring(0, 5) + hex.substring(5).toUpperCase();
        String masked = G7ExecLauncher.mask(mixed, ecid);
        check("mask: decimal, 0x hex, upper/mixed case and padded forms are all replaced", !masked.contains(dec) && !masked.toLowerCase().contains(hex) && masked.contains("(ECID)"));
        File files10 = Files.createTempDirectory("files10").toFile();
        s = G7ExecLauncher.startWith(spawner(new FakeProcess("ECID: " + dec + "\nUniqueChipID " + dec + "\ng7-result: x\n", 0, false)), ROMS, true, lib, ext, priv, files10, true, SHA);
        for (int i = 0; i < 100 && !s.run.finished(); i++) Thread.sleep(20);
        check("log and tail contain no decimal ECID", !G7ExecLauncher.readLogTail(files10, 100000).contains(dec) && !s.run.tail().contains(dec));
        G7ExecLauncher.drainMillis = 3000;


        // 5) an interrupt is not an end: the watcher waits again; a pipe whose close() never returns does not delay the published end
        final java.util.concurrent.atomic.AtomicBoolean ended = new java.util.concurrent.atomic.AtomicBoolean(false);
        final java.util.concurrent.atomic.AtomicInteger waits = new java.util.concurrent.atomic.AtomicInteger();
        Process intr = new Process() {
            @Override public OutputStream getOutputStream() { return new java.io.ByteArrayOutputStream(); }
            @Override public InputStream getInputStream() { return new InputStream() {
                @Override public int read() { return -1; }
                @Override public int read(byte[] b, int o, int l) { while (true) { try { Thread.sleep(50); } catch (InterruptedException e) { /* keep holding the pipe */ } } }
                @Override public void close() { while (true) { try { Thread.sleep(50); } catch (InterruptedException e) { /* never returns */ } } } }; }
            @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
            @Override public int waitFor() throws InterruptedException { if (waits.getAndIncrement() == 0) throw new InterruptedException("simulated"); while (!ended.get()) Thread.sleep(10); return 9; }
            @Override public boolean isAlive() { return !ended.get(); }
            @Override public int exitValue() { if (!ended.get()) throw new IllegalThreadStateException(); return 9; }
            @Override public void destroy() { }
        };
        File files11 = Files.createTempDirectory("files11").toFile();
        G7ExecLauncher.drainMillis = 100;
        s = G7ExecLauncher.startWith(spawner0(intr), ROMS, true, lib, ext, priv, files11, true, SHA);
        Thread.sleep(300);
        check("after an interrupted wait the process is still alive: no end is recorded", !s.run.finished() && !G7ExecLauncher.readLogTail(files11, 100000).contains("g7-exit"));
        ended.set(true);
        for (int i = 0; i < 200 && !s.run.finished(); i++) Thread.sleep(20);
        check("the real end (exit 9) is recorded once the process ends, although the pipe is held and its close() never returns", s.run.finished() && s.run.exitCode() == 9 && G7ExecLauncher.readLogTail(files11, 100000).contains("g7-exit: 9") && s.run.logIncomplete());
        G7ExecLauncher.drainMillis = 3000;

        // a spawner failure leaves no run
        s = G7ExecLauncher.startWith(c -> { throw new IOException("boom 0x" + hex); }, ROMS, true, lib, ext, priv, Files.createTempDirectory("files3").toFile(), true, SHA);
        check("spawn failure: not started, ECID masked in the message", !s.started && !s.message.contains(hex));

        System.out.println(fails == 0 ? "G7EXEC LAUNCHER OK (0 failed)" : "G7EXEC LAUNCHER FAILED (" + fails + ")");
        System.exit(fails == 0 ? 0 : 1);
    }
}
