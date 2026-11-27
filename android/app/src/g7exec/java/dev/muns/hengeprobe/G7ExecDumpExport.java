package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * [G7-EXEC] Packs one GPU-dump run (folder run-yyyyMMdd-HHmmss-SSS under g7_gpudump) together with the VM log (g7_cboot_*) and the lagfx log (g7_lagfx_*) of the SAME run into one new ZIP, with a manifest
 * that names the Henge and library commits. READ-ONLY on everything that exists: nothing is deleted, moved or changed; the only file written is the new ZIP (CREATE_NEW, never replaced). Runs made by older APKs
 * (no run-info.txt) are matched to their logs by time. Only after the run has ended (the caller checks).
 * The "current run" is the newest g7_cboot_ log (latest clone boot), never merely the newest dump folder: when that run has no dump folder (GPU data dump was OFF) its own logs are exported alone and
 * say so; an older run's dump, logs or PNGs are never substituted, and an explicitly chosen older run is labelled as older on the first line of export-info.txt.
 */
final class G7ExecDumpExport {
    static final Pattern RUN = Pattern.compile("run-(\\d{8}-\\d{6}-\\d{3})");
    static final String INFO = "run-info.txt";
    static final long LOG_MATCH_MS = 5_000, CBOOT_BEFORE_MS = 120_000;

    static final class DumpRun {
        final File dir; final String id; final long ms; final int files; final long bytes; final File cboot;   // dir == null: logs-only run (no dump folder), cboot = its own VM log
        DumpRun(File d, String i, long m, int f, long b) { this(d, i, m, f, b, null); }
        DumpRun(File d, String i, long m, int f, long b, File c) { dir = d; id = i; ms = m; files = f; bytes = b; cboot = c; }
        boolean noDump() { return dir == null; }
        String label() { return noDump() ? id + "  (GPU 덤프 없음: GPU data dump was OFF, 로그만)" : id + "  (" + files + " 파일, " + (bytes + 1023) / 1024 + " KiB)"; }
    }

    static final String NO_DUMP_TEXT = "this run has no GPU dump (GPU data dump was OFF); logs only";

    /** The newest g7_cboot_ log (the latest clone-boot run), or null. */
    static File latestCboot(File filesDir) {
        File[] l = filesDir == null ? null : filesDir.listFiles((d, n) -> n.matches("g7_cboot_[0-9]{13}\\.txt"));
        File best = null; long bt = -1;
        if (l != null) for (File f : l) { long t = logTime(f); if (t > bt) { bt = t; best = f; } }
        return best;
    }

    /** Does this dump run belong to that clone-boot log? run-info.txt names it exactly; older dumps (no vm_log) are matched by time like the export does. */
    static boolean belongsTo(DumpRun r, File cboot) {
        if (r == null || r.dir == null || cboot == null) return false;
        String named = info(r.dir, "vm_log");
        if (named != null) return named.equals(cboot.getName());
        long d = r.ms - logTime(cboot);
        return d >= -2_000 && d <= CBOOT_BEFORE_MS;
    }

    /** What the export dialog offers: element 0 is the CURRENT run (the latest clone boot) - with its dump if it has one, otherwise a logs-only entry - followed by the older dump runs, newest first. Never a stale dump in place of the current run. */
    static List<DumpRun> choices(File filesDir, File dumpRoot) {
        List<DumpRun> runs = listRuns(dumpRoot), out = new ArrayList<>();
        File cur = latestCboot(filesDir);
        if (cur == null) return runs;   // no clone-boot log at all: nothing to compare with, the old behaviour
        DumpRun mine = null;
        for (DumpRun r : runs) if (belongsTo(r, cur)) { mine = r; break; }
        if (mine == null) mine = new DumpRun(null, newId(logTime(cur)), logTime(cur), 0, 0, cur);
        out.add(mine);
        for (DumpRun r : runs) if (r != mine) out.add(r);
        return out;
    }

    private static String when(long ms) { return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(ms)); }

    /** First line of export-info.txt: which run this is, and whether it is the current one. */
    static String firstLine(DumpRun run, File cboot, File latest, boolean older) {
        if (run.noDump()) return "export_of=CURRENT RUN " + (cboot == null ? run.id : cboot.getName()) + " - " + NO_DUMP_TEXT;
        if (!older) return "export_of=CURRENT RUN run_id=" + run.id + " (latest clone-boot run" + (cboot == null ? "" : ", vm log " + cboot.getName()) + ")";
        return "export_of=OLDER RUN run_id=" + run.id + " date=" + when(run.ms) + " - NOT the latest run (latest clone-boot log: " + (latest == null ? "?" : latest.getName() + ", " + when(logTime(latest))) + ")";
    }

    /** A value from one header line of the run's own VM log (null when the line or the value is not there). Reads only the first 64 KiB. */
    static String logOpt(File cboot, String linePrefix, String regex) {
        if (cboot == null || !cboot.isFile()) return null;
        try (InputStream in = Files.newInputStream(cboot.toPath())) {
            byte[] b = new byte[64 * 1024]; int n = in.read(b); if (n <= 0) return null;
            for (String line : new String(b, 0, n, StandardCharsets.UTF_8).split("\n")) if (line.startsWith(linePrefix)) { Matcher m = Pattern.compile(regex).matcher(line); return m.find() ? m.group(1).trim() : null; }
        } catch (IOException | RuntimeException e) { return null; }
        return null;
    }

    private static String opt(String v, String why) { return v == null ? "UNKNOWN (" + why + ")" : v; }

    static long parseId(String id) {
        try { return new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).parse(id).getTime(); } catch (java.text.ParseException e) { return -1; }
    }

    static String newId(long ms) { return new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date(ms)); }

    /** All run folders under the dump root, newest first (also those made by older APKs). */
    static List<DumpRun> listRuns(File dumpRoot) {
        List<DumpRun> out = new ArrayList<>();
        File[] k = dumpRoot == null ? null : dumpRoot.listFiles();
        if (k == null) return out;
        for (File d : k) {
            Matcher m = RUN.matcher(d.getName());
            if (!d.isDirectory() || !m.matches()) continue;
            long[] st = {0, 0};
            walk(d, d, new ArrayList<>(), st);
            out.add(new DumpRun(d, m.group(1), parseId(m.group(1)), (int) st[0], st[1]));
        }
        Collections.sort(out, (a, b) -> b.id.compareTo(a.id));
        return out;
    }

    private static void walk(File root, File d, List<File> into, long[] st) {
        File[] k = d.listFiles();
        if (k == null) return;
        Arrays.sort(k);
        for (File f : k) {
            try { if (Files.isSymbolicLink(f.toPath())) continue; } catch (RuntimeException e) { continue; }
            if (f.isDirectory()) walk(root, f, into, st);
            else if (f.isFile()) { into.add(f); st[0]++; st[1] += f.length(); }
        }
    }

    private static long logTime(File f) { Matcher m = Pattern.compile("_(\\d{13})\\.txt").matcher(f.getName()); return m.find() ? Long.parseLong(m.group(1)) : -1; }

    /** Closest log of this prefix whose time lies in [runMs - before, runMs + after]; null if none. */
    static File matchLog(File filesDir, String prefix, long runMs, long before, long after) {
        File[] l = filesDir.listFiles((d, n) -> n.matches(prefix + "[0-9]{13}\\.txt"));
        File best = null; long bd = Long.MAX_VALUE;
        if (l != null) for (File f : l) {
            long t = logTime(f), d = t - runMs;
            if (t < 0 || d < -before || d > after) continue;
            if (Math.abs(d) < bd) { bd = Math.abs(d); best = f; }
        }
        return best;
    }

    /** The value of a key=value line in the run's run-info.txt, or null. */
    static String info(File runDir, String key) {
        File f = new File(runDir, INFO);
        if (!f.isFile() || f.length() > 64 * 1024) return null;
        try {
            for (String line : new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).split("\n")) if (line.startsWith(key + "=")) return line.substring(key.length() + 1).trim();
        } catch (IOException | RuntimeException e) { return null; }
        return null;
    }

    static final class Result { File zip; String message; }

    /** Builds the ZIP. Never touches an existing file except by reading it. */
    static Result export(File filesDir, DumpRun run, String appVersion, String hengeSha, String lagfxSha, String device) throws IOException {
        Result r = new Result();
        File lagfx = null, cboot = null; String how, vmHow;
        if (run.noDump()) { cboot = run.cboot; how = "time (logs-only run)"; vmHow = "time"; lagfx = matchLog(filesDir, "g7_lagfx_", run.ms, LOG_MATCH_MS, 60_000); }
        else {
        String named = info(run.dir, "lagfx_log");
        if (named != null) lagfx = G7ExecLauncher.logFile(filesDir, named);
        how = lagfx != null ? "run-info.txt" : "시간 일치(옛 APK의 덤프 또는 run-info 없음)";
        if (lagfx == null) lagfx = matchLog(filesDir, "g7_lagfx_", run.ms, LOG_MATCH_MS, LOG_MATCH_MS);
        String vmNamed = info(run.dir, "vm_log");
        if (vmNamed != null) cboot = G7ExecLauncher.logFile(filesDir, vmNamed);
        vmHow = cboot != null ? "run-info.txt" : "time";
        if (cboot == null) cboot = matchLog(filesDir, "g7_cboot_", run.ms, CBOOT_BEFORE_MS, 2_000);   // older runs: the VM log is opened just before the runner starts
        }
        File latest = latestCboot(filesDir);
        boolean older = !run.noDump() && latest != null && (cboot != null ? logTime(latest) > logTime(cboot) : logTime(latest) > run.ms + 2_000);
        File zip = null;
        for (int n = 1; n <= 99 && zip == null; n++) {
            File c = new File(filesDir, "g7_export_" + run.id + (n == 1 ? "" : "-" + n) + ".zip");
            if (!c.exists()) zip = c;
        }
        if (zip == null) throw new IOException("too many exports of this run");
        List<File> files = new ArrayList<>();
        if (!run.noDump()) walk(run.dir, run.dir, files, new long[2]);
        StringBuilder m = new StringBuilder();
        m.append(firstLine(run, cboot, latest, older)).append('\n');
        m.append("Henge GPU dump export\n");
        m.append("run_id=").append(run.id).append('\n');
        m.append("run_started_local_ms=").append(run.ms).append('\n');
        String runHenge = run.noDump() ? null : info(run.dir, "henge_commit"), runLagfx = run.noDump() ? null : info(run.dir, "lagfx_commit");
        m.append("run_henge_commit=").append(runHenge == null ? "UNKNOWN" : runHenge).append("   (the build that PRODUCED this run, from run-info.txt)\n");
        m.append("run_libapplegfx_vulkan_commit=").append(runLagfx == null ? "UNKNOWN" : runLagfx).append('\n');
        String runJit = run.noDump() ? null : info(run.dir, "jit_mb");
        String logJit = logOpt(cboot, "g7-jit-config:", "jit_mb=(\\d+)");
        m.append("run_jit_mb=").append(runJit != null ? runJit : logJit != null ? logJit + " (from g7-jit-config in the run's own VM log)" : "UNKNOWN (older run: the runner default 64 was always passed)").append('\n');
        String runExact = run.noDump() ? null : info(run.dir, "jit_exact");
        m.append("run_jit_exact=").append(runExact == null ? "UNKNOWN (older run: the exact seen-set was never switched on)" : runExact).append('\n');
        String runPeak = run.noDump() ? null : info(run.dir, "jit_exact_peak_mib");
        m.append("run_jit_exact_peak_mib=").append(runPeak == null ? "UNKNOWN (a run before the peak budget was passed: the engine's unlimited default applied if exact was ON)" : runPeak).append('\n');
        String why = "not recorded in this run's VM log";
        String gpuLine = logOpt(cboot, "g7-gpu-options:", "^(.*)$");
        m.append("run_options_source=").append(cboot == null ? "NONE (no VM log)" : cboot.getName() + " (g7-gpu-options / g7-run-config / g7-engine-diag header lines; nothing is guessed)").append('\n');
        String ev = logOpt(cboot, "g7-engine-diag:", "display_events=(ON|OFF)");
        if (ev == null && gpuLine != null) ev = gpuLine.contains("display events experiment ON") ? "ON" : "OFF";
        m.append("run_gpu_exec=").append(opt(logOpt(cboot, "g7-gpu-options:", "gpu_exec=(ON|OFF)"), why)).append('\n');
        m.append("run_vbl_exp=").append(opt(logOpt(cboot, "g7-gpu-options:", "vbl_exp=([A-Za-z0-9_]+)"), why)).append('\n');
        m.append("run_display_events=").append(opt(ev, why)).append('\n');
        m.append("run_wb_verify=").append(opt(logOpt(cboot, "g7-gpu-options:", "wb_verify=([^;]*);"), why)).append('\n');
        m.append("run_usb_mode=").append(opt(logOpt(cboot, "g7-run-config:", "usb_mode=(\\d+)"), why)).append('\n');
        m.append("run_clone=").append(opt(logOpt(cboot, "g7-run-config:", "clone=([^;]+)"), why)).append('\n');
        m.append("exporter_henge_commit=").append(hengeSha).append("   (the build that made this ZIP; may differ from the run's)\n");
        m.append("exporter_libapplegfx_vulkan_commit=").append(lagfxSha).append('\n');
        m.append("app_version=").append(appVersion).append('\n');
        m.append("device=").append(device).append('\n');
        m.append("exported_at_ms=").append(System.currentTimeMillis()).append('\n');
        m.append("dump_files=").append(files.size()).append(" dump_bytes=").append(run.bytes).append('\n');
        m.append("lagfx_log=").append(lagfx == null ? "NONE FOUND" : lagfx.getName()).append(" (matched by ").append(how).append(")\n");
        m.append("vm_log=").append(cboot == null ? "NONE FOUND" : cboot.getName()).append(" (matched by ").append(vmHow).append(cboot != null && "time".equals(vmHow) && !run.noDump() ? ": g7_cboot_ log closest to the run folder, up to 2 min before it" : "").append(")\n");
        StringBuilder fr = new StringBuilder(); int nfr = 0;
        for (File f : files) if (f.getName().startsWith("g7_frame_") && f.getName().endsWith(".png")) { nfr++; fr.append(' ').append(f.getName()).append('(').append(f.length()).append(" B)"); }
        m.append("frame_files=").append(nfr).append(run.noDump() ? " (none: " + NO_DUMP_TEXT + ")" : nfr == 0 ? " (none: the dump folder was off, or no frame was seen)" : " (boot_args_* = the framebuffer XNU's boot_args points at; pvg_surface_* = the guest's PVG surface, n = time series, _last_ = newest at stop):" + fr).append('\n');
        m.append("note=originals are not deleted or changed; the app keeps its own copies\n");
        try (ZipOutputStream z = new ZipOutputStream(Files.newOutputStream(zip.toPath(), StandardOpenOption.CREATE_NEW))) {
            put(z, "export-info.txt", m.toString().getBytes(StandardCharsets.UTF_8));
            if (lagfx != null) putFile(z, "logs/" + lagfx.getName(), lagfx);
            if (cboot != null) putFile(z, "logs/" + cboot.getName(), cboot);
            String base = run.noDump() ? null : run.dir.getCanonicalPath() + File.separator;
            for (File f : files) {
                String p = f.getCanonicalPath();
                if (!p.startsWith(base)) continue;   // never leaves the run folder
                putFile(z, "dump/" + p.substring(base.length()).replace(File.separatorChar, '/'), f);
            }
        } catch (IOException | RuntimeException e) {
            throw new IOException("ZIP write failed: " + e.getMessage(), e);
        }
        r.zip = zip;
        r.message = (run.noDump() ? "이 실행에는 GPU 덤프가 없습니다(" + NO_DUMP_TEXT + "). " : older ? "이전 실행(" + run.id + ", " + when(run.ms) + ")의 내보내기입니다 - 최신 실행이 아닙니다. " : "") + zip.getName() + " (" + (zip.length() + 1023) / 1024 + " KiB): 덤프 " + files.size() + "개, lagfx 로그 " + (lagfx == null ? "없음" : "있음") + ", VM 로그 " + (cboot == null ? "없음" : "있음");
        return r;
    }

    private static void put(ZipOutputStream z, String name, byte[] b) throws IOException { z.putNextEntry(new ZipEntry(name)); z.write(b); z.closeEntry(); }

    private static void putFile(ZipOutputStream z, String name, File f) throws IOException {
        z.putNextEntry(new ZipEntry(name));
        try (InputStream in = Files.newInputStream(f.toPath())) {
            byte[] b = new byte[64 * 1024]; int n;
            while ((n = in.read(b)) > 0) z.write(b, 0, n);
        }
        z.closeEntry();
    }

    /** Copies an export ZIP to a stream the user chose (save dialog). Reads only the ZIP. */
    static void copyTo(File zip, OutputStream out) throws IOException {
        try (InputStream in = Files.newInputStream(zip.toPath()); OutputStream o = out) {
            byte[] b = new byte[64 * 1024]; int n;
            while ((n = in.read(b)) > 0) o.write(b, 0, n);
        }
    }
}
