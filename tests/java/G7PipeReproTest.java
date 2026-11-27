package dev.muns.hengeprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** G7PipeRepro's reduction ladder (runLadder/run) on a plain JVM against a FAKE libg7pipelinerepro.so (a shell script): classification of every row, result-line parsing, counts, partial file, per-row timeout kill. No Android classes (AssetManager is a stub), no real Vulkan, nothing real is started. */
public class G7PipeReproTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    /** The fake runner: "--list-rows" prints the ids; "--only ID" behaves as the case table says. */
    static File fake(Path dir, String[] ids, String cases) throws IOException {
        StringBuilder list = new StringBuilder();
        for (String id : ids) list.append("echo 'pipeline-reduce: id ").append(id).append("'\n");
        String sh = "#!/bin/sh\nif [ \"$3\" = \"--list-rows\" ]; then\n" + list + "exit 0\nfi\nID=\"$4\"\ncase \"$ID\" in\n" + cases + "esac\n";
        File f = new File(dir.toFile(), G7PipeRepro.LIB_NAME);
        Files.write(f.toPath(), sh.getBytes(StandardCharsets.UTF_8));
        if (!f.setExecutable(true)) throw new IOException("chmod");
        return f;
    }
    static String run(File lib, ByteArrayOutputStream buf, int[] code, File logDir) throws Exception {
        File bundle = new File(logDir, "pipefail_x"); bundle.mkdirs();
        code[0] = G7PipeRepro.runLadder(lib.getParentFile(), bundle, null, false, buf, logDir);
        return new String(buf.toByteArray(), StandardCharsets.UTF_8);
    }
    static boolean has(String s, String t) { return s.contains(t); }
    static String readAll(File f) throws IOException { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }
    static boolean alive(File pidFile) throws Exception {
        if (!pidFile.isFile()) return false;
        String pid = readAll(pidFile).trim(); File st = new File("/proc/" + pid + "/stat");
        for (int i = 0; i < 40; i++) { if (!st.exists() || readAll(st).contains(") Z ")) return false; Thread.sleep(50); }
        return true;
    }
    static void rm(File f) { File[] k = f.listFiles(); if (k != null) for (File c : k) rm(c); f.delete(); }

    public static void main(String[] a) throws Exception {
        Path root = Files.createTempDirectory("g7pipe-test");
        try {
            G7PipeRepro.rowTimeoutS = 2;
            // ---- one ladder with every kind of row
            Path d1 = Files.createDirectory(root.resolve("a")); File log1 = Files.createDirectory(root.resolve("log1")).toFile();
            String[] ids = {"R0", "R0V", "A1", "A2", "S1", "C1", "N1", "F:absent.bin", "F:nosummary.bin", "F:crash.bin", "T1", "L1", "L2", "L3"};
            String cases =
                "R0) echo 'noise before'; echo 'pipeline-reduce: row=R0 result=FAIL vk=-13(VK_ERROR_UNKNOWN) t=3ms'; exit 0;;\n" +
                "R0V) echo 'pipeline-reduce: row=R0V result=BUILT'; exit 0;;\n" +
                "A1) echo 'pipeline-reduce: row=A1 result=BUILT'; exit 0;;\n" +
                "A2) echo 'pipeline-reduce: row=A2 result=FAIL vk=-1(VK_ERROR_OUT_OF_HOST_MEMORY)'; exit 0;;\n" +
                "S1) kill -SEGV $$; sleep 5;;\n" +                     // dies by a signal, prints no row
                "C1) echo 'some text without a row'; exit 3;;\n" +     // non-zero exit, no row
                "N1) echo 'only chatter'; exit 0;;\n" +                // exit 0, no row, not an F: row
                "F:absent.bin) echo '1 offline file row(s) absent, not run'; exit 0;;\n" +
                "F:nosummary.bin) echo 'nothing'; exit 0;;\n" +
                "F:crash.bin) echo '1 offline file row(s) absent'; exit 7;;\n" +   // the summary text does not excuse a non-zero exit
                "T1) echo $$ > \"" + root.resolve("t1.pid") + "\"; exec sleep 30;;\n" +
                "L1) echo 'pipeline-reduce: row=L1 result=n/a (offline file absent)'; exit 0;;\n" +   // the library's own n/a row
                "L2) echo 'pipeline-reduce: row=L2 result=CRASH'; echo 'pipeline-reduce: row=L2x result=BUILT'; exit 0;;\n" +   // only this row's own line is read
                "L3) echo 'pipeline-reduce: row=L30 result=BUILT'; exit 0;;\n";                            // row=L30 is not row=L3: no row line for L3
            File lib = fake(d1, ids, cases);
            ByteArrayOutputStream buf = new ByteArrayOutputStream(); int[] code = new int[1];
            String o = run(lib, buf, code, log1);
            check("header counts the rows listed", has(o, "ladder: 14 rows (one process per row"));
            check("R0 FAIL vk=-13 line is copied verbatim (prefix before 'pipeline-reduce:' dropped)", has(o, "\npipeline-reduce: row=R0 result=FAIL vk=-13(VK_ERROR_UNKNOWN) t=3ms\n") && !has(o, "noise before"));
            check("BUILT row", has(o, "\npipeline-reduce: row=A1 result=BUILT\n"));
            check("FAIL with another VkResult is a fail row, not -13", has(o, "row=A2 result=FAIL vk=-1("));
            check("signal death without a row line is CRASH", has(o, "row=S1 result=CRASH exit=") && has(o, "(no row line; process ended)"));
            check("non-zero exit without a row line is CRASH with the exit code", has(o, "row=C1 result=CRASH exit=3 (no row line; process ended)"));
            check("exit 0 without a row line is NOROW, not CRASH", has(o, "row=N1 result=NOROW exit=0 (no row line although the process exited normally)"));
            check("F:* row, exit 0, 'offline file row(s) absent' is n/a and says it was NOT run", has(o, "row=F:absent.bin result=n/a exit=0 (offline file absent: the row was NOT run;"));
            check("F:* row, exit 0, without the summary is NOROW", has(o, "row=F:nosummary.bin result=NOROW exit=0"));
            check("F:* row with the summary but a non-zero exit is CRASH, not n/a", has(o, "row=F:crash.bin result=CRASH exit=7"));
            check("per-row timeout gives TIMEOUT exit=-2", has(o, "row=T1 result=TIMEOUT exit=-2 (no row line; process ended)"));
            check("a result=n/a line from the library counts as n/a (not as an error)", has(o, "row=L1 result=n/a (offline file absent)"));
            check("only the row's own line is taken (row=L2 CRASH line read; L2x ignored)", has(o, "row=L2 result=CRASH\n") && !has(o, "row=L2x"));
            check("row=L3 is not matched by row=L30 (id match includes the trailing space)", has(o, "row=L3 result=NOROW exit=0"));
            // counts: built R0V,A1 =2 ; fail R0,A2 =2 ; n/a F:absent, L1 =2 ; other: S1,C1,N1,F:nosummary,F:crash,T1,L2,L3 = 8
            check("final counts: rows=14 built=2 fail=2 n/a=2 other=8", has(o, "ladder: rows=14 built=2 fail=2 n/a=2 other=8\n"));
            check("status: R0 same -13 (reproduced), R0V BUILT, run complete", has(o, "ladder-status: R0=same -13 (reproduced); R0V (original FS + REAL VS) = BUILT; run=complete"));
            check("return 0 = reproduced", code[0] == 0);
            File[] parts = log1.listFiles((dd, n) -> n.endsWith("-ladder-partial.txt"));
            check("one partial file per ladder", parts != null && parts.length == 1);
            String p = parts == null || parts.length == 0 ? "" : readAll(parts[0]);
            check("partial file has the header and every row line as written (n/a, CRASH, NOROW too), but not the summary", p.startsWith("bundle=pipefail_x ladder: 14 rows") && has(p, "row=R0 result=FAIL vk=-13") && has(p, "row=F:absent.bin result=n/a") && has(p, "row=T1 result=TIMEOUT") && has(p, "row=N1 result=NOROW") && !has(p, "ladder: rows="));
            // the timed-out child is really gone (destroyForcibly)
            String pid = new String(Files.readAllBytes(root.resolve("t1.pid")), StandardCharsets.UTF_8).trim();
            check("the timed-out child process was killed", !new File("/proc/" + pid).exists() || readAll(new File("/proc/" + pid + "/stat")).contains(") Z "));

            // ---- R0 BUILT => not reproduced (1)
            Path d2 = Files.createDirectory(root.resolve("b")); File log2 = Files.createDirectory(root.resolve("log2")).toFile();
            File lib2 = fake(d2, new String[]{"R0", "R0V"}, "R0) echo 'pipeline-reduce: row=R0 result=BUILT'; exit 0;;\nR0V) echo 'pipeline-reduce: row=R0V result=FAIL vk=-13(X)'; exit 0;;\n");
            buf = new ByteArrayOutputStream(); String o2 = run(lib2, buf, code, log2);
            check("R0 BUILT: return 1, status 'BUILT (not reproduced)', R0V 'same -13'", code[0] == 1 && has(o2, "R0=BUILT (not reproduced); R0V (original FS + REAL VS) = same -13; run=complete"));
            // ---- R0 FAIL with another VkResult => 2 ; R0V other FAIL
            Path d3 = Files.createDirectory(root.resolve("c")); File log3 = Files.createDirectory(root.resolve("log3")).toFile();
            File lib3 = fake(d3, new String[]{"R0", "R0V"}, "R0) echo 'pipeline-reduce: row=R0 result=FAIL vk=-3(X)'; exit 0;;\nR0V) echo 'pipeline-reduce: row=R0V result=FAIL vk=-3(X)'; exit 0;;\n");
            buf = new ByteArrayOutputStream(); String o3 = run(lib3, buf, code, log3);
            check("R0 FAIL other VkResult: return 2, 'FAIL with a different VkResult', R0V 'FAIL (other VkResult)'", code[0] == 2 && has(o3, "R0=FAIL with a different VkResult;") && has(o3, "= FAIL (other VkResult);"));
            // ---- R0: the library prints result=CRASH itself => undecided (never "reproduced"); R0V stays "not run"
            Path d4 = Files.createDirectory(root.resolve("d")); File log4 = Files.createDirectory(root.resolve("log4")).toFile();
            File lib4 = fake(d4, new String[]{"R0"}, "R0) echo 'pipeline-reduce: row=R0 result=CRASH'; exit 0;;\n");
            buf = new ByteArrayOutputStream(); String o4 = run(lib4, buf, code, log4);
            check("R0 line with result=CRASH: its own status CRASH (return 2), R0V 'not run'", code[0] == 2 && has(o4, "R0=CRASH (the R0 process crashed; undecided, NOT reproduced);") && has(o4, "R0V (original FS + REAL VS) = not run"));
            // ---- R0 process itself crashes (no row line): current behaviour is the label "no R0 line" (r0 is only set from a parsed line), still return 2
            Path d4b = Files.createDirectory(root.resolve("d2")); File log4b = Files.createDirectory(root.resolve("log4b")).toFile();
            File lib4b = fake(d4b, new String[]{"R0"}, "R0) exit 9;;\n");
            buf = new ByteArrayOutputStream(); String o4b = run(lib4b, buf, code, log4b);
            check("R0 process crash: row CRASH exit=9, status CRASH (not 'no R0 line'), return 2", code[0] == 2 && has(o4b, "row=R0 result=CRASH exit=9") && has(o4b, "R0=CRASH (the R0 process crashed") && !has(o4b, "R0=no R0 line"));
            // ---- no R0 line at all
            Path d5 = Files.createDirectory(root.resolve("e")); File log5 = Files.createDirectory(root.resolve("log5")).toFile();
            File lib5 = fake(d5, new String[]{"A1"}, "A1) echo 'pipeline-reduce: row=A1 result=BUILT'; exit 0;;\n");
            buf = new ByteArrayOutputStream(); String o5 = run(lib5, buf, code, log5);
            check("no R0 row: 'no R0 line', return 2", code[0] == 2 && has(o5, "R0=no R0 line;"));
            // ---- --list-rows gives nothing
            Path d6 = Files.createDirectory(root.resolve("f")); File log6 = Files.createDirectory(root.resolve("log6")).toFile();
            File lib6 = fake(d6, new String[]{}, "");
            buf = new ByteArrayOutputStream(); String o6 = run(lib6, buf, code, log6);
            check("empty row list: message with the output, return 2, no partial file", code[0] == 2 && has(o6, "ladder: --list-rows gave no rows; output:") && log6.listFiles((dd, n) -> n.endsWith("-ladder-partial.txt")).length == 0);
            // ---- an exception mid-ladder keeps the rows done so far and returns -3 (incomplete)
            Path d7 = Files.createDirectory(root.resolve("g")); File log7 = Files.createDirectory(root.resolve("log7")).toFile();
            File lib7 = fake(d7, new String[]{"R0", "R0V"}, "R0) echo 'pipeline-reduce: row=R0 result=FAIL vk=-13(X)'; rm -f \"$0\"; exit 0;;\nR0V) echo 'pipeline-reduce: row=R0V result=BUILT'; exit 0;;\n");
            buf = new ByteArrayOutputStream(); String o7 = run(lib7, buf, code, log7);
            check("exe vanishing after R0: ABORTED note, R0 kept, return -3, status run=INCOMPLETE", code[0] == -3 && has(o7, "ladder: ABORTED:") && has(o7, "(the rows above were completed)") && has(o7, "row=R0 result=FAIL vk=-13") && has(o7, "run=INCOMPLETE"));


            // ---- A5: R0 timeout / R0 NOROW have their own statuses; a never-listed R0 stays 'no R0 line'
            Path d9 = Files.createDirectory(root.resolve("i")); File log9 = Files.createDirectory(root.resolve("log9")).toFile();
            File lib9 = fake(d9, new String[]{"R0", "R0V"}, "R0) exec sleep 30;;\nR0V) echo chatter; exit 0;;\n");
            buf = new ByteArrayOutputStream(); String o9 = run(lib9, buf, code, log9);
            check("R0 timeout: status TIMEOUT, R0V NOROW, return 2", code[0] == 2 && has(o9, "R0=TIMEOUT (undecided);") && has(o9, "R0V (original FS + REAL VS) = NOROW;"));
            check("class line distinguishes FAIL(-13), FAIL(other), BUILT, CRASH, TIMEOUT, NOROW, n/a", has(o, "ladder-classes: FAIL(-13)=1 FAIL(other VkResult)=1 BUILT=2 CRASH=4 TIMEOUT=1 NOROW=3 n/a=2 other=0\n"));

            // ---- A4: the whole process tree is killed on timeout and on cancel, and the kill is VERIFIED (the grandchild is polled until gone)
            Path d10 = Files.createDirectory(root.resolve("j")); File log10 = Files.createDirectory(root.resolve("log10")).toFile();
            File gpid = root.resolve("grand.pid").toFile();
            File lib10 = fake(d10, new String[]{"R0", "T2"}, "R0) echo 'pipeline-reduce: row=R0 result=BUILT'; exit 0;;\nT2) sleep 30 & echo $! > \"" + gpid + "\"; wait;;\n");
            buf = new ByteArrayOutputStream(); String o10 = run(lib10, buf, code, log10);
            check("timeout row: the process tree is gone (grandchild killed) and the row says so", has(o10, "row=T2 result=TIMEOUT") && has(o10, "kill: process tree gone (child + ") && !alive(gpid));
            // cancel mid-row
            Path d11 = Files.createDirectory(root.resolve("k")); File log11 = Files.createDirectory(root.resolve("log11")).toFile();
            File gpid2 = root.resolve("grand2.pid").toFile();
            File lib11 = fake(d11, new String[]{"R0", "R0V", "R1"}, "R0) echo 'pipeline-reduce: row=R0 result=FAIL vk=-13(X)'; exit 0;;\nR0V) sleep 30 & echo $! > \"" + gpid2 + "\"; wait;;\nR1) echo 'pipeline-reduce: row=R1 result=BUILT'; exit 0;;\n");
            G7PipeRepro.Cancel cn = new G7PipeRepro.Cancel();
            new Thread(() -> { try { for (int i = 0; i < 100 && !gpid2.isFile(); i++) Thread.sleep(50); Thread.sleep(200); } catch (InterruptedException ignored) {} cn.cancel("test cancel"); }).start();
            buf = new ByteArrayOutputStream(); File bnd = new File(log11, "pipefail_x"); bnd.mkdirs();
            long t0 = System.currentTimeMillis();
            code[0] = G7PipeRepro.runLadder(lib11.getParentFile(), bnd, null, false, buf, log11, new G7PipeRepro.Options().cancel(cn));
            String o11 = new String(buf.toByteArray(), StandardCharsets.UTF_8);
            check("cancel: returns -5 quickly (not after the 2 s row timeout of the stuck row)", code[0] == -5 && System.currentTimeMillis() - t0 < 1900);
            check("cancel: finished rows kept, cancelled row NOT recorded, later row not run, status CANCELLED, kill verified", has(o11, "row=R0 result=FAIL vk=-13") && !has(o11, "row=R0V result") && !has(o11, "row=R1 ") && has(o11, "ladder: CANCELLED during row R0V (test cancel)") && has(o11, "kill check: process tree gone") && has(o11, "run=CANCELLED"));
            check("cancel: the grandchild process is really gone", !alive(gpid2));
            // a Cancel already set before the run: nothing is started
            G7PipeRepro.Cancel pre = new G7PipeRepro.Cancel(); pre.cancel("pre");
            buf = new ByteArrayOutputStream();
            code[0] = G7PipeRepro.runLadder(lib11.getParentFile(), bnd, null, false, buf, log11, new G7PipeRepro.Options().cancel(pre));
            check("cancel before start: -5, no row run", code[0] == -5 && !new String(buf.toByteArray(), StandardCharsets.UTF_8).contains("row="));

            // ---- run(): the whole path (header, verdict text, result file), reduce=true, matrix=false
            Path d8 = Files.createDirectory(root.resolve("h")); File log8 = Files.createDirectory(root.resolve("log8")).toFile();
            File lib8 = fake(d8, new String[]{"R0"}, "R0) echo 'pipeline-reduce: row=R0 result=FAIL vk=-13(X)'; exit 0;;\n");
            File bundle = new File(root.toFile(), "run-1/pipefail_h"); bundle.mkdirs();
            G7PipeRepro.Result r = G7PipeRepro.run(d8.toFile(), bundle, false, true, new android.content.res.AssetManager(), log8);
            check("run(): started, exit 0, header and verdict, result file saved", r.started && r.exitCode == 0 && has(r.text, "g7-pipelinerepro: bundle=pipefail_h run=run-1 matrix=false reduce=true red_assets=0") && has(r.text, "g7-pipelinerepro: exit=0 -> 재현됨") && r.file != null && r.file.isFile());
            G7PipeRepro.Result r2 = G7PipeRepro.run(d8.toFile().toPath().resolve("none").toFile(), bundle, false, true, null, log8);
            check("run() without the packaged exe: not started, nothing created", !r2.started && r2.file == null);
            // listBundles: newest run first, only pipefail_* with a state file
            File dump = new File(root.toFile(), "dump"); for (String s : new String[]{"run-1/pipefail_a", "run-2/pipefail_b", "run-2/other", "run-2/pipefail_nostate"}) new File(dump, s).mkdirs();
            Files.write(new File(dump, "run-1/pipefail_a/state").toPath(), new byte[1]); Files.write(new File(dump, "run-2/pipefail_b/state").toPath(), new byte[1]);
            List<File> bl = G7PipeRepro.listBundles(dump);
            check("listBundles: newest run first, state file required", bl.size() == 2 && bl.get(0).getName().equals("pipefail_b") && bl.get(1).getName().equals("pipefail_a"));
        } finally { rm(root.toFile()); }
        System.out.println(fails == 0 ? "G7PIPEREPRO OK" : "G7PIPEREPRO FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
