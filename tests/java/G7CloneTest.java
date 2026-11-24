package dev.muns.hengeprobe;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;

/** G7Clone on a plain JVM, mock files only. Stand-in "extents": the non-zero 4 KiB blocks of a file are its data (no lseek on a JVM). */
public class G7CloneTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    static final G7Clone.Extents SCAN = (f, size) -> {
        List<long[]> out = new ArrayList<>();
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            byte[] b = new byte[4096]; long s = -1;
            for (long pos = 0; pos < size; pos += 4096) {
                r.seek(pos); int n = r.read(b); boolean data = false; for (int i = 0; i < n; i++) if (b[i] != 0) { data = true; break; }
                if (data && s < 0) s = pos; if (!data && s >= 0) { out.add(new long[]{s, pos}); s = -1; }
            }
            if (s >= 0) out.add(new long[]{s, size});
        }
        return out;
    };
    static final G7Storage.Sizer SIZER = f -> { long t = 0; for (long[] e : SCAN.data(f, f.length())) t += e[1] - e[0]; return t; };

    public static final long ECID = 0x12345678901L;
    static void put(File f, long logical, long... offs) throws IOException {
        try (RandomAccessFile r = new RandomAccessFile(f, "rw")) { r.setLength(logical); for (long o : offs) { r.seek(o); byte[] b = new byte[8192]; Arrays.fill(b, (byte) (1 + o % 200)); r.write(b); } }
    }
    public static File pair(String state) throws IOException {
        File priv = Files.createTempDirectory("clone").toFile();
        put(new File(priv, "macos_disk.img"), 1L << 30, 0, 4096 * 5, 700L << 20, (1L << 30) - 8192);
        put(new File(priv, "macos_aux.img"), 4L << 20, 16384, 1 << 20);
        put(new File(priv, "macos_aux.initial"), 1 << 20, 0);
        Files.write(new File(priv, "g7_state.txt").toPath(), ("state=" + state + "\necid_tag=" + G7Clone.ecidTag(ECID) + "\n").getBytes());
        return priv;
    }
    static byte[] sha(File f) throws Exception { java.security.MessageDigest d = java.security.MessageDigest.getInstance("SHA-256"); try (InputStream in = new FileInputStream(f)) { byte[] b = new byte[1 << 16]; int n; while ((n = in.read(b)) > 0) d.update(b, 0, n); } return d.digest(); }
    public static String snapshot(File priv) throws Exception { StringBuilder sb = new StringBuilder(); for (String n : new String[]{"macos_disk.img", "macos_aux.img", "macos_aux.initial", "g7_state.txt"}) { File f = new File(priv, n); sb.append(n).append(f.length()).append(f.lastModified()).append(Arrays.toString(sha(f))); } return sb.toString(); }

    public static G7Clone.Result run(boolean enabled, boolean live, File priv, int runner, G7Clone.Extents ex, long free, java.util.function.BooleanSupplier cancel) {
        return G7Clone.cloneWith(enabled, () -> live, priv, () -> runner, ex, SIZER, () -> free, cancel);
    }
    static int clonesCount(File priv) { File[] l = new File(priv, "clones").listFiles(); return l == null ? 0 : l.length; }

    public static void main(String[] a) throws Exception {
        long ENOUGH = 1L << 40;
        File p = pair("in_progress"); String before = snapshot(p);
        G7Clone.Result r = run(true, false, p, 0, SCAN, ENOUGH, () -> false);
        check("a clone of an in_progress pair is made and verified: " + r.message, r.ok && r.dir != null);
        File c = r.dir;
        check("the clone has the members and the marker, no .partial", new File(c, "macos_disk.img").isFile() && new File(c, "macos_aux.img").isFile() && new File(c, "macos_aux.initial").isFile() && new File(c, "clone_state.txt").isFile() && c.list((d, n) -> n.endsWith(".partial")).length == 0);
        check("same logical size and same bytes", new File(c, "macos_disk.img").length() == (1L << 30) && Arrays.equals(sha(new File(c, "macos_disk.img")), sha(new File(p, "macos_disk.img"))) && Arrays.equals(sha(new File(c, "macos_aux.img")), sha(new File(p, "macos_aux.img"))));
        check("the original is byte for byte, size and time unchanged (state still in_progress)", snapshot(p).equals(before) && G7Storage.stateOf(new File(p, "g7_state.txt")).equals("in_progress"));
        String mk = new String(Files.readAllBytes(new File(c, "clone_state.txt").toPath()));
        check("the marker says clone_complete with the source state, the ECID TAG (not the ECID) and no path", mk.contains("state=clone_complete") && mk.contains("source_state=in_progress") && mk.contains("ecid_tag=" + G7Clone.ecidTag(ECID)) && !mk.contains("12345678901") && !mk.contains("/"));
        check("the state file with the tag is copied byte for byte", Arrays.equals(Files.readAllBytes(new File(c, "g7_state.txt").toPath()), Files.readAllBytes(new File(p, "g7_state.txt").toPath())));
        check("the tag matches the C implementation's algorithm (vector)", G7Clone.ecidTag(0x12345678901L).equals("b15a69182ce0d9c4"));
        check("validate: the finished clone passes for its ECID", G7Clone.validate(c, p, ECID) == null);
        check("validate: another ECID is refused", G7Clone.validate(c, p, ECID + 1) != null && G7Clone.validate(c, p, 0) != null);
        check("the copy is sparse: real data far below the 1 GiB logical size", SIZER.allocated(new File(c, "macos_disk.img")) < (1 << 20));

        // validate refuses everything that is not a finished clone of this app and this ECID
        File vv = r.dir; byte[] mkb = Files.readAllBytes(new File(vv, "clone_state.txt").toPath());
        Files.write(new File(vv, "clone_state.txt").toPath(), Arrays.copyOf(mkb, mkb.length / 2));
        check("validate: a half-written marker is refused", G7Clone.validate(vv, p, ECID) != null);
        Files.write(new File(vv, "clone_state.txt").toPath(), new String(mkb).replace("logical_disk_bytes=", "logical_disk_bytes=1").getBytes());
        check("validate: a marker whose size differs from the disk is refused", G7Clone.validate(vv, p, ECID) != null);
        Files.write(new File(vv, "clone_state.txt").toPath(), mkb);
        check("validate: the restored marker passes again", G7Clone.validate(vv, p, ECID) == null);
        Files.write(new File(vv, "extra.txt").toPath(), new byte[1]); check("validate: an unknown file is refused", G7Clone.validate(vv, p, ECID) != null); Files.delete(new File(vv, "extra.txt").toPath());
        Files.delete(new File(vv, "clone_state.txt").toPath()); check("validate: no marker is refused", G7Clone.validate(vv, p, ECID) != null); Files.write(new File(vv, "clone_state.txt").toPath(), mkb);
        File other = new File(Files.createTempDirectory("other").toFile(), "clones/clone-001"); other.mkdirs();
        check("validate: a folder outside this app's clones folder is refused", G7Clone.validate(other, p, ECID) != null && G7Clone.validate(new File("clones/clone-001"), p, ECID) != null && G7Clone.validate(p, p, ECID) != null);
        File lnk = new File(vv.getParentFile(), "clone-050"); Files.createSymbolicLink(lnk.toPath(), vv.toPath());
        check("validate: a folder that is a symbolic link is refused", G7Clone.validate(lnk, p, ECID) != null); Files.delete(lnk.toPath());
        File tgt = new File(p, "macos_aux.img"); File aux = new File(vv, "macos_aux.img"); byte[] auxb = Files.readAllBytes(aux.toPath()); Files.delete(aux.toPath()); Files.createSymbolicLink(aux.toPath(), tgt.toPath());
        check("validate: a member that is a link to the ORIGINAL is refused", G7Clone.validate(vv, p, ECID) != null); Files.delete(aux.toPath()); Files.write(aux.toPath(), auxb);
        check("validate: passes again after putting the member back", G7Clone.validate(vv, p, ECID) == null);

        // refusals leave nothing behind
        File p2 = pair("in_progress");
        check("switch off: refused, no clones folder", !run(false, false, p2, 0, SCAN, ENOUGH, () -> false).ok && clonesCount(p2) == 0);
        check("a live run: refused", !run(true, true, p2, 0, SCAN, ENOUGH, () -> false).ok && clonesCount(p2) == 0);
        check("a runner process found: refused", !run(true, false, p2, 1, SCAN, ENOUGH, () -> false).ok && clonesCount(p2) == 0);
        check("runner status unknown: refused", !run(true, false, p2, 2, SCAN, ENOUGH, () -> false).ok && clonesCount(p2) == 0);
        G7Clone.Result nf = run(true, false, p2, 0, SCAN, 1L << 20, () -> false);
        check("not enough free space: refused with the numbers, nothing created", !nf.ok && nf.message.contains("여유 공간이 부족") && clonesCount(p2) == 0);
        G7Clone.Extents none = (f, s) -> { throw new IOException("sparse-unsupported"); };
        G7Clone.Result un = run(true, false, p2, 0, none, ENOUGH, () -> false);
        check("no hole information: refused, never a full copy", !un.ok && un.message.contains("전체 복사로 대체하지 않습니다") && clonesCount(p2) == 0);
        G7Clone.Extents whole = (f, s) -> Collections.singletonList(new long[]{0, s});
        G7Clone.Result wh = run(true, false, p2, 0, whole, ENOUGH, () -> false);
        check("a file system that reports the whole file as data: refused as not sparse, nothing created", !wh.ok && wh.message.contains("전체 복사로 대체하지 않습니다") && clonesCount(p2) == 0);
        File p3 = pair("weird"); check("an unreadable state: refused", !run(true, false, p3, 0, SCAN, ENOUGH, () -> false).ok);
        File p4 = pair("done"); G7Clone.Result d = run(true, false, p4, 0, SCAN, ENOUGH, () -> false);
        check("a done pair can be cloned too, its state is recorded and untouched", d.ok && new String(Files.readAllBytes(new File(d.dir, "clone_state.txt").toPath())).contains("source_state=done") && G7Storage.stateOf(new File(p4, "g7_state.txt")).equals("done"));
        File p5 = pair("in_progress"); Files.delete(new File(p5, "macos_aux.img").toPath());
        check("a missing AUX: refused", !run(true, false, p5, 0, SCAN, ENOUGH, () -> false).ok && clonesCount(p5) == 0);

        // cancel in the middle: the partial files of this call and the empty folder are gone, the original is untouched
        // the allocation of the copy is bounded
        File pa = pair("in_progress"); G7Storage.Sizer inflated = f -> f.getName().endsWith(".partial") ? 100L << 30 : SIZER.allocated(f);
        G7Clone.Result al = G7Clone.cloneWith(true, () -> false, pa, () -> 0, SCAN, inflated, () -> ENOUGH, () -> false);
        check("a copy that uses far more than the original: refused, cleaned up", !al.ok && al.message.contains("허용 범위") && clonesCount(pa) == 0);
        // the free space is checked again while copying
        File pf = pair("in_progress"); put(new File(pf, "macos_disk.img"), 1L << 30, 0, 4096 * 5, 700L << 20, (1L << 30) - 8192); try (RandomAccessFile rr = new RandomAccessFile(new File(pf, "macos_disk.img"), "rw")) { rr.seek(100L << 20); byte[] blk = new byte[1 << 20]; Arrays.fill(blk, (byte) 5); for (int i = 0; i < 140; i++) rr.write(blk); }
        AtomicInteger fcalls = new AtomicInteger();
        G7Clone.Result fs = G7Clone.cloneWith(true, () -> false, pf, () -> 0, SCAN, SIZER, () -> fcalls.incrementAndGet() <= 1 ? ENOUGH : 1L << 20, () -> false);
        check("free space that falls during the copy stops it: reported, cleaned up", !fs.ok && fs.message.contains("여유 공간이 기준") && clonesCount(pf) == 0 && Arrays.equals(sha(new File(pf, "g7_state.txt")), sha(new File(pf, "g7_state.txt"))));
        File p6 = pair("in_progress"); String b6 = snapshot(p6); AtomicInteger polls = new AtomicInteger();
        G7Clone.Result cx = run(true, false, p6, 0, SCAN, ENOUGH, () -> polls.incrementAndGet() > 3);
        check("cancel: reported, the folder and partials removed, original untouched", !cx.ok && cx.message.contains("취소") && clonesCount(p6) == 0 && snapshot(p6).equals(b6));
        // a failure after the copy (the original changes while it is being verified)
        File p7 = pair("in_progress"); AtomicInteger calls = new AtomicInteger();
        G7Clone.Extents flaky = (f, s) -> { List<long[]> e = SCAN.data(f, s); if (f.getName().endsWith(".partial") && calls.incrementAndGet() == 1) { try (RandomAccessFile rw = new RandomAccessFile(new File(f.getParentFile().getParentFile().getParentFile(), "macos_aux.initial"), "rw")) { rw.seek(0); rw.write(7); } } return e; };
        G7Clone.Result fl = run(true, false, p7, 0, flaky, ENOUGH, () -> false);
        check("the original changed during the copy: refused, no clone, nothing left", !fl.ok && clonesCount(p7) == 0);
        // a second clone gets the next number
        G7Clone.Result rr2 = run(true, false, p, 0, SCAN, ENOUGH, () -> false);
        check("a second clone is clone-002 and the first is untouched", rr2.ok && rr2.dir.getName().equals("clone-002") && new File(c, "clone_state.txt").isFile());
        System.out.println(fails == 0 ? "G7CLONE OK" : "G7CLONE FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
