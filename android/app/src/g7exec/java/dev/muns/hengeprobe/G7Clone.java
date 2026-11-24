package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;
import java.util.function.IntSupplier;
import java.util.function.LongSupplier;

/**
 * [G7-EXEC] Only in the g7exec source set. A verified SPARSE copy of the current install pair into its own folder (vm/clones/clone-NNN/), so that a later boot test can use the copy and never the original.
 *
 * What it does to the original: it only READS macos_disk.img, macos_aux.img and macos_aux.initial (and g7_state.txt, to record its state). It writes, moves or deletes nothing there, never changes g7_state.txt, and
 * refuses to start while a run of this app is live or a runner process is (or cannot be told to be) present, all inside G7ExecLauncher.START_LOCK. If the original changed in size or time during the copy, the copy is refused.
 *
 * Sparse: only the data extents of the source (given by an Extents implementation: on Android lseek SEEK_DATA/SEEK_HOLE; on a file system without them it says so and NOTHING is copied -- there is no fallback to a full
 * copy, which would turn a 128 GiB sparse disk into 128 GiB of real data). The copy's length is set first (holes), data is written chunk by chunk, and a chunk that is all zeros is left as a hole.
 *
 * Complete or nothing: files are written as NAME.partial (CREATE_NEW), checked (same logical size; every data extent of the copy lies inside a data extent of the source; the bytes of every source data extent read back
 * equal), only then renamed, and the marker clone_state.txt ("state=clone_complete") is created LAST. A copy without the marker is not a copy: it has no macos_disk.img. On an error or a cancel the .partial files of
 * THIS call (and the folder, when empty) are removed -- nothing else is ever removed. The marker holds sizes and the source's state, no ECID (the ECID file is not part of the pair and is not copied).
 * No Android classes: tests/java/G7CloneTest.java runs on a plain JVM.
 */
public final class G7Clone {
    private G7Clone() {}

    /** Space that must stay free after the copy, besides the copy itself. */
    public static final long RESERVE_BYTES = 8L << 30;
    static final String[] MEMBERS = {"macos_disk.img", "macos_aux.img", "macos_aux.initial"};   // the last one is optional
    static final int CHUNK = 1 << 20;
    static final String MARKER = "clone_state.txt";

    /** The data extents [start, end) of a file, in order, whole file. Throws IOException("sparse-unsupported") when the file system cannot tell holes from data. */
    public interface Extents { List<long[]> data(File f, long size) throws IOException; }

    public static final class Result {
        public final boolean ok; public final String message; public final File dir;
        Result(boolean ok, String message, File dir) { this.ok = ok; this.message = message; this.dir = dir; }
    }

    private static boolean regular(Path p) { return Files.isRegularFile(p, LinkOption.NOFOLLOW_LINKS); }
    private static boolean exists(Path p) { return Files.exists(p, LinkOption.NOFOLLOW_LINKS); }

    public static Result clone(File priv, IntSupplier runnerStatus, Extents ex, G7Storage.Sizer sizer, LongSupplier freeBytes, BooleanSupplier cancel) {
        return cloneWith(G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, priv, runnerStatus, ex, sizer, freeBytes, cancel);
    }

    static Result cloneWith(boolean enabled, BooleanSupplier live, File priv, IntSupplier runnerStatus, Extents ex, G7Storage.Sizer sizer, LongSupplier freeBytes, BooleanSupplier cancel) {
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = G7Isolation.gate(enabled, live, runnerStatus);
            if (g != null) return new Result(false, g.replace("격리하지", "복제하지").replace("아무것도 옮기지", "아무것도 만들지"), null);
            if (G7Isolation.pending(priv)) return new Result(false, "끝나지 않은 격리가 있어 복제하지 않았습니다. 먼저 복구를 실행하세요.", null);
            File disk = new File(priv, MEMBERS[0]), aux = new File(priv, MEMBERS[1]), init = new File(priv, MEMBERS[2]);
            if (!regular(disk.toPath()) || !regular(aux.toPath())) return new Result(false, "복제할 설치 쌍(디스크·AUX)이 일반 파일로 없습니다. 아무것도 만들지 않았습니다.", null);
            if (exists(init.toPath()) && !regular(init.toPath())) return new Result(false, "AUX 원본 복사가 일반 파일이 아니어서 복제하지 않았습니다.", null);
            String state = G7Storage.stateOf(new File(priv, "g7_state.txt"));
            if (!state.equals("in_progress") && !state.equals("done")) return new Result(false, "설치 쌍의 상태를 읽을 수 없어 복제하지 않았습니다.", null);

            List<File> src = new ArrayList<>();
            src.add(disk); src.add(aux); if (exists(init.toPath())) src.add(init);
            long need = 0; long[] size = new long[src.size()], mtime = new long[src.size()];
            List<List<long[]>> ext = new ArrayList<>();
            try {
                for (int i = 0; i < src.size(); i++) {
                    size[i] = src.get(i).length(); mtime[i] = src.get(i).lastModified();
                    need += sizer.allocated(src.get(i));
                    List<long[]> got = ex.data(src.get(i), size[i]);
                    long sum = 0; for (long[] e : got) sum += e[1] - e[0];
                    long al = sizer.allocated(src.get(i));
                    // a file system without hole support reports the whole file as one data extent: far more than is really allocated. That is not a sparse answer, so nothing is copied.
                    if (sum > al + al / 4 + (64L << 20)) throw new IOException("sparse-unsupported");
                    ext.add(got);
                }
            } catch (IOException e) {
                return new Result(false, "sparse-unsupported".equals(e.getMessage()) ? "이 파일시스템은 구멍(SEEK_HOLE) 정보를 주지 않아 복제하지 않았습니다(전체 복사로 대체하지 않습니다). 아무것도 만들지 않았습니다." : "원본을 읽지 못했습니다: " + e.getMessage() + ". 아무것도 만들지 않았습니다.", null);
            }
            long free = freeBytes.getAsLong();
            if (free < need + RESERVE_BYTES) return new Result(false, "여유 공간이 부족해 복제하지 않았습니다(필요 " + G7Storage.fmt(need) + " + 여유분 " + G7Storage.fmt(RESERVE_BYTES) + ", 현재 " + G7Storage.fmt(free) + "). 아무것도 만들지 않았습니다.", null);

            File clones = new File(priv, "clones");
            if (!clones.isDirectory() && !clones.mkdirs()) return new Result(false, "clones 폴더를 만들 수 없습니다.", null);
            File dir = null;
            for (int n = 1; n < 1000 && dir == null; n++) { File c = new File(clones, String.format("clone-%03d", n)); if (c.mkdir()) dir = c; }
            if (dir == null) return new Result(false, "복제 폴더 이름을 정할 수 없습니다(clone-001..999가 이미 있음).", null);
            List<File> partials = new ArrayList<>();
            try {
                for (int i = 0; i < src.size(); i++) {
                    File part = new File(dir, src.get(i).getName() + ".partial");
                    partials.add(part);
                    copyOne(src.get(i), part, size[i], ext.get(i), cancel);
                }
                for (int i = 0; i < src.size(); i++) {
                    List<long[]> got = ex.data(partials.get(i), size[i]);
                    String bad = verify(src.get(i), partials.get(i), size[i], ext.get(i), got, cancel);
                    if (bad != null) throw new IOException("검증 실패(" + src.get(i).getName() + "): " + bad);
                }
                for (int i = 0; i < src.size(); i++)
                    if (src.get(i).length() != size[i] || src.get(i).lastModified() != mtime[i]) throw new IOException("복제 중 원본이 바뀌었습니다(" + src.get(i).getName() + ")");
                long alloc = 0;
                for (int i = 0; i < src.size(); i++) {
                    File fin = new File(dir, src.get(i).getName());
                    if (exists(fin.toPath())) throw new IOException("대상 이름이 이미 있습니다");
                    Files.move(partials.get(i).toPath(), fin.toPath());
                    alloc += sizer.allocated(fin);
                }
                String mk = "state=clone_complete\nsource_state=" + state + "\nlogical_disk_bytes=" + size[0] + "\nallocated_bytes=" + alloc + "\nmembers=" + src.size() + "\n";
                try (FileChannel c = FileChannel.open(new File(dir, MARKER).toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
                    ByteBuffer b = ByteBuffer.wrap(mk.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    while (b.hasRemaining()) c.write(b);
                    c.force(true);
                }
                return new Result(true, "복제본을 만들고 검증했습니다: " + dir.getName() + "(실제 " + G7Storage.fmt(alloc) + "). 원본은 읽기만 했습니다.", dir);
            } catch (IOException | RuntimeException e) {
                String cleaned = cleanup(dir, partials);
                return new Result(false, (e instanceof Cancelled ? "취소했습니다" : "복제하지 못했습니다: " + e.getMessage()) + ". " + cleaned + " 원본은 바뀌지 않았습니다.", null);
            }
        }
    }

    static final class Cancelled extends IOException { Cancelled() { super("cancelled"); } }

    /** Removes only the .partial files of this call and the folder when it is then empty. */
    private static String cleanup(File dir, List<File> partials) {
        for (File p : partials) { try { Files.deleteIfExists(p.toPath()); } catch (IOException ignored) { } }
        File[] left = dir.listFiles();
        if (left != null && left.length == 0 && dir.delete()) return "만들던 파일을 정리했습니다.";
        return "정리하지 못한 파일이 있을 수 있습니다(" + dir.getName() + "에 marker가 없으면 복제본이 아닙니다).";
    }

    private static void copyOne(File src, File dst, long size, List<long[]> extents, BooleanSupplier cancel) throws IOException {
        try (RandomAccessFile len = new RandomAccessFile(dst, "rw")) { if (len.length() != 0) throw new IOException("target not empty"); len.setLength(size); }
        try (FileChannel in = FileChannel.open(src.toPath(), StandardOpenOption.READ); FileChannel out = FileChannel.open(dst.toPath(), StandardOpenOption.WRITE)) {
            ByteBuffer buf = ByteBuffer.allocate(CHUNK);
            for (long[] e : extents) {
                for (long pos = e[0]; pos < e[1]; ) {
                    if (cancel.getAsBoolean()) throw new Cancelled();
                    int want = (int) Math.min(CHUNK, e[1] - pos);
                    buf.clear(); buf.limit(want);
                    int n = in.read(buf, pos);
                    if (n <= 0) throw new IOException("read ended early at " + pos);
                    if (!allZero(buf.array(), n)) { buf.flip(); long w = pos; while (buf.hasRemaining()) w += out.write(buf, w); }
                    pos += n;
                }
            }
            out.force(true);
        }
    }

    private static boolean allZero(byte[] b, int n) { for (int i = 0; i < n; i++) if (b[i] != 0) return false; return true; }

    /** null when the copy matches: same logical size; every data extent of the copy inside a data extent of the source; the bytes of every source data extent equal (a hole in the copy reads as zeros). */
    private static String verify(File src, File dst, long size, List<long[]> srcExt, List<long[]> dstExt, BooleanSupplier cancel) throws IOException {
        if (dst.length() != size) return "logical size differs";
        for (long[] d : dstExt) {
            boolean inside = false;
            for (long[] s : srcExt) if (d[0] >= s[0] && d[1] <= s[1]) { inside = true; break; }
            if (!inside) return "the copy has data where the source has a hole (at " + d[0] + ")";
        }
        try (FileChannel a = FileChannel.open(src.toPath(), StandardOpenOption.READ); FileChannel b = FileChannel.open(dst.toPath(), StandardOpenOption.READ)) {
            ByteBuffer x = ByteBuffer.allocate(CHUNK), y = ByteBuffer.allocate(CHUNK);
            for (long[] e : srcExt) {
                for (long pos = e[0]; pos < e[1]; ) {
                    if (cancel.getAsBoolean()) throw new Cancelled();
                    int want = (int) Math.min(CHUNK, e[1] - pos);
                    x.clear(); x.limit(want); y.clear(); y.limit(want);
                    int n = a.read(x, pos), m = b.read(y, pos);
                    if (n != want || m != want) return "short read at " + pos;
                    if (!java.util.Arrays.equals(x.array(), 0, want, y.array(), 0, want)) return "bytes differ at " + pos;
                    pos += want;
                }
            }
        }
        return null;
    }
}
