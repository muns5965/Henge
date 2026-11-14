package dev.muns.hengeprobe;

import dev.muns.hengeprobe.RomBranchFinder.Result;
import dev.muns.hengeprobe.RomBranchFinder.Status;
import java.io.File;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;

/**
 * Plain-java test of RomBranchFinder (no framework), on synthetic ROMs only:
 *   javac -encoding UTF-8 -d out android/app/src/main/java/dev/muns/hengeprobe/RomSlice.java android/app/src/main/java/dev/muns/hengeprobe/RomBranchFinder.java tests/java/RomBranchFinderTest.java && java -cp out dev.muns.hengeprobe.RomBranchFinderTest
 * It checks the search and its refusals. That the screen, buttons and clipboard behave is not covered here.
 */
public class RomBranchFinderTest {
    static int fails;
    static final long BASE = RomSlice.BASE;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    static int bl(long from, long to) { return 0x94000000 | (int) (((to - from) >> 2) & 0x3ffffff); }
    static int b(long from, long to) { return 0x14000000 | (int) (((to - from) >> 2) & 0x3ffffff); }
    static int low26(long from, long to) { return (int) (((to - from) >> 2) & 0x3ffffff); }

    static byte[] rom(int words) {   // filler is NOP (0xd503201f), which is none of the things searched for
        byte[] r = new byte[words * 4];
        for (int i = 0; i < words; i++) put(r, i, 0xd503201f);
        return r;
    }
    static void put(byte[] r, int word, int v) {
        r[word * 4] = (byte) v; r[word * 4 + 1] = (byte) (v >> 8); r[word * 4 + 2] = (byte) (v >> 16); r[word * 4 + 3] = (byte) (v >> 24);
    }
    static long pcOf(int word) { return BASE + 4L * word; }
    static String sha(byte[] d) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (byte x : MessageDigest.getInstance("SHA-256").digest(d)) sb.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return sb.toString();
    }
    static File file(byte[] d) throws Exception {
        File f = File.createTempFile("romfind-secret-name", ".bin");
        f.deleteOnExit();
        Files.write(f.toPath(), d);
        return f;
    }
    static Result run(byte[] d, String target) throws Exception {
        return new RomBranchFinder(d.length, sha(d)).find(file(d), target);
    }
    static boolean shown(List<Long> l, long... pcs) {
        if (l.size() != pcs.length) return false;
        for (int i = 0; i < pcs.length; i++) if (l.get(i) != pcs[i]) return false;
        return true;
    }

    public static void main(String[] a) throws Exception {
        // --- hits: forward and backward, BL and B, and the first and last word ---
        byte[] d = rom(1024);
        long T = pcOf(500);
        put(d, 0, bl(pcOf(0), T));                 // the first word of the file, forward
        put(d, 100, bl(pcOf(100), T));             // forward
        put(d, 900, bl(pcOf(900), T));             // backward (negative offset)
        put(d, 1023, bl(pcOf(1023), T));           // the last word, backward
        put(d, 50, b(pcOf(50), T));                // B forward
        put(d, 700, b(pcOf(700), T));              // B backward
        Result r = run(d, "0x" + Long.toHexString(T));
        check("search succeeds on a synthetic ROM", r.status == Status.OK);
        check("BL hits, forward and backward, first and last word, in address order", shown(r.blShown, pcOf(0), pcOf(100), pcOf(900), pcOf(1023)) && r.blTotal == 4);
        check("B hits are listed apart from BL", shown(r.bShown, pcOf(50), pcOf(700)) && r.bTotal == 2);
        check("size and SHA-256 of the file are reported", r.fileSize == d.length && sha(d).equals(r.sha256));

        // --- near misses and other instructions ---
        d = rom(1024);
        T = pcOf(500);
        put(d, 10, bl(pcOf(10), T + 4));           // one word after the target
        put(d, 11, bl(pcOf(11), T - 4));           // one word before the target
        put(d, 12, b(pcOf(12), T + 4));
        put(d, 13, b(pcOf(13), T - 4));
        put(d, 20, 0xd63f0000 | 3 << 5);           // BLR x3
        put(d, 21, 0xd65f03c0);                    // RET
        put(d, 22, 0x54000000 | (int) (((T - pcOf(22)) >> 2) & 0x7ffff) << 5);   // B.EQ to the target
        put(d, 23, 0xb4000000 | (int) (((T - pcOf(23)) >> 2) & 0x7ffff) << 5);   // CBZ to the target
        put(d, 24, 0x00000000);
        put(d, 25, 0xffffffff);
        // same low 26 bits as a real BL to T, but another instruction: LDR literal, a 0b100111 and a 0b110101 group, and plain data
        put(d, 30, 0x1c000000 | low26(pcOf(30), T));
        put(d, 31, 0x9c000000 | low26(pcOf(31), T));
        put(d, 32, 0xd4000000 | low26(pcOf(32), T));
        put(d, 33, 0x5c000000 | low26(pcOf(33), T));
        put(d, 34, 0x04000000 | low26(pcOf(34), T));
        put(d, 35, 0x90000000 | low26(pcOf(35), T));   // ADRP group
        r = run(d, "0x" + Long.toHexString(T));
        check("no hit when there is none; zero hits is a successful search", r.status == Status.OK && r.blTotal == 0 && r.bTotal == 0 && r.blShown.isEmpty() && r.bShown.isEmpty());
        put(d, 40, bl(pcOf(40), T));
        r = run(d, "0x" + Long.toHexString(T));
        check("a real BL among the near misses is the only hit", r.blTotal == 1 && shown(r.blShown, pcOf(40)) && r.bTotal == 0);

        // --- the target is exact: a word 4 bytes off is another search ---
        r = run(d, "0x" + Long.toHexString(T + 4));
        check("searching the next word finds the near miss, not the real hit", shown(r.blShown, pcOf(10)) && shown(r.bShown, pcOf(12)));
        r = run(d, "0x" + Long.toHexString(T - 4));
        check("searching the previous word finds its own near miss", shown(r.blShown, pcOf(11)) && shown(r.bShown, pcOf(13)));

        // --- total against shown ---
        d = rom(2048);
        T = pcOf(1000);
        for (int i = 0; i < 40; i++) put(d, 1100 + i, bl(pcOf(1100 + i), T));
        for (int i = 0; i < 3; i++) put(d, 1300 + i, b(pcOf(1300 + i), T));
        r = run(d, "0x" + Long.toHexString(T));
        check("40 BL hits: all counted, the first 32 kept, in address order", r.blTotal == 40 && r.blShown.size() == 32 && r.blShown.get(0) == pcOf(1100) && r.blShown.get(31) == pcOf(1131));
        check("the B list is separate and not cut", r.bTotal == 3 && r.bShown.size() == 3);
        String s = RomBranchFinder.screenText(r);
        check("the screen says 'total 40, 32 shown' for the BL list and shows the B list in full", s.contains("BL 직접 참조: 총 40건 중 32건 표시") && s.contains("B 직접 참조: 총 3건\n"));
        check("the 33rd BL address is not on the screen", !s.contains(String.format(Locale.ROOT, "0x%x", pcOf(1132))));

        // --- target validation ---
        d = rom(1024);
        RomBranchFinder fin = new RomBranchFinder(d.length, sha(d));
        File f = file(d);
        check("a bad format is refused", fin.find(f, "zz").status == Status.BAD_TARGET_FORMAT && fin.find(f, "").status == Status.BAD_TARGET_FORMAT && fin.find(f, null).status == Status.BAD_TARGET_FORMAT);
        check("below the ROM is refused", fin.find(f, "0xffffc").status == Status.TARGET_BELOW_ROM && fin.find(f, "0").status == Status.TARGET_BELOW_ROM);
        check("an unaligned target is refused", fin.find(f, "0x100001").status == Status.TARGET_UNALIGNED && fin.find(f, "0x100002").status == Status.TARGET_UNALIGNED);
        check("the first word is a valid target", fin.find(f, "0x100000").status == Status.OK);
        check("the last word is a valid target", fin.find(f, "0x" + Long.toHexString(pcOf(1023))).status == Status.OK);
        check("the word after the file is refused", fin.find(f, "0x" + Long.toHexString(pcOf(1024))).status == Status.TARGET_OUT_OF_ROM);
        check("a refused target leaves no hits and no copy text", RomBranchFinder.copyText(fin.find(f, "0x" + Long.toHexString(pcOf(1024)))) == null);

        // --- the file ---
        check("a missing file is refused", fin.find(new File("/nonexistent/romfind.bin"), "0x100000").status == Status.NO_FILE);
        check("a null file is refused", fin.find(null, "0x100000").status == Status.NO_FILE);
        File dir = Files.createTempDirectory("romfinddir").toFile();
        dir.deleteOnExit();
        check("a directory is refused", fin.find(dir, "0x100000").status == Status.NOT_A_FILE);
        byte[] longer = Arrays.copyOf(d, d.length + 4);
        r = fin.find(file(longer), "0x100000");
        check("a longer file is refused before it is read or hashed", r.status == Status.SIZE_MISMATCH && r.sha256 == null && r.fileSize == longer.length);
        byte[] shorter = Arrays.copyOf(d, d.length - 4);
        r = fin.find(file(shorter), "0x100000");
        check("a shorter file is refused before it is read or hashed", r.status == Status.SIZE_MISMATCH && r.sha256 == null);
        byte[] other = d.clone();
        put(other, 7, bl(pcOf(7), pcOf(8)));
        r = fin.find(file(other), "0x" + Long.toHexString(pcOf(8)));
        check("same size, other content: refused as a hash mismatch, nothing searched", r.status == Status.HASH_MISMATCH && r.blTotal == 0 && r.sha256 != null && !r.sha256.equals(sha(d)));
        check("a hash mismatch has no copy text", RomBranchFinder.copyText(r) == null);
        check("the hash expectation is case-insensitive", new RomBranchFinder(d.length, sha(d).toUpperCase(Locale.ROOT)).find(f, "0x100000").status == Status.OK);

        // --- a size that is not a multiple of 4: the tail is ignored, nothing throws ---
        byte[] odd = Arrays.copyOf(rom(64), 64 * 4 + 3);
        odd[64 * 4] = 0x00; odd[64 * 4 + 1] = 0x00; odd[64 * 4 + 2] = (byte) 0xff;
        put(odd, 3, bl(pcOf(3), pcOf(10)));
        r = new RomBranchFinder(odd.length, sha(odd)).find(file(odd), "0x" + Long.toHexString(pcOf(10)));
        check("a 3-byte tail is ignored and the aligned words are searched", r.status == Status.OK && shown(r.blShown, pcOf(3)));

        // --- the production instance uses the constants the app accepts a ROM by ---
        RomBranchFinder genuine = RomBranchFinder.forGenuineRom();
        r = genuine.find(file(new byte[(int) RomSlice.EXPECTED_LENGTH]), "0x100000");
        check("production: a file of the expected size with the wrong content is a hash mismatch", r.status == Status.HASH_MISMATCH && r.fileSize == RomSlice.EXPECTED_LENGTH);
        check("production: any other size is a size mismatch", genuine.find(f, "0x100000").status == Status.SIZE_MISMATCH);
        check("production: a target at the end of the genuine ROM size is out of the ROM", genuine.find(f, "0x" + Long.toHexString(BASE + RomSlice.EXPECTED_LENGTH)).status == Status.TARGET_OUT_OF_ROM);

        // --- what is shown and copied ---
        d = rom(256);
        T = pcOf(100);
        put(d, 5, bl(pcOf(5), T));
        File secret = file(d);
        r = new RomBranchFinder(d.length, sha(d)).find(secret, "0x" + Long.toHexString(T));
        s = RomBranchFinder.screenText(r);
        String c = RomBranchFinder.copyText(r);
        check("the screen has the target, the hit and the caveats", s.contains("검색 대상: 0x" + Long.toHexString(T)) && s.contains("0x" + Long.toHexString(pcOf(5))) && s.contains("후보") && s.contains("0건이어도"));
        check("the copy text has the target and the hit and says it is a candidate", c != null && c.contains("0x" + Long.toHexString(T)) && c.contains("0x" + Long.toHexString(pcOf(5))) && c.contains("후보"));
        Pattern bytesLike = Pattern.compile("[0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2}");
        check("no ROM bytes on the screen or in the copy text (no hex dump, no instruction words)", !bytesLike.matcher(s).find() && !bytesLike.matcher(c).find()
                && !s.contains("d503201f") && !c.contains("d503201f") && !s.contains(String.format(Locale.ROOT, "%08x", bl(pcOf(5), T))) && !c.contains(String.format(Locale.ROOT, "%08x", bl(pcOf(5), T))));
        check("neither contains the file's path or name", !s.contains(secret.getName()) && !c.contains(secret.getName()) && !s.contains(secret.getParent()) && !c.contains(secret.getParent()));
        r = new RomBranchFinder(d.length, sha(d)).find(secret, "0x" + Long.toHexString(pcOf(200)));
        s = RomBranchFinder.screenText(r);
        check("zero hits is shown as a result (0건) and is copyable, not as a failure", r.status == Status.OK && s.contains("BL 직접 참조: 0건") && s.contains("B 직접 참조: 0건") && !s.contains("거부/실패") && RomBranchFinder.copyText(r) != null);
        r = new RomBranchFinder(d.length, sha(d)).find(new File("/nonexistent/x"), "0x100000");
        s = RomBranchFinder.screenText(r);
        check("a failure is shown as a refusal with its reason, not as 0건", s.contains("거부/실패") && s.contains("내부 ROM 파일이 없습니다") && !s.contains("0건"));
        r = new RomBranchFinder(d.length, sha(d)).find(file(Arrays.copyOf(d, d.length + 4)), "0x100000");
        s = RomBranchFinder.screenText(r);
        check("a size refusal shows the size and no SHA-256 line", s.contains("파일 크기: " + (d.length + 4)) && !s.contains("SHA-256:"));
        r = fin.find(file(other), "0x100000");
        s = RomBranchFinder.screenText(r);
        check("a hash refusal says the hash differs from the expectation", s.contains("앱의 기대값과 다릅니다") && s.contains("거부/실패") && !s.contains("같습니다"));

        // --- every status has its own message ---
        boolean distinct = true;
        java.util.HashSet<String> seen = new java.util.HashSet<>();
        for (Status st : Status.values()) distinct &= seen.add(RomBranchFinder.message(st)) && !RomBranchFinder.message(st).equals(st.toString());
        check("every status has its own message", distinct);

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
