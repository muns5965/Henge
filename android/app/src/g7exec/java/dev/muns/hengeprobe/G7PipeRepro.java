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

    private G7PipeRepro() {}

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

    /** The ladder without fork (the library has no process creation in this build): list the rows once, then one process per row (--only id) so a compiler crash or hang
     *  costs one row, not the run. Adaptive rows (ddmin/greedy) depend on earlier results and are therefore NOT generated here. Returns 0 when R0 reproduced the failure. */
    static int runLadder(File nativeLibDir, File bundle, File redDir, boolean matrix, ByteArrayOutputStream buf, File logDir) throws IOException, InterruptedException {
        String exe = new File(nativeLibDir, LIB_NAME).getPath();
        List<String> ids = new ArrayList<>();
        String listOut = runOne(Arrays.asList(exe, bundle.getPath(), "--reduce", "--list-rows"), redDir, 60, null, 4_000_000);
        for (String ln : listOut.split("\n")) { int k = ln.indexOf("pipeline-reduce: id "); if (k >= 0) ids.add(ln.substring(k + "pipeline-reduce: id ".length()).trim()); }
        if (ids.isEmpty()) { buf.write(("ladder: --list-rows gave no rows; output:\n" + listOut + "\n").getBytes(StandardCharsets.UTF_8)); return 2; }
        StringBuilder o = new StringBuilder("ladder: " + ids.size() + " rows (one process per row; adaptive rows not generated; BUILT = creation returned success on this device, not rendering)\n");
        int r0 = -9, built = 0, fail = 0, bad = 0, na = 0; boolean complete = false; String r0v = "not run";
        long end = System.currentTimeMillis() + 840_000L;
        // each row is appended to its own file as soon as it is done, so an app kill or a crash keeps everything finished so far (the final window/file still comes at the end)
        File partial = new File(logDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + "-ladder-partial.txt");
        try { appendLine(partial, "bundle=" + bundle.getName() + " " + o.toString().trim()); } catch (IOException e) { o.append("(partial file not writable: ").append(e).append(")\n"); }
        try {
        for (String id : ids) {
            if (System.currentTimeMillis() > end) { o.append("ladder: time budget used up before row ").append(id).append("\n"); break; }
            int[] ec = new int[1];
            String out = runOne(Arrays.asList(exe, bundle.getPath(), "--reduce", "--only", id), redDir, 40, ec, 4_000_000);
            String rowLine = null;
            for (String ln : out.split("\n")) if (ln.contains("pipeline-reduce: row=" + id + " ")) { rowLine = ln.substring(ln.indexOf("pipeline-reduce:")); break; }
            if (rowLine == null) {
                // exit 0 with no row line is NOT a crash: an older library (the pinned one) prints no row for an offline-file row (F:<name>) whose file is absent, only the summary
                // "N offline file row(s) absent ..."; a signal or a non-zero exit is a crash, -2 a timeout
                boolean absent = ec[0] == 0 && id.startsWith("F:") && out.contains("offline file row(s) absent");
                String l = absent ? "pipeline-reduce: row=" + id + " result=n/a exit=0 (offline file absent: the row was NOT run; the library printed no row line for it)"
                        : "pipeline-reduce: row=" + id + " result=" + (ec[0] == -2 ? "TIMEOUT" : ec[0] == 0 ? "NOROW" : "CRASH") + " exit=" + ec[0] + (ec[0] == 0 ? " (no row line although the process exited normally)" : " (no row line; process ended)");
                o.append(l).append("\n"); appendQuiet(partial, l); if (absent) na++; else bad++; continue;
            }
            o.append(rowLine).append("\n"); appendQuiet(partial, rowLine);
            if (rowLine.contains("result=BUILT")) built++; else if (rowLine.contains("result=FAIL")) fail++; else if (rowLine.contains("result=n/a")) na++; else bad++;
            if (id.equals("R0V")) r0v = rowLine.contains("result=BUILT") ? "BUILT" : rowLine.contains("result=FAIL vk=-13(") ? "same -13" : rowLine.contains("result=FAIL") ? "FAIL (other VkResult)" : "undecided";
            if (id.equals("R0")) r0 = rowLine.contains("result=BUILT") ? 0 : rowLine.contains("result=FAIL vk=-13(") ? 1 : rowLine.contains("result=FAIL") ? 4 : 3;   // CRASH/TIMEOUT/INVALID on R0 is NOT "failure reproduced"
        }
        o.append("ladder: rows=").append(ids.size()).append(" built=").append(built).append(" fail=").append(fail).append(" n/a=").append(na).append(" other=").append(bad).append("\n");
            complete = System.currentTimeMillis() <= end && !ids.isEmpty();
        } catch (RuntimeException | InterruptedException | IOException e) {
            o.append("ladder: ABORTED: ").append(e).append(" (the rows above were completed)\n");
            if (e instanceof InterruptedException) Thread.currentThread().interrupt();
        } finally {
            o.append("ladder-status: R0=").append(r0 == 1 ? "same -13 (reproduced)" : r0 == 0 ? "BUILT (not reproduced)" : r0 == 4 ? "FAIL with a different VkResult" : r0 == -9 ? "no R0 line" : "undecided (crash/timeout/invalid)")
             .append("; R0V (original FS + REAL VS) = ").append(r0v).append("; run=").append(complete ? "complete" : "INCOMPLETE (aborted, time budget, or no rows)").append(" (the two are independent)\n");
            buf.write(o.toString().getBytes(StandardCharsets.UTF_8));   // whatever was collected so far is always handed to the result window
        }
        return !complete ? -3 : r0 == 1 ? 0 : r0 == 0 ? 1 : 2;   // an incomplete run never reports a plain verdict   // 0 reproduced (R0 FAIL), 1 not reproduced (R0 BUILT), 2 undecided
    }

    private static void appendLine(File f, String line) throws IOException {
        try (java.io.FileOutputStream fo = new java.io.FileOutputStream(f, true)) { fo.write((line + "\n").getBytes(StandardCharsets.UTF_8)); fo.getFD().sync(); }
    }
    private static void appendQuiet(File f, String line) { try { appendLine(f, line); } catch (IOException ignored) {} }

    private static String runOne(List<String> c, File redDir, int timeoutS, int[] exitOut, int maxBytes) throws IOException, InterruptedException {
        ProcessBuilder pb = new ProcessBuilder(c).redirectErrorStream(true);
        if (redDir != null) pb.environment().put("LAGFX_PIPELINE_MATRIX_DIR", redDir.getPath());
        Process p = pb.start();
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        Thread rd = new Thread(() -> { try (InputStream in = p.getInputStream()) { byte[] t = new byte[8192]; int n; while ((n = in.read(t)) > 0) if (b.size() < maxBytes) b.write(t, 0, Math.min(n, maxBytes - b.size())); } catch (IOException ignored) {} });
        rd.start();
        int ec;
        if (!p.waitFor(timeoutS, TimeUnit.SECONDS)) { p.destroyForcibly(); ec = -2; } else ec = p.exitValue();
        rd.join(2000);
        if (exitOut != null) exitOut[0] = ec;
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

    static Result run(File nativeLibDir, File bundle, boolean matrix, boolean reduce, android.content.res.AssetManager assets, File logDir) {
        if (!BUSY.compareAndSet(false, true)) return new Result(false, -1, "다른 재현 시험이 이미 실행 중입니다.", null);
        try { return run0(nativeLibDir, bundle, matrix, reduce, assets, logDir); } finally { BUSY.set(false); }
    }

    private static Result run0(File nativeLibDir, File bundle, boolean matrix, boolean reduce, android.content.res.AssetManager assets, File logDir) {
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
            if (reduce) { code = runLadder(nativeLibDir, bundle, redDir, matrix, buf, logDir); return finish(head, code, buf, note, logDir); }
            int[] ec = new int[1];
            String outText = runOne(cmd, redDir, 180, ec, MAX_OUT);
            buf.write(outText.getBytes(StandardCharsets.UTF_8));
            if (ec[0] == -2) note += "\n[시간 제한 180초: 강제 종료]\n";
            if (buf.size() >= MAX_OUT) note += "\n[출력이 " + MAX_OUT + " 바이트에서 잘렸을 수 있습니다]\n";
            code = ec[0];
        } catch (IOException | InterruptedException | RuntimeException e) {
            if (buf.size() == 0) return new Result(false, -1, head + "실행하지 못했습니다: " + e, null);
            note += "\n[중단: " + e + "; 그때까지의 출력은 위에 있습니다]\n"; code = -3;   // partial output stays visible and copyable
        }
        return finish(head, code, buf, note, logDir);
    }

    private static Result finish(StringBuilder head, int code, ByteArrayOutputStream buf, String note, File logDir) {
        String verdict = code == 0 ? "재현됨(기록된 실패가 같은 결과로 다시 발생)" : code == 1 ? "재현되지 않음(생성 성공 또는 다른 결과)" : code == 2 ? "재현 도구 오류(사용법·번들·Vulkan)" : code == -2 ? "시간 초과" : code == -3 ? "중단(예외): 그때까지의 결과만 표시" : code == -4 ? "실행 오류" : "비정상 종료(코드 " + code + ", 신호 종료는 128 이상일 수 있음)";
        String text = head + "g7-pipelinerepro: exit=" + code + " -> " + verdict + "\n" + new String(buf.toByteArray(), StandardCharsets.UTF_8) + note;
        File f = null;
        try {
            f = new File(logDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + ".txt");
            java.nio.file.Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8), java.nio.file.StandardOpenOption.CREATE_NEW);
        } catch (IOException e) { f = null; text += "\n[결과 파일을 저장하지 못했습니다: " + e + "]\n"; }
        return new Result(true, code, text, f);
    }
}
