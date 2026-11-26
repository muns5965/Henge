package dev.muns.hengeprobe;

import java.io.File;
import java.nio.file.Files;
import java.util.*;
import java.util.zip.*;

public class G7ExecDumpExportTest {
    static int failed = 0;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) failed++; }
    public static void main(String[] a) throws Exception {
        File fd = Files.createTempDirectory("exp").toFile(), root = new File(fd, "g7_gpudump");
        String id = "20261126-143512-345"; long ms = G7ExecDumpExport.parseId(id);
        File run = new File(root, "run-" + id); new File(run, "sub").mkdirs();
        Files.write(new File(run, "sub/a.bin").toPath(), new byte[] {1, 2, 3}); Files.write(new File(run, "x.spv").toPath(), new byte[] {4});
        Files.write(new File(fd, String.format("g7_lagfx_%013d.txt", ms + 120)).toPath(), "L".getBytes());          // the run's lagfx log (old APK: no run-info.txt, matched by time)
        Files.write(new File(fd, String.format("g7_cboot_%013d.txt", ms - 800)).toPath(), "C".getBytes());          // its VM log
        Files.write(new File(fd, String.format("g7_lagfx_%013d.txt", ms + 900000)).toPath(), "other".getBytes());   // another run's log
        new File(root, "run-20260101-000000-000").mkdirs();
        List<G7ExecDumpExport.DumpRun> rs = G7ExecDumpExport.listRuns(root);
        check("two runs listed, newest first", rs.size() == 2 && rs.get(0).id.equals(id) && rs.get(0).files == 2 && rs.get(0).bytes == 4);
        G7ExecDumpExport.Result r = G7ExecDumpExport.export(fd, rs.get(0), "v", "henge-sha", "lagfx-sha", "dev");
        Set<String> n = new TreeSet<>(); String info;
        try (ZipFile z = new ZipFile(r.zip)) {
            for (Enumeration<? extends ZipEntry> e = z.entries(); e.hasMoreElements();) n.add(e.nextElement().getName());
            java.io.InputStream in = z.getInputStream(z.getEntry("export-info.txt")); info = new String(in.readAllBytes(), "UTF-8");
        }
        check("zip: dump files, both logs of THIS run (not the other run's), manifest", n.equals(new TreeSet<>(Arrays.asList("dump/sub/a.bin", "dump/x.spv", "export-info.txt", String.format("logs/g7_cboot_%013d.txt", ms - 800), String.format("logs/g7_lagfx_%013d.txt", ms + 120)))));
        check("manifest names run id and both commits", info.contains("run_id=" + id) && info.contains("henge_commit=henge-sha") && info.contains("libapplegfx_vulkan_commit=lagfx-sha"));
        check("originals untouched", new File(run, "x.spv").length() == 1 && new File(fd, String.format("g7_lagfx_%013d.txt", ms + 120)).isFile());
        G7ExecDumpExport.Result r2 = G7ExecDumpExport.export(fd, rs.get(0), "v", "h", "l", "d");
        check("a second export never replaces the first", !r2.zip.equals(r.zip) && r.zip.isFile() && r2.zip.getName().endsWith("-2.zip") && G7ExecLauncher.logFile(fd, r2.zip.getName()) != null);
        // a newer APK's run-info.txt names the log exactly
        Files.write(new File(run, G7ExecDumpExport.INFO).toPath(), ("lagfx_log=" + String.format("g7_lagfx_%013d.txt", ms + 900000) + "\n").getBytes());
        G7ExecDumpExport.Result r3 = G7ExecDumpExport.export(fd, G7ExecDumpExport.listRuns(root).get(0), "v", "h", "l", "d");
        try (ZipFile z = new ZipFile(r3.zip)) { check("run-info.txt wins over time matching", z.getEntry(String.format("logs/g7_lagfx_%013d.txt", ms + 900000)) != null && z.getEntry(String.format("logs/g7_lagfx_%013d.txt", ms + 120)) == null); }
        System.out.println(failed == 0 ? "G7DUMPEXPORT OK" : "G7DUMPEXPORT FAILED " + failed);
        System.exit(failed == 0 ? 0 : 1);
    }
}
