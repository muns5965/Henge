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
 */
final class G7ExecDumpExport {
    static final Pattern RUN = Pattern.compile("run-(\\d{8}-\\d{6}-\\d{3})");
    static final String INFO = "run-info.txt";
    static final long LOG_MATCH_MS = 5_000, CBOOT_BEFORE_MS = 120_000;

    static final class DumpRun {
        final File dir; final String id; final long ms; final int files; final long bytes;
        DumpRun(File d, String i, long m, int f, long b) { dir = d; id = i; ms = m; files = f; bytes = b; }
        String label() { return id + "  (" + files + " 파일, " + (bytes + 1023) / 1024 + " KiB)"; }
    }

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
        File lagfx = null, cboot = null; String how;
        String named = info(run.dir, "lagfx_log");
        if (named != null) lagfx = G7ExecLauncher.logFile(filesDir, named);
        how = lagfx != null ? "run-info.txt" : "시간 일치(옛 APK의 덤프 또는 run-info 없음)";
        if (lagfx == null) lagfx = matchLog(filesDir, "g7_lagfx_", run.ms, LOG_MATCH_MS, LOG_MATCH_MS);
        String vmNamed = info(run.dir, "vm_log");
        if (vmNamed != null) cboot = G7ExecLauncher.logFile(filesDir, vmNamed);
        String vmHow = cboot != null ? "run-info.txt" : "time";
        if (cboot == null) cboot = matchLog(filesDir, "g7_cboot_", run.ms, CBOOT_BEFORE_MS, 2_000);   // older runs: the VM log is opened just before the runner starts
        File zip = null;
        for (int n = 1; n <= 99 && zip == null; n++) {
            File c = new File(filesDir, "g7_export_" + run.id + (n == 1 ? "" : "-" + n) + ".zip");
            if (!c.exists()) zip = c;
        }
        if (zip == null) throw new IOException("too many exports of this run");
        List<File> files = new ArrayList<>();
        walk(run.dir, run.dir, files, new long[2]);
        StringBuilder m = new StringBuilder();
        m.append("Henge GPU dump export\n");
        m.append("run_id=").append(run.id).append('\n');
        m.append("run_started_local_ms=").append(run.ms).append('\n');
        String runHenge = info(run.dir, "henge_commit"), runLagfx = info(run.dir, "lagfx_commit");
        m.append("run_henge_commit=").append(runHenge == null ? "UNKNOWN" : runHenge).append("   (the build that PRODUCED this run, from run-info.txt)\n");
        m.append("run_libapplegfx_vulkan_commit=").append(runLagfx == null ? "UNKNOWN" : runLagfx).append('\n');
        m.append("exporter_henge_commit=").append(hengeSha).append("   (the build that made this ZIP; may differ from the run's)\n");
        m.append("exporter_libapplegfx_vulkan_commit=").append(lagfxSha).append('\n');
        m.append("app_version=").append(appVersion).append('\n');
        m.append("device=").append(device).append('\n');
        m.append("exported_at_ms=").append(System.currentTimeMillis()).append('\n');
        m.append("dump_files=").append(files.size()).append(" dump_bytes=").append(run.bytes).append('\n');
        m.append("lagfx_log=").append(lagfx == null ? "NONE FOUND" : lagfx.getName()).append(" (matched by ").append(how).append(")\n");
        m.append("vm_log=").append(cboot == null ? "NONE FOUND" : cboot.getName()).append(" (matched by ").append(vmHow).append(cboot != null && "time".equals(vmHow) ? ": g7_cboot_ log closest to the run folder, up to 2 min before it" : "").append(")\n");
        m.append("note=originals are not deleted or changed; the app keeps its own copies\n");
        try (ZipOutputStream z = new ZipOutputStream(Files.newOutputStream(zip.toPath(), StandardOpenOption.CREATE_NEW))) {
            put(z, "export-info.txt", m.toString().getBytes(StandardCharsets.UTF_8));
            if (lagfx != null) putFile(z, "logs/" + lagfx.getName(), lagfx);
            if (cboot != null) putFile(z, "logs/" + cboot.getName(), cboot);
            String base = run.dir.getCanonicalPath() + File.separator;
            for (File f : files) {
                String p = f.getCanonicalPath();
                if (!p.startsWith(base)) continue;   // never leaves the run folder
                putFile(z, "dump/" + p.substring(base.length()).replace(File.separatorChar, '/'), f);
            }
        } catch (IOException | RuntimeException e) {
            throw new IOException("ZIP write failed: " + e.getMessage(), e);
        }
        r.zip = zip;
        r.message = zip.getName() + " (" + (zip.length() + 1023) / 1024 + " KiB): 덤프 " + files.size() + "개, lagfx 로그 " + (lagfx == null ? "없음" : "있음") + ", VM 로그 " + (cboot == null ? "없음" : "있음");
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
