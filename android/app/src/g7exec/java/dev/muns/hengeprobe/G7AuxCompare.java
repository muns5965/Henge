package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * READ-ONLY comparison of the auxiliary storage (AUX) before and after the restore, to see what the restore changed: size, SHA-256, the changed 512-byte sector ranges (counted from the
 * start of the AUX data, i.e. after the 16 KiB the machine skips) and the printable text found in the changed ranges, with the boot-related words marked. Nothing is written, moved or deleted:
 * the files are only opened for reading (no links, regular files, inside the given folder). The text search is a plain ASCII scan -- it does NOT understand the NVRAM record format, so a word
 * found here may be an old or deleted value; the report says so. Used on the original install pair (macos_aux.initial vs macos_aux.img) and on a clone's AUX against the original's.
 */
public final class G7AuxCompare {
    public static final String IMG = "macos_aux.img", INITIAL = "macos_aux.initial";
    /** The machine's AUX data starts after this many bytes (host/vm.c aux_offset default). */
    public static final int DATA_OFFSET = 16384;
    static final int SECTOR = 512;
    static final String[] WORDS = {"auto-boot", "boot-command", "boot-args", "bootdelay", "boot-volume", "recovery", "iboot", "obliterate", "policy", "preboot", "nvram", "boot-"};
    static final int MAX_RANGES = 24, MAX_STRINGS = 40, MIN_STRING = 5;

    private G7AuxCompare() { }

    static final class Img { long size; String sha; byte[] data; }

    static Img load(File f, File dir) throws IOException {
        File cf = f.getCanonicalFile();
        if (!cf.getParentFile().equals(dir.getCanonicalFile())) throw new IOException(f.getName() + ": 지정한 폴더 밖의 파일입니다");
        if (Files.isSymbolicLink(f.toPath()) || !Files.isRegularFile(f.toPath(), LinkOption.NOFOLLOW_LINKS)) throw new IOException(f.getName() + ": 일반 파일이 아닙니다");
        long len = f.length();
        if (len < DATA_OFFSET || len > (64L << 20)) throw new IOException(f.getName() + ": 크기가 예상 범위가 아닙니다(" + len + ")");
        Img i = new Img();
        i.size = len;
        i.data = new byte[(int) len];
        try (InputStream in = Files.newInputStream(f.toPath())) {
            int off = 0;
            while (off < i.data.length) { int n = in.read(i.data, off, i.data.length - off); if (n < 0) break; off += n; }
            if (off != i.data.length) throw new IOException(f.getName() + ": 끝까지 읽지 못했습니다");
        }
        try { i.sha = hex(MessageDigest.getInstance("SHA-256").digest(i.data)); } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
        return i;
    }

    static String hex(byte[] b) { StringBuilder s = new StringBuilder(); for (byte x : b) s.append(String.format(Locale.ROOT, "%02x", x & 0xff)); return s.toString(); }

    /** Changed sector ranges as {firstSector, lastSector} (AUX-data sector numbers; the 16 KiB before the data are reported as "header"). */
    static List<long[]> ranges(byte[] a, byte[] b) {
        List<long[]> out = new ArrayList<>();
        int n = Math.min(a.length, b.length);
        long open = -1, last = -1;
        for (int s = 0; s * SECTOR < n; s++) {
            int from = s * SECTOR, to = Math.min(n, from + SECTOR);
            boolean diff = false;
            for (int k = from; k < to; k++) if (a[k] != b[k]) { diff = true; break; }
            long sec = (long) s - DATA_OFFSET / SECTOR;
            if (diff) { if (open < 0) open = sec; last = sec; }
            else if (open >= 0) { out.add(new long[] {open, last}); open = -1; }
        }
        if (open >= 0) out.add(new long[] {open, last});
        return out;
    }

    /** Printable ASCII runs (>= MIN_STRING) inside [fromByte, toByte) of d, as "offset: text" (offset in AUX-data bytes). */
    static List<String> strings(byte[] d, int fromByte, int toByte) {
        List<String> out = new ArrayList<>();
        int start = -1;
        for (int i = Math.max(0, fromByte); i <= Math.min(d.length, toByte); i++) {
            boolean p = i < Math.min(d.length, toByte) && d[i] >= 0x20 && d[i] < 0x7f;
            if (p && start < 0) start = i;
            if (!p && start >= 0) {
                if (i - start >= MIN_STRING) out.add(Integer.toString(start - DATA_OFFSET) + ": " + new String(d, start, Math.min(i - start, 120), java.nio.charset.StandardCharsets.US_ASCII));
                start = -1;
            }
        }
        return out;
    }

    static boolean boot(String s) { String l = s.toLowerCase(Locale.ROOT); for (String w : WORDS) if (l.contains(w)) return true; return false; }

    static int nonZeroSectors(byte[] d) {
        int c = 0;
        for (int s = DATA_OFFSET / SECTOR; s * SECTOR < d.length; s++) {
            for (int k = s * SECTOR; k < Math.min(d.length, (s + 1) * SECTOR); k++) if (d[k] != 0) { c++; break; }
        }
        return c;
    }

    /** The report for the original pair: dir holds macos_aux.initial and macos_aux.img. */
    public static String compareInitialToImage(File dirIn) {
        StringBuilder r = new StringBuilder("[AUX 비교: 복원 전(macos_aux.initial) ↔ 복원 후(macos_aux.img)] 읽기 전용\n");
        try {
            File dir = dirIn.getCanonicalFile();
            Img a = load(new File(dir, INITIAL), dir), b = load(new File(dir, IMG), dir);
            r.append("복원 전: ").append(a.size).append(" 바이트, SHA-256 ").append(a.sha.substring(0, 16)).append("…, 내용 있는 섹터 ").append(nonZeroSectors(a.data)).append('\n');
            r.append("복원 후: ").append(b.size).append(" 바이트, SHA-256 ").append(b.sha.substring(0, 16)).append("…, 내용 있는 섹터 ").append(nonZeroSectors(b.data)).append('\n');
            if (a.size != b.size) r.append("크기가 다릅니다.\n");
            boolean hdr = false;
            for (int k = 0; k < DATA_OFFSET && k < Math.min(a.data.length, b.data.length); k++) if (a.data[k] != b.data[k]) { hdr = true; break; }
            r.append("머리 16 KiB: ").append(hdr ? "다름" : "같음").append('\n');
            List<long[]> rg = ranges(a.data, b.data);
            long total = 0; for (long[] x : rg) total += x[1] - x[0] + 1;
            r.append("바뀐 섹터(512바이트): ").append(total).append("개, 구간 ").append(rg.size()).append("개\n");
            for (int i = 0; i < rg.size() && i < MAX_RANGES; i++) r.append("  구간 ").append(rg.get(i)[0]).append("~").append(rg.get(i)[1]).append(" (").append(rg.get(i)[1] - rg.get(i)[0] + 1).append("섹터)\n");
            if (rg.size() > MAX_RANGES) r.append("  … 나머지 ").append(rg.size() - MAX_RANGES).append("개 구간 생략\n");
            appendStrings(r, "복원 후 AUX의 바뀐 구간에서 찾은 글자열", b.data, rg);
            if (rg.isEmpty()) r.append("바뀐 곳이 없습니다: 복원 중 AUX에 쓰기가 반영되지 않았다는 뜻입니다.\n");
            r.append("주의: 글자열 검색일 뿐 NVRAM 레코드 구조를 해석하지 않습니다. 이전 값·삭제된 값이 남아 있을 수 있어 '찾았다'가 '유효한 설정'은 아닙니다.\n");
        } catch (IOException e) {
            r.append("비교하지 못했습니다: ").append(e.getMessage()).append('\n');
        }
        return r.toString();
    }

    private static void appendStrings(StringBuilder r, String title, byte[] d, List<long[]> rg) {
        List<String> all = new ArrayList<>();
        for (long[] x : rg) all.addAll(strings(d, (int) (x[0] * SECTOR) + DATA_OFFSET, (int) ((x[1] + 1) * SECTOR) + DATA_OFFSET));
        List<String> hit = new ArrayList<>();
        for (String s : all) if (boot(s)) hit.add(s);
        r.append(title).append(": 전체 ").append(all.size()).append("개, 부팅 관련 단어 포함 ").append(hit.size()).append("개\n");
        int shown = 0;
        for (String s : hit) { if (shown++ >= MAX_STRINGS) break; r.append("  * ").append(s).append('\n'); }
        if (hit.isEmpty()) for (int i = 0; i < all.size() && i < 12; i++) r.append("  - ").append(all.get(i)).append('\n');
    }

    /** Is a clone's AUX still byte-identical to the INSTALL PAIR's macos_aux.img? (Only a clone made from the install pair and never booted must be; a clone made from another clone can differ and still be a correct copy: see compareCloneToOrigin.) */
    public static String compareCloneToOriginal(File privIn, File cloneDirIn) {
        try {
            File priv = privIn.getCanonicalFile(), cd = cloneDirIn.getCanonicalFile();
            Img o = load(new File(priv, IMG), priv), c = load(new File(cd, IMG), cd);
            return cd.getName() + ": " + (o.sha.equals(c.sha) ? "원본 복원 후 AUX와 같음" : "원본과 다름(" + ranges(o.data, c.data).size() + "개 구간)");
        } catch (IOException e) {
            return cloneDirIn.getName() + ": 비교하지 못했습니다(" + e.getMessage() + ")";
        }
    }

    /** The clone's AUX against the AUX of what it was really copied from (clone_origins.txt): the install pair or another clone. No record (made before that file existed): says so and falls back to the install pair. */
    public static String compareCloneToOrigin(File privIn, File cloneDirIn) {
        try {
            File priv = privIn.getCanonicalFile(), cd = cloneDirIn.getCanonicalFile();
            String from = G7Clone.originOf(priv, cd.getName());
            if (from == null) return "출처 기록 없음(기록 기능 이전에 만든 복제본으로 보입니다) — 설치 원본 기준: " + compareCloneToOriginal(priv, cd);
            if (from.equals("install-pair")) return "출처: 설치 쌍 — " + compareCloneToOriginal(priv, cd);
            File src = new File(new File(priv, "clones"), from);
            if (!from.matches("clone-[0-9]{3}") || !src.isDirectory()) return cd.getName() + ": 출처 " + from + " 폴더가 없어 비교하지 못했습니다";
            Img o = load(new File(src, IMG), src), c = load(new File(cd, IMG), cd);
            return cd.getName() + ": 출처 " + from + " — " + (o.sha.equals(c.sha) ? "AUX 같음(복제 정확)" : "AUX 다름(" + ranges(o.data, c.data).size() + "개 구간; " + from + "가 그 뒤 부팅으로 바뀌었거나 복제 문제)");
        } catch (IOException e) {
            return cloneDirIn.getName() + ": 비교하지 못했습니다(" + e.getMessage() + ")";
        }
    }
}
