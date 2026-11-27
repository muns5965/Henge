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
        check("manifest names run id and both commits", info.contains("run_id=" + id) && info.contains("exporter_henge_commit=henge-sha") && info.contains("exporter_libapplegfx_vulkan_commit=lagfx-sha") && info.contains("run_henge_commit=UNKNOWN") && info.contains("run_libapplegfx_vulkan_commit=UNKNOWN"));   // no run-info: the run's builds are UNKNOWN, never the exporter's
        check("originals untouched", new File(run, "x.spv").length() == 1 && new File(fd, String.format("g7_lagfx_%013d.txt", ms + 120)).isFile());
        G7ExecDumpExport.Result r2 = G7ExecDumpExport.export(fd, rs.get(0), "v", "h", "l", "d");
        check("a second export never replaces the first", !r2.zip.equals(r.zip) && r.zip.isFile() && r2.zip.getName().endsWith("-2.zip") && G7ExecLauncher.logFile(fd, r2.zip.getName()) != null);
        check("manifest without frame files says so", info.contains("frame_files=0"));
        Files.write(new File(run, "g7_frame_boot_args_first_1280x800.png").toPath(), new byte[] {9, 9}); Files.write(new File(run, "g7_frame_pvg_surface_1_t10s_1920x1080.png").toPath(), new byte[] {8});
        G7ExecDumpExport.Result rf = G7ExecDumpExport.export(fd, G7ExecDumpExport.listRuns(root).get(0), "v", "h", "l", "d");
        try (ZipFile z = new ZipFile(rf.zip)) {
            String inf = new String(z.getInputStream(z.getEntry("export-info.txt")).readAllBytes(), "UTF-8");
            check("frame PNGs of the run are in the ZIP under dump/ and the manifest lists them with their labels", z.getEntry("dump/g7_frame_boot_args_first_1280x800.png") != null && z.getEntry("dump/g7_frame_pvg_surface_1_t10s_1920x1080.png") != null && inf.contains("frame_files=2") && inf.contains("g7_frame_boot_args_first_1280x800.png(2 B)"));
        }
        Files.delete(new File(run, "g7_frame_boot_args_first_1280x800.png").toPath()); Files.delete(new File(run, "g7_frame_pvg_surface_1_t10s_1920x1080.png").toPath());
        // a newer APK's run-info.txt names the log exactly
        Files.write(new File(fd, String.format("g7_cboot_%013d.txt", ms - 90000)).toPath(), "C2".getBytes());   // the VM log run-info names (older than the time window would pick)
        Files.write(new File(run, G7ExecDumpExport.INFO).toPath(), ("lagfx_log=" + String.format("g7_lagfx_%013d.txt", ms + 900000) + "\nvm_log=" + String.format("g7_cboot_%013d.txt", ms - 90000) + "\nhenge_commit=RUNHENGE\nlagfx_commit=RUNLAGFX\n").getBytes());
        G7ExecDumpExport.Result r3 = G7ExecDumpExport.export(fd, G7ExecDumpExport.listRuns(root).get(0), "v", "h", "l", "d");
        String info3;
        try (ZipFile z = new ZipFile(r3.zip)) {
            check("run-info.txt wins over time matching", z.getEntry(String.format("logs/g7_lagfx_%013d.txt", ms + 900000)) != null && z.getEntry(String.format("logs/g7_lagfx_%013d.txt", ms + 120)) == null);
            check("the VM log named by run-info.txt is the one exported", z.getEntry(String.format("logs/g7_cboot_%013d.txt", ms - 90000)) != null && z.getEntry(String.format("logs/g7_cboot_%013d.txt", ms - 800)) == null);
            info3 = new String(z.getInputStream(z.getEntry("export-info.txt")).readAllBytes(), "UTF-8");
        }
        check("manifest: the run's builds come from run-info.txt, the exporter's are separate", info3.contains("run_henge_commit=RUNHENGE") && info3.contains("run_libapplegfx_vulkan_commit=RUNLAGFX") && info3.contains("exporter_henge_commit=h") && info3.contains("exporter_libapplegfx_vulkan_commit=l"));
        // ---- run-id selection: the current run is the latest clone boot; a stale dump must never stand in for it ----
        File fd2 = Files.createTempDirectory("exp2").toFile(), root2 = new File(fd2, "g7_gpudump");
        long tA = 1795760879032L, tB = 1795786974588L, tC = 1795790000000L;   // A: old run with dump (run-info), B: newer run without dump, C: newest, with dump
        String idA = G7ExecDumpExport.newId(tA + 3000);
        File runA = new File(root2, "run-" + idA); runA.mkdirs(); Files.write(new File(runA, "old.bin").toPath(), new byte[] {7}); Files.write(new File(runA, "g7_frame_old_1x1.png").toPath(), new byte[] {6});
        Files.write(new File(runA, G7ExecDumpExport.INFO).toPath(), ("run_id=" + idA + "\nlagfx_log=" + String.format("g7_lagfx_%013d.txt", tA + 3000) + "\nvm_log=" + String.format("g7_cboot_%013d.txt", tA) + "\njit_mb=64\nhenge_commit=OLDH\nlagfx_commit=OLDL\n").getBytes());
        Files.write(new File(fd2, String.format("g7_cboot_%013d.txt", tA)).toPath(), "OLDCBOOT".getBytes());
        Files.write(new File(fd2, String.format("g7_lagfx_%013d.txt", tA + 3000)).toPath(), "OLDLAGFX".getBytes());
        Files.write(new File(fd2, String.format("g7_cboot_%013d.txt", tB)).toPath(), ("g7-cboot-start: 1\ng7-gpu-options: gpu_exec=ON (x); jit_mb=256; jit_exact=OFF; vbl_exp=tick_none (LAGFX_VBL_EXP); gpu_readback=OFF (unset); wb_verify_requested=OFF; wb_verify_effective=ON (LAGFX_WB_VERIFY=1); wb_verify_reason=immediate_writeback; display events experiment ON (G7_PVG_DISPLAY_EVENTS=1); GPU data dump off\ng7-run-config: clone=clone-002; usb_mode=2\ng7-jit-config: jit_mb=256 (chosen)\n").getBytes());
        Files.write(new File(fd2, String.format("g7_lagfx_%013d.txt", tB + 2000)).toPath(), "NEWLAGFX".getBytes());
        List<G7ExecDumpExport.DumpRun> ch = G7ExecDumpExport.choices(fd2, root2);
        check("selection: the latest clone boot has no dump dir -> element 0 is a logs-only entry of THAT run, the stale dump is only offered as older", ch.size() == 2 && ch.get(0).noDump() && ch.get(0).cboot.getName().equals(String.format("g7_cboot_%013d.txt", tB)) && ch.get(1).id.equals(idA) && !ch.get(1).noDump());
        G7ExecDumpExport.Result rn = G7ExecDumpExport.export(fd2, ch.get(0), "v", "h", "l", "d");
        Set<String> nn = new TreeSet<>(); String in1, first1;
        try (ZipFile z = new ZipFile(rn.zip)) {
            for (Enumeration<? extends ZipEntry> e = z.entries(); e.hasMoreElements();) nn.add(e.nextElement().getName());
            in1 = new String(z.getInputStream(z.getEntry("export-info.txt")).readAllBytes(), "UTF-8"); first1 = in1.split("\n")[0];
        }
        check("no-dump run: only its own two logs + manifest, nothing of the old run (no dump/, no old logs, no PNG)", nn.equals(new TreeSet<>(Arrays.asList("export-info.txt", String.format("logs/g7_cboot_%013d.txt", tB), String.format("logs/g7_lagfx_%013d.txt", tB + 2000)))));
        check("no-dump run: first line of export-info.txt says current run + 'this run has no GPU dump (GPU data dump was OFF); logs only'", first1.startsWith("export_of=CURRENT RUN ") && first1.contains("this run has no GPU dump (GPU data dump was OFF); logs only") && rn.message.contains("GPU data dump was OFF") && !in1.contains(idA) && !in1.contains("OLDH") && !in1.contains(String.format("%013d", tA)));
        check("no-dump run: its own options are read from its VM log, not UNKNOWN and not invented", in1.contains("run_jit_mb=256") && in1.contains("run_vbl_exp=tick_none") && in1.contains("run_display_events=ON") && in1.contains("run_usb_mode=2") && in1.contains("run_clone=clone-002") && in1.contains("run_gpu_exec=ON") && in1.contains("wb_verify_requested=OFF") && in1.contains("wb_verify_effective=ON (LAGFX_WB_VERIFY=1)") && in1.contains("wb_verify_reason=immediate_writeback") && !in1.contains("run_jit_mb=UNKNOWN"));
        // the user explicitly picks the older run: labelled older, run id + date, on the first line
        G7ExecDumpExport.Result ro = G7ExecDumpExport.export(fd2, ch.get(1), "v", "h", "l", "d");
        String first2;
        try (ZipFile z = new ZipFile(ro.zip)) { first2 = new String(z.getInputStream(z.getEntry("export-info.txt")).readAllBytes(), "UTF-8").split("\n")[0]; check("older run still exports its own dump + logs", z.getEntry("dump/old.bin") != null && z.getEntry("dump/g7_frame_old_1x1.png") != null && z.getEntry(String.format("logs/g7_cboot_%013d.txt", tA)) != null); }
        check("older run: first line says OLDER RUN, run id and date, NOT the latest", first2.startsWith("export_of=OLDER RUN run_id=" + idA + " date=") && first2.contains("NOT the latest run") && ro.message.startsWith("이전 실행"));
        // a newest run WITH a dump dir keeps the old behaviour: its dump is element 0 and is labelled current
        String idC = G7ExecDumpExport.newId(tC + 2500); File runC = new File(root2, "run-" + idC); runC.mkdirs(); Files.write(new File(runC, "new.bin").toPath(), new byte[] {1});
        Files.write(new File(runC, G7ExecDumpExport.INFO).toPath(), ("lagfx_log=" + String.format("g7_lagfx_%013d.txt", tC + 2500) + "\nvm_log=" + String.format("g7_cboot_%013d.txt", tC) + "\n").getBytes());
        Files.write(new File(fd2, String.format("g7_cboot_%013d.txt", tC)).toPath(), "NEWESTC".getBytes()); Files.write(new File(fd2, String.format("g7_lagfx_%013d.txt", tC + 2500)).toPath(), "NEWESTL".getBytes());
        List<G7ExecDumpExport.DumpRun> ch2 = G7ExecDumpExport.choices(fd2, root2);
        check("latest run with a dump dir: it is element 0 (old behaviour), the older dump follows", ch2.size() == 2 && !ch2.get(0).noDump() && ch2.get(0).id.equals(idC) && ch2.get(1).id.equals(idA));
        try (ZipFile z = new ZipFile(G7ExecDumpExport.export(fd2, ch2.get(0), "v", "h", "l", "d").zip)) { check("latest run with dump: labelled CURRENT, its dump and logs are in the ZIP", new String(z.getInputStream(z.getEntry("export-info.txt")).readAllBytes(), "UTF-8").startsWith("export_of=CURRENT RUN run_id=" + idC) && z.getEntry("dump/new.bin") != null && z.getEntry("dump/old.bin") == null); }
        // an old-APK stale dump (no run-info) that precedes the newest boot by far is not matched to it either
        Files.delete(new File(runA, G7ExecDumpExport.INFO).toPath());
        check("old dump without run-info: matched to its own time window only, not to a later boot", !G7ExecDumpExport.belongsTo(G7ExecDumpExport.listRuns(root2).get(1), new File(fd2, String.format("g7_cboot_%013d.txt", tB))) && G7ExecDumpExport.belongsTo(G7ExecDumpExport.listRuns(root2).get(1), new File(fd2, String.format("g7_cboot_%013d.txt", tA))));
        // older cboot logs (no g7-run-config line): clone and usb mode come from the runner's own lines
        File oldLog = new File(fd2, String.format("g7_cboot_%013d.txt", tC + 5000));
        Files.write(oldLog.toPath(), ("g7-cboot-start: 1\ng7-gpu-options: gpu_exec=OFF (x); jit_mb=64; wb_verify=OFF (unset); GPU data dump off\nui_runner_vm_clone: clone folder /data/user/0/x/files/g7_clones/clones/clone-003 verified (ecid ok)\nui_runner_vm_clone usb mode 0 (A none): no virtual USB device\n").getBytes());
        G7ExecDumpExport.Result rl = G7ExecDumpExport.export(fd2, new G7ExecDumpExport.DumpRun(null, G7ExecDumpExport.newId(tC + 5000), tC + 5000, 0, 0, oldLog), "v", "h", "l", "d");
        try (ZipFile z = new ZipFile(rl.zip)) { String il = new String(z.getInputStream(z.getEntry("export-info.txt")).readAllBytes(), "UTF-8");
            check("older log: clone and usb mode are read from the runner's existing lines, not UNKNOWN", il.contains("run_clone=clone-003") && il.contains("run_usb_mode=0") && il.contains("wb_verify_effective=UNKNOWN (older log: only 'wb_verify=OFF'")); }
        File bothLog = new File(fd2, String.format("g7_cboot_%013d.txt", tC + 6000));
        Files.write(bothLog.toPath(), ("g7-run-config: clone=clone-009; usb_mode=2\nui_runner_vm_clone: clone folder /x/clones/clone-003 verified\nui_runner_vm_clone usb mode 0 (A none): x\n").getBytes());
        try (ZipFile z = new ZipFile(G7ExecDumpExport.export(fd2, new G7ExecDumpExport.DumpRun(null, G7ExecDumpExport.newId(tC + 6000), tC + 6000, 0, 0, bothLog), "v", "h", "l", "d").zip)) { String ib = new String(z.getInputStream(z.getEntry("export-info.txt")).readAllBytes(), "UTF-8");
            check("g7-run-config is preferred over the old runner lines", ib.contains("run_clone=clone-009") && ib.contains("run_usb_mode=2")); }
        check("no clone-boot log at all: the old listing (newest dump first) is returned", G7ExecDumpExport.choices(Files.createTempDirectory("exp3").toFile(), root2).size() == 2);
        System.out.println(failed == 0 ? "G7DUMPEXPORT OK" : "G7DUMPEXPORT FAILED " + failed);
        System.exit(failed == 0 ? 0 : 1);
    }
}
