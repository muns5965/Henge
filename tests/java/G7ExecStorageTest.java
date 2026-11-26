package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

/** G7Storage (storage list and deletion of failed install pairs) on a plain JVM with mock files in temp directories: nothing real is touched. */
public class G7ExecStorageTest {
    static int fails = 0;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }
    static final java.util.function.BooleanSupplier NOT_LIVE = () -> false;
    static final G7Storage.Sizer SZ = f -> f.length();              // stand-in for "allocated blocks"
    static final G7Storage.Deleter OK = Files::delete;

    static void put(File d, String n, int len) throws IOException { Files.write(new File(d, n).toPath(), new byte[len]); }
    static void state(File d, String s) throws IOException { Files.write(new File(d, "g7_state.txt").toPath(), (s + "\necid_tag=ab\n").getBytes()); }
    static File vm() throws IOException {
        File d = Files.createTempDirectory("vm").toFile();
        Files.write(new File(d, "vm_ecid.txt").toPath(), "ecid=0x1\n".getBytes());
        Files.write(new File(d, "UniversalMac_15.6.1_24G90_Restore.ipsw").toPath(), new byte[] {9, 9});
        return d;
    }
    static File archive(File vm, String name, String st, int disk) throws IOException {
        File a = new File(new File(vm, "archives"), name); a.mkdirs();
        put(a, "macos_disk.img", disk); put(a, "macos_aux.img", 50); put(a, "macos_aux.initial", 3);
        if (st != null) state(a, st);
        Files.write(new File(a, "ISOLATION_DONE.txt").toPath(), "done".getBytes());
        return a;
    }
    static String snap(File d) throws IOException {
        List<String> l = new ArrayList<>();
        try (java.util.stream.Stream<Path> w = Files.walk(d.toPath())) { w.forEach(p -> { try { l.add(d.toPath().relativize(p) + (Files.isRegularFile(p) ? ":" + Files.size(p) : "/")); } catch (IOException e) { l.add("ERR"); } }); }
        java.util.Collections.sort(l); return l.toString();
    }
    static List<String> names(String... n) { return new ArrayList<>(Arrays.asList(n)); }

    public static void main(String[] x) throws Exception {
        // ---- list: real size, state, protection
        File vm = vm();
        archive(vm, "g7-archive-001", "state=in_progress", 4000);
        archive(vm, "g7-archive-002", "state=done", 900);
        File a3 = archive(vm, "g7-archive-003", "state=in_progress", 100); put(a3, "notes.bin", 1);   // an entry we do not know
        File a4 = archive(vm, "g7-archive-004", null, 100);                                           // no state file but a disk
        List<G7Storage.Item> li = G7Storage.list(vm, SZ);
        check("list: 4 archives, sizes are the allocated sizes (disk+aux+aux.initial+note+state)", li.size() == 4 && li.get(0).allocated == 4000 + 50 + 3 + 4 + new File(vm, "archives/g7-archive-001/g7_state.txt").length());
        check("list: in_progress is deletable, done is not, unknown entry is not, missing state with a disk is not",
                li.get(0).deletable && !li.get(1).deletable && li.get(1).note.contains("정상 설치") && !li.get(2).deletable && !li.get(3).deletable);
        check("list: a date is shown (from ISOLATION_DONE.txt)", li.get(0).createdMs > 0);
        check("fmt: GB and MB", G7Storage.fmt(12L << 30).equals("12.0 GB") && G7Storage.fmt(5L << 20).equals("5.0 MB"));
        check("free space: below the minimum is refused with the numbers, enough passes", !G7Storage.enoughFor(1L << 30).isEmpty() && G7Storage.enoughFor(G7Storage.MIN_FREE_BYTES).isEmpty());

        // ---- delete one archive: only its own members, the folder goes, everything else stays
        String keep = snap(new File(vm, "archives/g7-archive-002")) + snap(a3) + snap(a4);
        G7Isolation.Result dummy = null;
        G7Storage.Result r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vm, names("g7-archive-001"), () -> 0, SZ);
        check("delete: ok, archive 001 is gone completely", r.ok && !new File(vm, "archives/g7-archive-001").exists());
        check("delete: the other archives, the ECID file and the IPSW are untouched", (snap(new File(vm, "archives/g7-archive-002")) + snap(a3) + snap(a4)).equals(keep) && new File(vm, "vm_ecid.txt").isFile() && new File(vm, "UniversalMac_15.6.1_24G90_Restore.ipsw").length() == 2);

        // ---- refusals: done, unknown entry, no state; bad names; traversal; symlink
        String before = snap(vm);
        r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vm, names("g7-archive-002", "g7-archive-003", "g7-archive-004"), () -> 0, SZ);
        check("refused: done / unknown entry / no state file are not deleted and the result says so", !r.ok && snap(vm).equals(before) && r.message.contains("정상 설치") && r.message.contains("notes.bin") && r.message.contains("상태를 확인할 수 없어"));
        r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vm, names("../vm", "g7-archive-001/../g7-archive-002", "archives", ""), () -> 0, SZ);
        check("refused: names that are not archive folder names (traversal) do nothing", !r.ok && snap(vm).equals(before));
        File outside = Files.createTempDirectory("outside").toFile(); put(outside, "macos_disk.img", 77); state(outside, "state=in_progress");
        Files.createSymbolicLink(new File(vm, "archives/g7-archive-009").toPath(), outside.toPath());
        String outBefore = snap(outside);
        r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vm, names("g7-archive-009"), () -> 0, SZ);
        check("symlinked archive folder is not followed: the target is untouched", !r.ok && snap(outside).equals(outBefore));
        Files.delete(new File(vm, "archives/g7-archive-009").toPath());
        File a5 = archive(vm, "g7-archive-005", "state=in_progress", 10);
        File sl = new File(a5, "macos_aux.initial"); Files.delete(sl.toPath()); Files.createSymbolicLink(sl.toPath(), new File(outside, "macos_disk.img").toPath());
        r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vm, names("g7-archive-005"), () -> 0, SZ);
        check("a symlink member stops the archive; the link target is untouched", !r.ok && snap(outside).equals(outBefore) && a5.exists());

        // ---- gates: nothing is deleted
        File vg = vm(); File ag = archive(vg, "g7-archive-001", "state=in_progress", 100); String sg = snap(vg);
        check("gate: a live run blocks", !G7Storage.deleteArchivesWith(OK, true, () -> true, vg, names("g7-archive-001"), () -> 0, SZ).ok && snap(vg).equals(sg));
        check("gate: runner found blocks", !G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vg, names("g7-archive-001"), () -> 1, SZ).ok && snap(vg).equals(sg));
        check("gate: runner status unknown blocks", !G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vg, names("g7-archive-001"), () -> -1, SZ).ok && snap(vg).equals(sg));
        check("gate: start switch off blocks", !G7Storage.deleteArchivesWith(OK, false, NOT_LIVE, vg, names("g7-archive-001"), () -> 0, SZ).ok && snap(vg).equals(sg));
        Files.write(new File(vg, "archives/isolating.txt").toPath(), "archive=g7-archive-001\n".getBytes());
        r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vg, names("g7-archive-001"), () -> 0, SZ);
        check("gate: an unfinished isolation (journal) blocks every deletion", !r.ok && r.message.contains("끝나지 않은 격리") && new File(ag, "macos_disk.img").exists());
        File vc0 = vm(); put(vc0, "macos_disk.img", 10); state(vc0, "state=in_progress"); Files.write(new File(vc0, "archives").toPath().resolve("x").getParent().toFile().mkdirs() ? new File(vc0, "archives/isolating.txt").toPath() : null, "a".getBytes());
        check("gate: the same for the pair in vm/", !G7Storage.deleteCurrentWith(OK, true, NOT_LIVE, vc0, () -> 0).ok && new File(vc0, "macos_disk.img").exists());

        // ---- partial deletion: the deleter fails on the 2nd file; the state file stays; running again finishes
        File vp = vm(); File ap = archive(vp, "g7-archive-001", "state=in_progress", 300);
        AtomicInteger calls = new AtomicInteger();
        G7Storage.Deleter failSecond = p -> { if (calls.incrementAndGet() == 2) throw new IOException("disk error"); Files.delete(p); };
        r = G7Storage.deleteArchivesWith(failSecond, true, NOT_LIVE, vp, names("g7-archive-001"), () -> 0, SZ);
        check("partial: the failure is reported, disk is gone, the state file is still there (the pair still says in_progress)", !r.ok && !new File(ap, "macos_disk.img").exists() && new File(ap, "macos_aux.img").exists() && new File(ap, "g7_state.txt").exists());
        check("partial: the list still offers it for deletion", G7Storage.list(vp, SZ).get(0).deletable);
        r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vp, names("g7-archive-001"), () -> 0, SZ);
        check("partial: deleting again finishes it", r.ok && !ap.exists());
        // an app that ended between two files: only the disk was deleted, nothing else changed
        File ap2 = archive(vp, "g7-archive-002", "state=in_progress", 300); Files.delete(new File(ap2, "macos_disk.img").toPath());
        check("killed in the middle: the rest can be deleted afterwards", G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vp, names("g7-archive-002"), () -> 0, SZ).ok && !ap2.exists());
        // the folder itself cannot be removed: files are gone, the next call cleans up
        File ap3 = archive(vp, "g7-archive-003", "state=in_progress", 20);
        G7Storage.Deleter failDir = p -> { if (Files.isDirectory(p)) throw new IOException("busy"); Files.delete(p); };
        r = G7Storage.deleteArchivesWith(failDir, true, NOT_LIVE, vp, names("g7-archive-003"), () -> 0, SZ);
        check("partial: files deleted, folder not: reported, and only an empty folder is left", !r.ok && ap3.isDirectory() && ap3.list().length == 0);
        check("partial: the empty leftover is still offered and is cleaned by the next deletion", G7Storage.list(vp, SZ).get(0).deletable && G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vp, names("g7-archive-003"), () -> 0, SZ).ok && !ap3.exists());
        // the state file is deleted LAST
        File vo = vm(); archive(vo, "g7-archive-001", "state=in_progress", 10); List<String> order = new ArrayList<>();
        G7Storage.deleteArchivesWith(p -> { order.add(p.getFileName().toString()); Files.delete(p); }, true, NOT_LIVE, vo, names("g7-archive-001"), () -> 0, SZ);
        check("order: disk first, state file last before the folder", order.get(0).equals("macos_disk.img") && order.get(order.size() - 2).equals("g7_state.txt") && order.get(order.size() - 1).equals("g7-archive-001"));

        // ---- delete all deletable (the caller passes what the list marks deletable); done stays
        File vall = vm(); archive(vall, "g7-archive-001", "state=in_progress", 10); archive(vall, "g7-archive-002", "state=done", 10); archive(vall, "g7-archive-003", "state=in_progress", 10);
        List<String> del = new ArrayList<>(); for (G7Storage.Item it : G7Storage.list(vall, SZ)) if (it.deletable) del.add(it.name);
        r = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vall, del, () -> 0, SZ);
        check("delete all: both failed pairs go, the finished one stays", r.ok && !new File(vall, "archives/g7-archive-001").exists() && !new File(vall, "archives/g7-archive-003").exists() && new File(vall, "archives/g7-archive-002/macos_disk.img").exists());

        // ---- delete current (no isolation)
        File vc = vm(); put(vc, "macos_disk.img", 500); put(vc, "macos_aux.img", 40); put(vc, "macos_aux.initial", 3); state(vc, "state=in_progress"); archive(vc, "g7-archive-001", "state=done", 5);
        List<String> ord2 = new ArrayList<>();
        r = G7Storage.deleteCurrentWith(p -> { ord2.add(p.getFileName().toString()); Files.delete(p); }, true, NOT_LIVE, vc, () -> 0);
        check("current: the four pair files are deleted, state file last", r.ok && ord2.equals(Arrays.asList("macos_disk.img", "macos_aux.img", "macos_aux.initial", "g7_state.txt")));
        check("current: ECID file, IPSW and the archives are untouched; the preflight sees a clean state", new File(vc, "vm_ecid.txt").isFile() && new File(vc, "UniversalMac_15.6.1_24G90_Restore.ipsw").isFile() && new File(vc, "archives/g7-archive-001/macos_disk.img").isFile() && !new File(vc, "macos_disk.img").exists() && !new File(vc, "g7_state.txt").exists());
        check("current: nothing left to delete is said plainly", !G7Storage.deleteCurrentWith(OK, true, NOT_LIVE, vc, () -> 0).ok);
        File vd = vm(); put(vd, "macos_disk.img", 5); put(vd, "macos_aux.img", 5); state(vd, "state=done");
        r = G7Storage.deleteCurrentWith(OK, true, NOT_LIVE, vd, () -> 0);
        check("current: a finished (done) pair is never deleted", !r.ok && new File(vd, "macos_disk.img").exists() && new File(vd, "g7_state.txt").exists());
        File vu = vm(); put(vu, "macos_disk.img", 5);
        check("current: a disk without a readable state is not deleted", !G7Storage.deleteCurrentWith(OK, true, NOT_LIVE, vu, () -> 0).ok && new File(vu, "macos_disk.img").exists());
        File vl = vm(); put(vl, "macos_disk.img", 5); state(vl, "state=in_progress");
        check("current: live run blocks", !G7Storage.deleteCurrentWith(OK, true, () -> true, vl, () -> 0).ok && new File(vl, "macos_disk.img").exists());
        check("current: runner found blocks", !G7Storage.deleteCurrentWith(OK, true, NOT_LIVE, vl, () -> 1).ok && new File(vl, "macos_disk.img").exists());
        File vs = vm(); File real = new File(vs, "real.bin"); put(vs, "real.bin", 5); state(vs, "state=in_progress");
        Files.createSymbolicLink(new File(vs, "macos_disk.img").toPath(), real.toPath());
        check("current: a symlinked disk is refused and its target is untouched", !G7Storage.deleteCurrentWith(OK, true, NOT_LIVE, vs, () -> 0).ok && real.exists() && new File(vs, "g7_state.txt").exists());
        File vf = vm(); put(vf, "macos_disk.img", 5); put(vf, "macos_aux.img", 5); state(vf, "state=in_progress");
        r = G7Storage.deleteCurrentWith(p -> { if (p.getFileName().toString().equals("macos_aux.img")) throw new IOException("io"); Files.delete(p); }, true, NOT_LIVE, vf, () -> 0);
        check("current: a failure stops, says what happened, the state file stays so it can be deleted again", !r.ok && new File(vf, "g7_state.txt").exists() && G7Storage.deleteCurrentWith(OK, true, NOT_LIVE, vf, () -> 0).ok);

        // ---- race with a start: deletion waits for the start lock; two deletions of the same archive at once
        File vr = vm(); archive(vr, "g7-archive-001", "state=in_progress", 200);
        final Object lock = G7ExecLauncher.START_LOCK; final boolean[] done = {false}; final G7Storage.Result[] res = new G7Storage.Result[2];
        Thread holder; final java.util.concurrent.CountDownLatch locked = new java.util.concurrent.CountDownLatch(1), release = new java.util.concurrent.CountDownLatch(1);
        holder = new Thread(() -> { synchronized (lock) { locked.countDown(); try { release.await(); } catch (InterruptedException e) { } } }); holder.start(); locked.await();
        Thread t1 = new Thread(() -> res[0] = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, vr, names("g7-archive-001"), () -> 0, SZ));
        t1.start(); Thread.sleep(300);
        check("race: while a start holds the lock nothing is deleted", t1.isAlive() && new File(vr, "archives/g7-archive-001/macos_disk.img").exists());
        release.countDown(); t1.join(5000); holder.join();
        check("race: after the lock is free the deletion runs", res[0] != null && res[0].ok && !new File(vr, "archives/g7-archive-001").exists());
        File v2 = vm(); archive(v2, "g7-archive-001", "state=in_progress", 200);
        Thread ta = new Thread(() -> res[0] = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, v2, names("g7-archive-001"), () -> 0, SZ)), tb = new Thread(() -> res[1] = G7Storage.deleteArchivesWith(OK, true, NOT_LIVE, v2, names("g7-archive-001"), () -> 0, SZ));
        ta.start(); tb.start(); ta.join(); tb.join();
        check("race: two deletions of the same folder at once: no error, folder gone, ECID/IPSW intact", res[0] != null && res[1] != null && !new File(v2, "archives/g7-archive-001").exists() && new File(v2, "vm_ecid.txt").isFile());

        // ---- the check before a start (low space): once the preflight passes (sparse IPSW stand-in of the right size, ROM stand-in), too little space refuses before anything is created
        File lib = Files.createTempDirectory("lib").toFile(), ext = Files.createTempDirectory("ext").toFile(), files = Files.createTempDirectory("files").toFile(), pv = vm();
        Files.write(new File(lib, "libg7runner.so").toPath(), new byte[] {1});
        Files.write(new File(pv, "vm_ecid.txt").toPath(), "ecid=0x123456789ab\n".getBytes());   // a stand-in inside the valid range (>= 2^40)
        Files.delete(new File(pv, G7ExecLauncher.IPSW_NAME).toPath());
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(new File(pv, G7ExecLauncher.IPSW_NAME), "rw")) { f.setLength(G7ExecLauncher.IPSW_BYTES); }   // sparse: no real space
        final File romFile = new File(ext, "rom.bin"); Files.write(romFile.toPath(), new byte[] {1});
        G7ExecLauncher.Start st = G7ExecLauncher.startWith(c -> { throw new IOException("must not be started"); }, (e, p) -> romFile, true, lib, ext, pv, files, true, "0".repeat(64), () -> 2L << 30);
        check("start: too little free space -> refused with the numbers, nothing created, no process", !st.started && st.message.contains("저장공간이 부족") && files.list().length == 0 && !new File(pv, "macos_disk.img").exists());
        st = G7ExecLauncher.startWith(c -> { throw new IOException("must not be started"); }, (e, p) -> romFile, true, lib, ext, pv, files, true, "0".repeat(64), () -> G7Storage.MIN_FREE_BYTES);
        check("start: enough space -> it goes on past the space check (here it stops at the stand-in spawner)", !st.started && !st.message.contains("저장공간이 부족"));

        // ---- delete ONE clone: only that folder; the install pair and the other clones stay; live boot/copy/run block it
        File vcl = vm(); put(vcl, "macos_disk.img", 500); put(vcl, "g7_state.txt", 3);
        File cl = new File(vcl, "clones"); File c1 = new File(cl, "clone-001"), c2 = new File(cl, "clone-002"); c1.mkdirs(); c2.mkdirs();
        for (File c : new File[] {c1, c2}) { put(c, "macos_disk.img", 900); put(c, "macos_aux.img", 40); put(c, "macos_aux.initial", 4); put(c, "g7_state.txt", 3); put(c, "clone_state.txt", 5); }
        List<G7Storage.Item> cli = G7Storage.listClones(vcl, SZ);
        check("clones: list shows both with real size", cli.size() == 2 && cli.get(1).name.equals("clone-002") && cli.get(1).allocated == 900 + 40 + 4 + 3 + 5);

        String c1snap = snap(c1);
        check("clones: a live boot/copy blocks it", !G7Storage.deleteCloneWith(OK, true, NOT_LIVE, () -> true, vcl, "clone-002", () -> 0).ok && c2.exists());
        check("clones: a live install run blocks it", !G7Storage.deleteCloneWith(OK, true, () -> true, () -> false, vcl, "clone-002", () -> 0).ok && c2.exists());
        check("clones: bad names are refused (path tricks)", !G7Storage.deleteCloneWith(OK, true, NOT_LIVE, () -> false, vcl, "../vm", () -> 0).ok && !G7Storage.deleteCloneWith(OK, true, NOT_LIVE, () -> false, vcl, "clone-002/..", () -> 0).ok && c2.exists());
        List<String> ordc = new ArrayList<>();
        r = G7Storage.deleteCloneWith(p -> { ordc.add(p.getFileName().toString()); Files.delete(p); }, true, NOT_LIVE, () -> false, vcl, "clone-002", () -> 0);
        check("clones: deleted, marker first, folder gone", r.ok && !c2.exists() && ordc.get(0).equals("clone_state.txt") && ordc.get(ordc.size() - 1).equals("clone-002"));
        check("clones: clone-001 and the install pair untouched", snap(c1).equals(c1snap) && new File(vcl, "macos_disk.img").length() == 500 && new File(vcl, "g7_state.txt").exists());
        File c3 = new File(cl, "clone-003"); c3.mkdirs(); put(c3, "macos_disk.img", 5); new File(c3, "sub").mkdirs();
        check("clones: a sub-folder inside stops it with nothing deleted", !G7Storage.deleteCloneWith(OK, true, NOT_LIVE, () -> false, vcl, "clone-003", () -> 0).ok && new File(c3, "macos_disk.img").exists());
        File c4 = new File(cl, "clone-004"); c4.mkdirs(); put(c4, "clone_state.txt", 1); put(c4, "macos_disk.img", 5);
        r = G7Storage.deleteCloneWith(p -> { if (p.getFileName().toString().equals("macos_disk.img")) throw new IOException("x"); Files.delete(p); }, true, NOT_LIVE, () -> false, vcl, "clone-004", () -> 0);
        check("clones: an error in the middle leaves a folder without marker (no longer a clone) and says so", !r.ok && !new File(c4, "clone_state.txt").exists() && new File(c4, "macos_disk.img").exists() && G7Storage.deleteCloneWith(OK, true, NOT_LIVE, () -> false, vcl, "clone-004", () -> 0).ok);

        System.out.println(fails == 0 ? "G7EXEC STORAGE OK (0 failed)" : "G7EXEC STORAGE FAILED " + fails);
        System.exit(fails == 0 ? 0 : 1);
    }
}
