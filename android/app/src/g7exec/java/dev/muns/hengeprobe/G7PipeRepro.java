package dev.muns.hengeprobe;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * [G7-EXEC] GPU pipeline reproduction WITHOUT a virtual machine. A failed vkCreateGraphicsPipelines leaves a bundle folder (pipefail_<hash>, written by libapplegfx-vulkan inside the GPU-dump run
 * folder when "셰이더·파이프라인 저장" is on). libg7pipelinerepro.so (examples/pipeline-repro.c of the library, linked statically with it) rebuilds exactly that pipeline on this phone's Vulkan device
 * and prints a report; with matrix=true it also runs the library's DIAGNOSTIC variant matrix (each variant starts from the original; nothing here is ever applied to a real run).
 * Starts one child process, reads its output (bounded), writes it to g7_pipelinerepro_<ms>.txt in the app's files folder. Touches no clone, no ROM, no disk image.
 */
final class G7PipeRepro {
    static final String LIB_NAME = "libg7pipelinerepro.so";
    static final String LOG_PREFIX = "g7_pipelinerepro_";
    static final int MAX_OUT = 512 * 1024;
    static final long TIMEOUT_S = 180;
    static int rowTimeoutS = 40;   // per row; not final only so the JVM test (tests/java/G7PipeReproTest.java) can use a short one

    private G7PipeRepro() {}

    /** User cancel / activity destroy. cancel() kills the running child's whole process tree at once (from any thread) and the ladder stops before the next row. */
    static final class Cancel {
        private volatile boolean cancelled; private volatile Process proc; volatile String why = ""; volatile String killReport = "";
        void cancel(String reason) { why = reason; cancelled = true; Process p = proc; if (p != null) killReport = killTree(p); }
        boolean isCancelled() { return cancelled; }
        void attach(Process p) { proc = p; if (cancelled) killReport = killTree(p); }   // cancel() raced with the start of the child
        void detach() { proc = null; }
    }
    /** Options of one ladder run; every field has the previous behaviour as default (cancel null = never cancelled). */
    static final class Options {
        Cancel cancel;
        java.util.Set<String> groups;     // null or containing "ALL" = every row (default); else group keys from GROUPS
        Listener listener;                // progress, called from the worker thread after every row
        Options cancel(Cancel c) { cancel = c; return this; }
        Options groups(java.util.Set<String> g) { groups = g; return this; }
        Options listener(Listener l) { listener = l; return this; }
    }
    interface Listener { void onProgress(Progress p); }
    /** done = rows finished in this run (incl. n/a); failed = FAIL(any) + CRASH + TIMEOUT + NOROW + other; remaining = selected rows not yet finished. */
    static final class Progress {
        int selected, done, built, failed, na, remaining; String current = "";
        public String toString() { return "done " + done + "/" + selected + " (BUILT " + built + ", failed " + failed + ", n/a " + na + "), remaining " + remaining + (current.isEmpty() ? "" : ", now " + current); }
    }

    /** Row groups = row-id prefixes of the --list-rows output (nothing is typed: the UI shows these as check boxes with the row count). Every row belongs to exactly one group; BASE is whatever no other group claims
     *  (R0, R0V and all base/binding before-after rows: their ids are not known to this app, so they stay in BASE). R0 and R0V are ANCHORS: always run, because the status line and the verdict depend on them. */
    static final String[][] GROUPS = {
        {"BASE", "기본 행 + 바인딩 전/후 (R0, R0V 포함; 아래 그룹이 아닌 모든 행)"},
        {"XN", "CH10/CH11 입력 부분집합 XN*"},
        {"XGU", "진입 컷 격자 XG* / XU*"},
        {"PRUNE", "가지치기·SSA·지역변수 행 PB..PALLI (PB.CH10 형식)"},
        {"SPVOPT", "spirv-opt 행 F:*"},
        {"IW", "인터페이스 행 IW*"},
        {"CXF", "constexpr-sampler 행 CXF*"},
    };
    static String groupOf(String id) {
        if (id.startsWith("F:")) return "SPVOPT";
        if (id.matches("XN[0-9a-fA-F]{2}")) return "XN";
        if (id.startsWith("XG") || id.startsWith("XU")) return "XGU";
        if (id.matches("P[A-Z]+\\..*")) return "PRUNE";
        if (id.startsWith("IW")) return "IW";
        if (id.startsWith("CXF")) return "CXF";
        return "BASE";
    }
    static boolean isAnchor(String id) { return id.equals("R0") || id.equals("R0V"); }
    /** The rows to run, in list order. groups null/empty/ALL = all rows. */
    static List<String> select(List<String> ids, java.util.Set<String> groups) {
        if (groups == null || groups.isEmpty() || groups.contains("ALL")) return new ArrayList<>(ids);
        List<String> out = new ArrayList<>();
        for (String id : ids) if (isAnchor(id) || groups.contains(groupOf(id))) out.add(id);
        return out;
    }
    /** Row count per group key (LinkedHashMap in GROUPS order, empty groups included with 0). */
    static java.util.Map<String, Integer> groupCounts(List<String> ids) {
        java.util.Map<String, Integer> m = new java.util.LinkedHashMap<>();
        for (String[] g : GROUPS) m.put(g[0], 0);
        for (String id : ids) m.merge(groupOf(id), 1, Integer::sum);
        return m;
    }
    /** Runs --list-rows once (UI: to show the group check boxes before the run). Empty list = the runner gave no rows. */
    static List<String> listRows(File nativeLibDir, File bundle, File redDir, Cancel cancel) throws IOException, InterruptedException {
        String exe = new File(nativeLibDir, LIB_NAME).getPath();
        return parseIds(runOne(Arrays.asList(exe, bundle.getPath(), "--reduce", "--list-rows"), redDir, 60, null, 4_000_000, cancel, null));
    }
    static List<String> parseIds(String listOut) {
        List<String> ids = new ArrayList<>();
        for (String ln : listOut.split("\n")) { int k = ln.indexOf("pipeline-reduce: id "); if (k >= 0) ids.add(ln.substring(k + "pipeline-reduce: id ".length()).trim()); }
        return ids;
    }
    private static volatile Cancel ACTIVE;
    /** For the activity's onDestroy: kills the running reproduction (child and its children). No-op when nothing runs. */
    static void cancelActive(String why) { Cancel c = ACTIVE; if (c != null) c.cancel(why); }

    /** Kills p and all its descendants (snapshot taken BEFORE the parent dies: afterwards orphans cannot be found), then WAITS up to 3 s and checks that every one is really gone.
     *  ProcessHandle is used by reflection (Android API 33+, JDK 9+; this app's compileSdk is 34 but minSdk 30). Returns a one-line report: "tree gone (N processes)" or "STILL ALIVE: ..." / "descendants not enumerable". */
    static String killTree(Process p) {
        List<Object> hs = new ArrayList<>(); String enumNote = "";
        Class<?> phc = null; java.lang.reflect.Method destroy = null, alive = null;
        try {
            phc = Class.forName("java.lang.ProcessHandle");
            destroy = phc.getMethod("destroyForcibly"); alive = phc.getMethod("isAlive");
            Object stream = Process.class.getMethod("descendants").invoke(p);
            java.util.Iterator<?> it = ((java.util.stream.Stream<?>) stream).iterator();
            while (it.hasNext()) hs.add(it.next());
        } catch (ReflectiveOperationException | RuntimeException e) { enumNote = "descendants not enumerable (" + e.getClass().getSimpleName() + "): only the direct child was killed"; }
        try {
            if (destroy != null) for (Object h : hs) { try { destroy.invoke(h); } catch (ReflectiveOperationException | RuntimeException ignored) {} }
        } finally { p.destroyForcibly(); }
        boolean parentGone = false;
        try { parentGone = p.waitFor(3, TimeUnit.SECONDS); } catch (InterruptedException ie) { Thread.currentThread().interrupt(); }
        int still = 0;
        long until = System.currentTimeMillis() + 3000;
        while (alive != null) {
            still = 0;
            for (Object h : hs) { try { if ((Boolean) alive.invoke(h)) still++; } catch (ReflectiveOperationException | RuntimeException ignored) {} }
            if (still == 0 || System.currentTimeMillis() > until) break;
            try { Thread.sleep(50); } catch (InterruptedException ie) { Thread.currentThread().interrupt(); break; }
        }
        String r = (parentGone && still == 0) ? "process tree gone (child + " + hs.size() + " descendant(s))" : "STILL ALIVE after kill: child=" + (parentGone ? "gone" : "alive") + " descendants=" + still + "/" + hs.size();
        return enumNote.isEmpty() ? r : r + "; " + enumNote;
    }

    static boolean isPackaged(File nativeLibDir) { return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile(); }

    /** All pipefail_* bundle folders under dumpRoot/run-*, newest first (by the run folder name, then the bundle name). */
    static List<File> listBundles(File dumpRoot) {
        List<File> out = new ArrayList<>();
        File[] runs = dumpRoot == null ? null : dumpRoot.listFiles();
        if (runs == null) return out;
        Arrays.sort(runs, Comparator.comparing(File::getName).reversed());
        for (File r : runs) {
            if (!r.isDirectory() || !r.getName().startsWith("run-")) continue;
            File[] b = r.listFiles();
            if (b == null) continue;
            Arrays.sort(b, Comparator.comparing(File::getName));
            for (File d : b) if (d.isDirectory() && d.getName().startsWith("pipefail_") && new File(d, "state").isFile()) out.add(d);
        }
        return out;
    }

    /** Result class of one row line. FAIL13 = vk=-13, FAILOTHER = another VkResult. */
    static String classify(String rowLine) {
        int k = rowLine.indexOf(" result="); if (k < 0) return "OTHER";
        String t = rowLine.substring(k + 8); int e = t.indexOf(' '); if (e >= 0) t = t.substring(0, e);
        if (t.equals("BUILT")) return "BUILT";
        if (t.equals("FAIL")) return rowLine.contains("result=FAIL vk=-13(") ? "FAIL13" : "FAILOTHER";
        if (t.equals("n/a")) return "NA";
        if (t.equals("CRASH") || t.equals("TIMEOUT") || t.equals("NOROW")) return t;
        return "OTHER";
    }

    /** The ladder without fork (the library has no process creation in this build): list the rows once, then one process per row (--only id) so a compiler crash or hang
     *  costs one row, not the run. Adaptive rows (ddmin/greedy) depend on earlier results and are therefore NOT generated here. Returns 0 when R0 reproduced the failure, -5 when cancelled. */
    static int runLadder(File nativeLibDir, File bundle, File redDir, boolean matrix, ByteArrayOutputStream buf, File logDir) throws IOException, InterruptedException {
        return runLadder(nativeLibDir, bundle, redDir, matrix, buf, logDir, new Options());
    }

    static int runLadder(File nativeLibDir, File bundle, File redDir, boolean matrix, ByteArrayOutputStream buf, File logDir, Options opt) throws IOException, InterruptedException {
        Cancel cancel = opt.cancel;
        String exe = new File(nativeLibDir, LIB_NAME).getPath();
        String listOut = runOne(Arrays.asList(exe, bundle.getPath(), "--reduce", "--list-rows"), redDir, 60, null, 4_000_000, cancel, null);
        List<String> ids = parseIds(listOut);
        if (cancel != null && cancel.isCancelled()) { buf.write("ladder: CANCELLED before the row list was complete\n".getBytes(StandardCharsets.UTF_8)); return -5; }
        if (ids.isEmpty()) { buf.write(("ladder: --list-rows gave no rows; output:\n" + listOut + "\n").getBytes(StandardCharsets.UTF_8)); return 2; }
        StringBuilder o = new StringBuilder("ladder: " + ids.size() + " rows (one process per row; adaptive rows not generated; BUILT = creation returned success on this device, not rendering)\n");
        int r0 = -9, built = 0, fail = 0, bad = 0, na = 0; boolean complete = false, cancelled = false; String r0v = "not run";
        java.util.Map<String, Integer> cls = new java.util.TreeMap<>();
        List<String> sel = select(ids, opt.groups);
        boolean partialSel = sel.size() != ids.size();
        Progress pg = new Progress(); pg.selected = sel.size(); pg.remaining = sel.size();
        if (partialSel) o.append("ladder-selection: groups=").append(new java.util.TreeSet<>(opt.groups)).append(" -> ").append(sel.size()).append(" of ").append(ids.size()).append(" rows (R0 and R0V always run); the other rows were NOT run\n");
        if (opt.listener != null) opt.listener.onProgress(pg);
        long end = System.currentTimeMillis() + 840_000L;
        // each row is appended to its own file as soon as it is done, so an app kill or a crash keeps everything finished so far (the final window/file still comes at the end)
        File partial = new File(logDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + "-ladder-partial.txt");
        try { appendLine(partial, "bundle=" + bundle.getName() + " " + o.toString().trim()); } catch (IOException e) { o.append("(partial file not writable: ").append(e).append(")\n"); }
        try {
        for (String id : sel) {
            if (cancel != null && cancel.isCancelled()) { cancelled = true; break; }
            if (System.currentTimeMillis() > end) { o.append("ladder: time budget used up before row ").append(id).append("\n"); break; }
            int[] ec = new int[1]; String[] kr = new String[1]; pg.current = id;
            String out = runOne(Arrays.asList(exe, bundle.getPath(), "--reduce", "--only", id), redDir, rowTimeoutS, ec, 4_000_000, cancel, kr);
            if (ec[0] == -6) { cancelled = true; o.append("ladder: CANCELLED during row ").append(id).append(" (").append(cancel.why).append("); this row is NOT recorded; kill check: ").append(kr[0]).append("\n"); break; }
            String rowLine = null;
            for (String ln : out.split("\n")) if (ln.contains("pipeline-reduce: row=" + id + " ")) { rowLine = ln.substring(ln.indexOf("pipeline-reduce:")); break; }
            if (rowLine == null) {
                // exit 0 with no row line is NOT a crash: an older library (the pinned one) prints no row for an offline-file row (F:<name>) whose file is absent, only the summary
                // "N offline file row(s) absent ..."; a signal or a non-zero exit is a crash, -2 a timeout
                boolean absent = ec[0] == 0 && id.startsWith("F:") && out.contains("offline file row(s) absent");
                rowLine = absent ? "pipeline-reduce: row=" + id + " result=n/a exit=0 (offline file absent: the row was NOT run; the library printed no row line for it)"
                        : "pipeline-reduce: row=" + id + " result=" + (ec[0] == -2 ? "TIMEOUT" : ec[0] == 0 ? "NOROW" : "CRASH") + " exit=" + ec[0] + (ec[0] == 0 ? " (no row line although the process exited normally)" : " (no row line; process ended)")
                          + (ec[0] == -2 && kr[0] != null ? " kill: " + kr[0] : "");
            }
            o.append(rowLine).append("\n"); appendQuiet(partial, rowLine);
            String c = classify(rowLine); cls.merge(c, 1, Integer::sum);
            if (c.equals("BUILT")) built++; else if (c.startsWith("FAIL")) fail++; else if (c.equals("NA")) na++; else bad++;
            pg.done++; pg.remaining = sel.size() - pg.done; pg.built = built; pg.na = na; pg.failed = fail + bad; pg.current = "";
            if (opt.listener != null) { try { opt.listener.onProgress(pg); } catch (RuntimeException ignored) {} }
            if (id.equals("R0V")) r0v = c.equals("BUILT") ? "BUILT" : c.equals("FAIL13") ? "same -13" : c.equals("FAILOTHER") ? "FAIL (other VkResult)" : c.equals("CRASH") || c.equals("TIMEOUT") || c.equals("NOROW") ? c : "undecided";
            if (id.equals("R0")) r0 = c.equals("BUILT") ? 0 : c.equals("FAIL13") ? 1 : c.equals("FAILOTHER") ? 4 : c.equals("CRASH") ? 5 : c.equals("TIMEOUT") ? 6 : c.equals("NOROW") ? 7 : 3;   // CRASH/TIMEOUT/NOROW/INVALID on R0 is NOT "failure reproduced"
        }
        o.append("ladder: rows=").append(sel.size()).append(" built=").append(built).append(" fail=").append(fail).append(" n/a=").append(na).append(" other=").append(bad).append("\n");
        o.append("ladder-classes: FAIL(-13)=").append(cls.getOrDefault("FAIL13", 0)).append(" FAIL(other VkResult)=").append(cls.getOrDefault("FAILOTHER", 0)).append(" BUILT=").append(built)
         .append(" CRASH=").append(cls.getOrDefault("CRASH", 0)).append(" TIMEOUT=").append(cls.getOrDefault("TIMEOUT", 0)).append(" NOROW=").append(cls.getOrDefault("NOROW", 0)).append(" n/a=").append(na).append(" other=").append(cls.getOrDefault("OTHER", 0)).append("\n");
            complete = !cancelled && System.currentTimeMillis() <= end && !sel.isEmpty();
        } catch (RuntimeException | InterruptedException | IOException e) {
            o.append("ladder: ABORTED: ").append(e).append(" (the rows above were completed)\n");
            if (e instanceof InterruptedException) Thread.currentThread().interrupt();
        } finally {
            o.append("ladder-status: R0=").append(r0 == 1 ? "same -13 (reproduced)" : r0 == 0 ? "BUILT (not reproduced)" : r0 == 4 ? "FAIL with a different VkResult" : r0 == 5 ? "CRASH (the R0 process crashed; undecided, NOT reproduced)" : r0 == 6 ? "TIMEOUT (undecided)" : r0 == 7 ? "NOROW (exit 0, no R0 line; undecided)" : r0 == -9 ? "no R0 line" : "undecided (invalid)")
             .append("; R0V (original FS + REAL VS) = ").append(r0v).append("; run=").append(complete ? "complete" : cancelled ? "CANCELLED by the user (rows above are the finished ones)" : "INCOMPLETE (aborted, time budget, or no rows)").append(" (the two are independent)\n");
            buf.write(o.toString().getBytes(StandardCharsets.UTF_8));   // whatever was collected so far is always handed to the result window
        }
        return cancelled ? -5 : !complete ? -3 : r0 == 1 ? 0 : r0 == 0 ? 1 : 2;   // an incomplete run never reports a plain verdict   // 0 reproduced (R0 FAIL), 1 not reproduced (R0 BUILT), 2 undecided, -5 cancelled
    }

    private static void appendLine(File f, String line) throws IOException {
        try (java.io.FileOutputStream fo = new java.io.FileOutputStream(f, true)) { fo.write((line + "\n").getBytes(StandardCharsets.UTF_8)); fo.getFD().sync(); }
    }
    private static void appendQuiet(File f, String line) { try { appendLine(f, line); } catch (IOException ignored) {} }

    private static String runOne(List<String> c, File redDir, int timeoutS, int[] exitOut, int maxBytes) throws IOException, InterruptedException { return runOne(c, redDir, timeoutS, exitOut, maxBytes, null, null); }

    /** exitOut[0]: exit value, -2 timeout (tree killed), -6 cancelled (tree killed). killOut[0] gets the kill report when the tree was killed. */
    private static String runOne(List<String> c, File redDir, int timeoutS, int[] exitOut, int maxBytes, Cancel cancel, String[] killOut) throws IOException, InterruptedException {
        ProcessBuilder pb = new ProcessBuilder(c).redirectErrorStream(true);
        if (redDir != null) pb.environment().put("LAGFX_PIPELINE_MATRIX_DIR", redDir.getPath());
        if (cancel != null && cancel.isCancelled()) { if (exitOut != null) exitOut[0] = -6; return ""; }
        Process p = pb.start();
        if (cancel != null) cancel.attach(p);
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        Thread rd = new Thread(() -> { try (InputStream in = p.getInputStream()) { byte[] t = new byte[8192]; int n; while ((n = in.read(t)) > 0) if (b.size() < maxBytes) b.write(t, 0, Math.min(n, maxBytes - b.size())); } catch (IOException ignored) {} });
        rd.start();
        int ec; String kr = null;
        try {
            long until = System.currentTimeMillis() + timeoutS * 1000L; boolean done = false;
            while (!(done = p.waitFor(100, TimeUnit.MILLISECONDS))) {
                if (cancel != null && cancel.isCancelled()) break;
                if (System.currentTimeMillis() >= until) break;
            }
            // checked first: cancel() itself kills the tree, so the child may already look "done" (exit 137) here
            if (cancel != null && cancel.isCancelled()) { ec = -6; kr = cancel.killReport.isEmpty() ? killTree(p) : cancel.killReport; }
            else if (done) ec = p.exitValue();
            else { ec = -2; kr = killTree(p); }
        } catch (InterruptedException | RuntimeException e) { killTree(p); throw e; }
        finally { if (cancel != null) cancel.detach(); }
        rd.join(2000);
        if (exitOut != null) exitOut[0] = ec;
        if (killOut != null) killOut[0] = kr;
        return new String(b.toByteArray(), StandardCharsets.UTF_8);
    }

    /** Copies assets/red/* (ladder input files) into dir once per size; returns a one-line note. Never touches anything else. */
    static String stageRedAssets(android.content.res.AssetManager am, File dir) {
        int n = 0, bad = 0;
        try {
            String[] names = am.list("red");
            if (names == null) return "red_assets=0";
            if (!dir.isDirectory() && !dir.mkdirs()) return "red_assets=mkdir_failed";
            for (String nm : names) {
                File out = new File(dir, nm);
                try (InputStream in = am.open("red/" + nm)) {
                    // always rewritten: a same-size file from an older APK must never be reused
                    try (java.io.FileOutputStream fo = new java.io.FileOutputStream(out)) { byte[] t = new byte[65536]; int k; while ((k = in.read(t)) > 0) fo.write(t, 0, k); }
                    n++;
                } catch (Exception e) { bad++; }
            }
        } catch (Exception e) { return "red_assets=error"; }
        return "red_assets=" + n + (bad > 0 ? " failed=" + bad : "");
    }

    static final class Result { final boolean started; final int exitCode; final String text; final File file; Result(boolean s, int c, String t, File f) { started = s; exitCode = c; text = t; file = f; } }

    /** Runs the repro on one bundle. Blocking: call from a worker thread. */
    private static final java.util.concurrent.atomic.AtomicBoolean BUSY = new java.util.concurrent.atomic.AtomicBoolean(false);

    static Result run(File nativeLibDir, File bundle, boolean matrix, boolean reduce, android.content.res.AssetManager assets, File logDir) { return run(nativeLibDir, bundle, matrix, reduce, assets, logDir, new Options()); }

    static Result run(File nativeLibDir, File bundle, boolean matrix, boolean reduce, android.content.res.AssetManager assets, File logDir, Options opt) {
        if (!BUSY.compareAndSet(false, true)) return new Result(false, -1, "다른 재현 시험이 이미 실행 중입니다.", null);
        if (opt.cancel == null) opt.cancel = new Cancel();
        ACTIVE = opt.cancel;
        try { return run0(nativeLibDir, bundle, matrix, reduce, assets, logDir, opt); } finally { ACTIVE = null; BUSY.set(false); }
    }

    private static Result run0(File nativeLibDir, File bundle, boolean matrix, boolean reduce, android.content.res.AssetManager assets, File logDir, Options opt) {
        if (!isPackaged(nativeLibDir)) return new Result(false, -1, "재현 실행 파일(" + LIB_NAME + ")이 이 APK에 없습니다.", null);
        if (bundle == null || !bundle.isDirectory()) return new Result(false, -1, "번들 폴더가 없습니다.", null);
        List<String> cmd = new ArrayList<>();
        cmd.add(new File(nativeLibDir, LIB_NAME).getPath());
        cmd.add(bundle.getPath());
        File redDir = null; String redNote = "";
        if (reduce) { cmd.add("--reduce"); redDir = new File(logDir, "red-assets"); redNote = stageRedAssets(assets, redDir); }   // --reduce: the A2Xghfc reduction ladder (library f486810), crash-isolated per row
        if (matrix) { cmd.add("--matrix"); cmd.add("--m13"); }   // --m13: the -13 matrix rows M0..M10 (library 42151bd) after the old a..j variants
        StringBuilder head = new StringBuilder();
        head.append("g7-pipelinerepro: bundle=").append(bundle.getName()).append(" run=").append(bundle.getParentFile() == null ? "?" : bundle.getParentFile().getName())
            .append(" matrix=").append(matrix).append(" reduce=").append(reduce).append(reduce && matrix ? "(matrix NOT run in ladder mode)" : "").append(redNote.isEmpty() ? "" : " " + redNote).append(" henge=").append(G7ExecLauncher.HENGE_SHA).append(" lagfx=").append(G7ExecLauncher.LAGFX_SHA).append("\n");
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        int code = -1; String note = "";
        try {
            if (reduce) { code = runLadder(nativeLibDir, bundle, redDir, matrix, buf, logDir, opt); return finish(head, code, buf, note, logDir); }
            int[] ec = new int[1];
            String[] kr = new String[1];
            String outText = runOne(cmd, redDir, 180, ec, MAX_OUT, opt.cancel, kr);
            buf.write(outText.getBytes(StandardCharsets.UTF_8));
            if (ec[0] == -2) note += "\n[시간 제한 180초: 강제 종료; " + kr[0] + "]\n";
            if (ec[0] == -6) note += "\n[사용자 취소: " + kr[0] + "]\n";
            if (buf.size() >= MAX_OUT) note += "\n[출력이 " + MAX_OUT + " 바이트에서 잘렸을 수 있습니다]\n";
            code = ec[0];
        } catch (IOException | InterruptedException | RuntimeException e) {
            if (buf.size() == 0) return new Result(false, -1, head + "실행하지 못했습니다: " + e, null);
            note += "\n[중단: " + e + "; 그때까지의 출력은 위에 있습니다]\n"; code = -3;   // partial output stays visible and copyable
        }
        return finish(head, code, buf, note, logDir);
    }

    private static Result finish(StringBuilder head, int code, ByteArrayOutputStream buf, String note, File logDir) {
        String verdict = code == 0 ? "재현됨(기록된 실패가 같은 결과로 다시 발생)" : code == 1 ? "재현되지 않음(생성 성공 또는 다른 결과)" : code == 2 ? "재현 도구 오류(사용법·번들·Vulkan)" : code == -2 ? "시간 초과" : code == -3 ? "중단(예외): 그때까지의 결과만 표시" : code == -5 || code == -6 ? "사용자 취소: 그때까지의 결과만 표시" : code == -4 ? "실행 오류" : "비정상 종료(코드 " + code + ", 신호 종료는 128 이상일 수 있음)";
        String text = head + "g7-pipelinerepro: exit=" + code + " -> " + verdict + "\n" + new String(buf.toByteArray(), StandardCharsets.UTF_8) + note;
        File f = null;
        try {
            f = new File(logDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + ".txt");
            java.nio.file.Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8), java.nio.file.StandardOpenOption.CREATE_NEW);
        } catch (IOException e) { f = null; text += "\n[결과 파일을 저장하지 못했습니다: " + e + "]\n"; }
        return new Result(true, code, text, f);
    }
}
