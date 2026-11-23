package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** G7Isolation on a plain JVM with mock files in temp directories: nothing real is touched. */
public class G7ExecIsolationTest {
    static int fails = 0;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }
    static final G7Isolation.Mover OK = (a, b) -> Files.move(a, b, java.nio.file.StandardCopyOption.ATOMIC_MOVE);

    static File vm(byte[] disk, byte[] aux, String state) throws IOException {
        File d = Files.createTempDirectory("vm").toFile();
        Files.write(new File(d, "macos_disk.img").toPath(), disk);
        Files.write(new File(d, "macos_aux.img").toPath(), aux);
        Files.write(new File(d, "g7_state.txt").toPath(), state.getBytes());
        Files.write(new File(d, "macos_aux.initial").toPath(), new byte[] {5, 5, 5});
        Files.write(new File(d, "vm_ecid.txt").toPath(), "ecid=0x1\n".getBytes());
        Files.write(new File(d, "UniversalMac_15.6.1_24G90_Restore.ipsw").toPath(), new byte[] {9, 9});
        return d;
    }
    static String snapshot(File d) throws IOException {
        List<String> l = new ArrayList<>();
        try (java.util.stream.Stream<Path> w = Files.walk(d.toPath())) {
            w.filter(Files::isRegularFile).forEach(p -> { try { l.add(d.toPath().relativize(p) + ":" + Files.size(p) + ":" + Arrays.hashCode(Files.readAllBytes(p))); } catch (IOException e) { l.add("ERR"); } });
        }
        java.util.Collections.sort(l);
        return l.toString();
    }
    static String content(File d) throws IOException {   // names + sizes + content of every file with the path made relative to its archive
        return snapshot(d);
    }
    static final java.util.function.BooleanSupplier NOT_LIVE = () -> false;

    public static void main(String[] a) throws Exception {
        byte[] disk = new byte[5000], aux = new byte[700]; Arrays.fill(disk, (byte) 7); Arrays.fill(aux, (byte) 3);

        // normal: renamed into archives/g7-archive-001, same bytes, nothing else touched, journal moved into the archive
        File vm = vm(disk, aux, "state=in_progress\necid_tag=eb5a591ec138b9b0\n");
        String before = snapshot(vm);
        G7Isolation.Result r = G7Isolation.isolateWith(OK, true, NOT_LIVE, vm, 0);
        File ar = new File(vm, "archives/g7-archive-001");
        check("isolation ok", r.ok && r.message.contains("g7-archive-001"));
        check("the three files are in the archive with the same bytes, not in vm/ any more",
                Arrays.equals(Files.readAllBytes(new File(ar, "macos_disk.img").toPath()), disk) && Arrays.equals(Files.readAllBytes(new File(ar, "macos_aux.img").toPath()), aux)
                && new String(Files.readAllBytes(new File(ar, "g7_state.txt").toPath())).startsWith("state=in_progress")
                && !new File(vm, "macos_disk.img").exists() && !new File(vm, "macos_aux.img").exists() && !new File(vm, "g7_state.txt").exists());
        check("macos_aux.initial is moved with the pair (4 files), nothing of the pair is left in vm/", new File(ar, "macos_aux.initial").length() == 3 && !new File(vm, "macos_aux.initial").exists() && r.message.contains("4개"));
        check("vm_ecid.txt and the IPSW are untouched", new File(vm, "vm_ecid.txt").isFile() && new File(vm, "UniversalMac_15.6.1_24G90_Restore.ipsw").length() == 2);
        check("the journal is gone from archives/ and kept in the archive as ISOLATION_DONE.txt; not pending", !G7Isolation.pending(vm) && new File(ar, "ISOLATION_DONE.txt").isFile() && !new File(vm, "archives/isolating.txt").exists());
        check("nothing was deleted or changed: every original file is found again with the same size and content",
                before.contains("macos_disk.img:5000") && snapshot(ar).contains("macos_disk.img:5000:" + Arrays.hashCode(disk)) && snapshot(ar).contains("macos_aux.img:700:" + Arrays.hashCode(aux)));
        check("archive count is 1", G7Isolation.archiveCount(vm) == 1);

        // a second pair next time goes to 002; an existing archive is never used or overwritten
        Files.write(new File(vm, "macos_disk.img").toPath(), new byte[] {1}); Files.write(new File(vm, "macos_aux.img").toPath(), new byte[] {2}); Files.write(new File(vm, "g7_state.txt").toPath(), "state=done\n".getBytes());
        String arBefore = snapshot(ar);
        r = G7Isolation.isolateWith(OK, true, NOT_LIVE, vm, 0);
        check("a second isolation goes to g7-archive-002 and leaves 001 as it was", r.ok && new File(vm, "archives/g7-archive-002/macos_disk.img").length() == 1 && snapshot(ar).equals(arBefore));

        // refusals: nothing is moved
        File v2 = vm(disk, aux, "state=in_progress\n"); String s2 = snapshot(v2);
        check("switch off: refused, nothing moved", !G7Isolation.isolateWith(OK, false, NOT_LIVE, v2, 0).ok && snapshot(v2).equals(s2) && !new File(v2, "archives").exists());
        check("a live run of this app: refused, nothing moved", !G7Isolation.isolateWith(OK, true, () -> true, v2, 0).ok && snapshot(v2).equals(s2));
        check("runner process found: refused, nothing moved", !G7Isolation.isolateWith(OK, true, NOT_LIVE, v2, 1).ok && snapshot(v2).equals(s2));
        check("runner process status unknown: refused, nothing moved", !G7Isolation.isolateWith(OK, true, NOT_LIVE, v2, -1).ok && snapshot(v2).equals(s2));
        new File(v2, "macos_aux.img").delete();
        r = G7Isolation.isolateWith(OK, true, NOT_LIVE, v2, 0);
        check("a member that is missing is simply not moved; the others are (3 files)", r.ok && !new File(v2, "macos_disk.img").exists() && new File(v2, "archives/g7-archive-001/macos_disk.img").isFile() && !new File(v2, "archives/g7-archive-001/macos_aux.img").exists());
        // the leftover case seen on the device: the state file, disk and AUX are already aside, only macos_aux.initial is still in vm/
        File vL = vm(disk, aux, "state=in_progress\n");
        for (String n : new String[] {"macos_disk.img", "macos_aux.img", "g7_state.txt"}) new File(vL, n).delete();
        r = G7Isolation.isolateWith(OK, true, NOT_LIVE, vL, 0);
        check("only macos_aux.initial left over: it is isolated (1 file)", r.ok && new File(vL, "archives/g7-archive-001/macos_aux.initial").length() == 3 && !new File(vL, "macos_aux.initial").exists());
        File vE = Files.createTempDirectory("vme").toFile();
        check("nothing of the pair exists: refused", !G7Isolation.isolateWith(OK, true, NOT_LIVE, vE, 0).ok && !new File(vE, "archives").exists());
        File v3 = vm(disk, aux, "state=in_progress\n");
        new File(v3, "macos_aux.img").delete(); Files.createSymbolicLink(new File(v3, "macos_aux.img").toPath(), new File(v3, "vm_ecid.txt").toPath());
        check("a symbolic link instead of a file: refused, nothing moved", !G7Isolation.isolateWith(OK, true, NOT_LIVE, v3, 0).ok && new File(v3, "macos_disk.img").isFile());
        File v4 = vm(disk, aux, "state=in_progress\n");
        Files.createDirectories(new File(v4, "archives").toPath()); Files.createSymbolicLink(new File(v4, "archives/g7-archive-001").toPath(), v4.toPath());
        r = G7Isolation.isolateWith(OK, true, NOT_LIVE, v4, 0);
        check("a name that is already taken (even a link) is skipped, never followed or overwritten: goes to 002", r.ok && new File(v4, "archives/g7-archive-002/macos_disk.img").isFile());

        // the whole action holds the start lock (no start can happen at the same time)
        File v5 = vm(disk, aux, "state=in_progress\n"); boolean[] held = {true};
        G7Isolation.isolateWith((x, y) -> { held[0] &= Thread.holdsLock(G7ExecLauncher.START_LOCK); OK.move(x, y); }, true, NOT_LIVE, v5, 0);
        check("the moves run under the start lock", held[0]);

        // the runner check runs inside the start lock and only after the live-run check
        File vA = vm(disk, aux, "state=in_progress\n"); boolean[] inLock = {false}; int[] calls = {0};
        G7Isolation.isolateWith(OK, true, NOT_LIVE, vA, (java.util.function.IntSupplier) () -> { calls[0]++; inLock[0] = Thread.holdsLock(G7ExecLauncher.START_LOCK); return 0; });
        check("the runner check is made once, inside the start lock", calls[0] == 1 && inLock[0]);
        File vB = vm(disk, aux, "state=in_progress\n"); int[] cb = {0};
        G7Isolation.isolateWith(OK, true, () -> true, vB, (java.util.function.IntSupplier) () -> { cb[0]++; return 0; });
        check("a live run stops before the runner is even looked at", cb[0] == 0);

        // a failed move: rolled back, nothing lost, not pending
        File v6 = vm(disk, aux, "state=in_progress\n"); String s6 = snapshot(v6);
        int[] n6 = {0};
        r = G7Isolation.isolateWith((x, y) -> { if (++n6[0] == 2) throw new IOException("boom"); OK.move(x, y); }, true, NOT_LIVE, v6, 0);
        check("failure on the 2nd move: everything moved back, the pair is as before, not pending", !r.ok && r.message.contains("되돌렸") && !G7Isolation.pending(v6)
                && new File(v6, "macos_disk.img").length() == 5000 && new File(v6, "macos_aux.img").length() == 700 && new File(v6, "g7_state.txt").isFile() && new File(v6, "archives/g7-archive-001/ROLLED_BACK.txt").isFile());
        check("the rolled-back pair can be isolated afterwards (the failed attempt used archive 001, this one 002)", G7Isolation.isolateWith(OK, true, NOT_LIVE, v6, 0).ok && new File(v6, "archives/g7-archive-002/macos_disk.img").length() == 5000);

        // failure + failed rollback (like an app killed in the middle): pending, blocks the start, recover finishes the move
        File v7 = vm(disk, aux, "state=in_progress\n");
        int[] n7 = {0};
        r = G7Isolation.isolateWith((x, y) -> { n7[0]++; if (n7[0] >= 2) throw new IOException("killed"); OK.move(x, y); }, true, NOT_LIVE, v7, 0);
        check("failure and failed rollback: stays unfinished, says so", !r.ok && r.message.contains("끝나지 않은") && G7Isolation.pending(v7));
        check("one file in the archive, two still in vm/ (nothing lost)", new File(v7, "archives/g7-archive-001/macos_disk.img").length() == 5000 && new File(v7, "macos_aux.img").length() == 700 && new File(v7, "g7_state.txt").isFile());
        check("a new isolation is refused while one is unfinished", !G7Isolation.isolateWith(OK, true, NOT_LIVE, v7, 0).ok && G7Isolation.pending(v7));
        check("recover is refused while a runner is found / unknown, or with the switch off", !G7Isolation.recoverWith(OK, true, NOT_LIVE, v7, 1).ok && !G7Isolation.recoverWith(OK, true, NOT_LIVE, v7, -1).ok && !G7Isolation.recoverWith(OK, false, NOT_LIVE, v7, 0).ok && G7Isolation.pending(v7));
        r = G7Isolation.recoverWith(OK, true, NOT_LIVE, v7, 0);
        check("recover moves only what is still in vm/, checks the sizes and closes the journal", r.ok && !G7Isolation.pending(v7) && new File(v7, "archives/g7-archive-001/macos_aux.img").length() == 700
                && new File(v7, "archives/g7-archive-001/g7_state.txt").isFile() && !new File(v7, "macos_aux.img").exists() && new File(v7, "archives/g7-archive-001/ISOLATION_RECOVERED.txt").isFile());
        check("recover with nothing unfinished says so", !G7Isolation.recoverWith(OK, true, NOT_LIVE, v7, 0).ok);

        // recover stops on doubt: a file in both places, a file nowhere, a wrong size
        File v8 = vm(disk, aux, "state=in_progress\n"); int[] n8 = {0};
        G7Isolation.isolateWith((x, y) -> { n8[0]++; if (n8[0] >= 2) throw new IOException("killed"); OK.move(x, y); }, true, NOT_LIVE, v8, 0);
        Files.write(new File(v8, "macos_disk.img").toPath(), disk);   // the same name now also exists in vm/
        String s8 = snapshot(v8);
        check("recover: a file in both places stops it, nothing moved", !G7Isolation.recoverWith(OK, true, NOT_LIVE, v8, 0).ok && snapshot(v8).equals(s8) && G7Isolation.pending(v8));
        new File(v8, "macos_disk.img").delete();
        File moved = new File(v8, "archives/g7-archive-001/macos_disk.img"); byte[] keep = Files.readAllBytes(moved.toPath());
        Files.write(moved.toPath(), new byte[3]);
        check("recover: a wrong size stops it, nothing moved", !G7Isolation.recoverWith(OK, true, NOT_LIVE, v8, 0).ok && new File(v8, "macos_aux.img").isFile() && G7Isolation.pending(v8));
        moved.delete();
        check("recover: a file that is nowhere stops it, nothing moved", !G7Isolation.recoverWith(OK, true, NOT_LIVE, v8, 0).ok && new File(v8, "macos_aux.img").isFile() && G7Isolation.pending(v8));

        // the target is never replaced, even by a mover that would replace (rename replaces silently)
        File v9 = vm(disk, aux, "state=in_progress\n");
        Files.createDirectories(new File(v9, "archives/g7-archive-001").toPath());
        Files.write(new File(v9, "archives/g7-archive-001/x").toPath(), new byte[1]);
        check("an existing archive folder is skipped (002 used), 001 untouched", G7Isolation.isolateWith(OK, true, NOT_LIVE, v9, 0).ok && new File(v9, "archives/g7-archive-001/x").length() == 1);

        // preflight: an unfinished isolation blocks the start (not taken for a clean state); a finished one does not
        File lib = Files.createTempDirectory("lib").toFile(), ext = Files.createTempDirectory("ext").toFile();
        Files.write(new File(lib, G7ExecLauncher.LIB_NAME).toPath(), new byte[] {1});
        File pv = Files.createTempDirectory("pvv").toFile();
        Files.write(new File(pv, "AVPBooter.vmapple2.bin").toPath(), new byte[] {1}); Files.write(new File(pv, "vm_ecid.txt").toPath(), "ecid=0x13579bdf024681\n".getBytes());
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(new File(pv, G7ExecLauncher.IPSW_NAME), "rw")) { f.setLength(G7ExecLauncher.IPSW_BYTES); }
        File rom = new File(pv, "AVPBooter.vmapple2.bin");
        check("preflight is ready with no pair and no journal", G7ExecLauncher.preflightWith((e, p) -> rom, lib, ext, pv).ready);
        Files.createDirectories(new File(pv, "archives").toPath()); Files.write(new File(pv, "archives/isolating.txt").toPath(), "archive=g7-archive-001\n".getBytes());
        G7ExecLauncher.Preflight pf = G7ExecLauncher.preflightWith((e, p) -> rom, lib, ext, pv);
        check("preflight: an unfinished isolation blocks the start and says so", !pf.ready && pf.text.contains("격리") && pf.text.contains("복구"));
        String sm = G7ExecLauncher.summary(Files.createTempDirectory("f").toFile(), pv);
        check("the summary shows the unfinished isolation", sm.contains("격리: 끝나지 않음"));

        System.out.println(fails == 0 ? "G7EXEC ISOLATION OK (0 failed)" : "G7EXEC ISOLATION FAILED (" + fails + ")");
        System.exit(fails == 0 ? 0 : 1);
    }
}
