package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;

/**
 * Plain-java test of RomSlice (no framework, a synthetic ROM, no real ROM bytes):
 *   javac -encoding UTF-8 -d out android/app/src/main/java/dev/muns/hengeprobe/RomSlice.java tests/java/RomSliceTest.java && java -cp out dev.muns.hengeprobe.RomSliceTest
 * It checks the pure logic only. The screen, the clipboard and a real device are not covered.
 */
public class RomSliceTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    static final int SIZE = 2048;

    static byte[] rom() {
        byte[] b = new byte[SIZE];
        for (int i = 0; i < SIZE; i++) b[i] = (byte) (i * 31 + (i >> 8) * 7 + 5);
        return b;
    }

    static String sha(byte[] b) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (byte x : MessageDigest.getInstance("SHA-256").digest(b)) sb.append(String.format("%02x", x & 0xff));
        return sb.toString();
    }

    static File write(File dir, String name, byte[] b) throws IOException {
        File f = new File(dir, name);
        Files.write(f.toPath(), b);
        return f;
    }

    static RomSlice.Result rd(RomSlice s, File f, String pc, String len) { return s.read(f, pc, len); }

    public static void main(String[] a) throws Exception {
        File dir = Files.createTempDirectory("romslice").toFile();
        byte[] data = rom();
        String good = sha(data);
        File f = write(dir, "rom.bin", data);

        // --- a normal read, byte for byte ---
        RomSlice s = new RomSlice(SIZE, good);
        RomSlice.Result r = rd(s, f, "0x100010", "32");
        check("normal read: OK", r.status == RomSlice.Status.OK);
        check("normal read: exact bytes at file offset 0x10", r.data != null && Arrays.equals(r.data, Arrays.copyOfRange(data, 0x10, 0x30)));
        check("normal read: pc, length, offset reported", r.pc == 0x100010 && r.length == 32);
        check("normal read: size and hash reported", r.fileSize == SIZE && good.equals(r.sha256));
        check("normal read: total counted", r.totalAfter == 32 && s.total() == 32);
        check("pc without 0x and upper case digits", rd(new RomSlice(SIZE, good), f, "100010", "32").status == RomSlice.Status.OK
                && rd(new RomSlice(SIZE, good), f, "0X100010", "0x20").status == RomSlice.Status.OK);
        check("hash given in upper case is accepted", rd(new RomSlice(SIZE, good.toUpperCase()), f, "0x100000", "4").status == RomSlice.Status.OK);

        // the first and the last words of the file
        RomSlice s2 = new RomSlice(SIZE, good);
        check("first word", Arrays.equals(rd(s2, f, "0x100000", "4").data, Arrays.copyOfRange(data, 0, 4)));
        check("last word (offset size-4)", Arrays.equals(rd(s2, f, "0x" + Long.toHexString(0x100000L + SIZE - 4), "4").data, Arrays.copyOfRange(data, SIZE - 4, SIZE)));
        RomSlice.Result past = rd(s2, f, "0x" + Long.toHexString(0x100000L + SIZE), "4");
        check("starting at the end of the file: refused (OUT_OF_FILE)", past.status == RomSlice.Status.OUT_OF_FILE && past.data == null);
        check("crossing the end by one word: refused", rd(s2, f, "0x" + Long.toHexString(0x100000L + SIZE - 4), "8").status == RomSlice.Status.OUT_OF_FILE);
        check("a refusal after the check does not add to the total", s2.total() == 8);

        // --- input validation (each refused, the total unchanged, no data) ---
        RomSlice v = new RomSlice(SIZE, good);
        String[][] bad = {
            {"", "16", "BAD_PC_FORMAT"}, {"zz", "16", "BAD_PC_FORMAT"}, {"0x", "16", "BAD_PC_FORMAT"}, {"-0x100000", "16", "BAD_PC_FORMAT"},
            {"0x1000000000000000000", "16", "BAD_PC_FORMAT"}, {"0xffffffffffffffff", "16", "BAD_PC_FORMAT"}, {"0x100 000", "16", "BAD_PC_FORMAT"},
            {"0x１０００００", "16", "BAD_PC_FORMAT"},
            {"0x100000", "", "BAD_LEN_FORMAT"}, {"0x100000", "abc", "BAD_LEN_FORMAT"}, {"0x100000", "-4", "BAD_LEN_FORMAT"},
            {"0x100000", "99999999999", "BAD_LEN_FORMAT"}, {"0x100000", "1.5", "BAD_LEN_FORMAT"},
            {"0xfffff", "16", "PC_BELOW_ROM"}, {"0x0", "16", "PC_BELOW_ROM"},
            {"0x100002", "16", "PC_UNALIGNED"}, {"0x100001", "4", "PC_UNALIGNED"},
            {"0x100000", "0", "LEN_RANGE"}, {"0x100000", "513", "LEN_RANGE"}, {"0x100000", "516", "LEN_RANGE"}, {"0x100000", "1000000", "LEN_RANGE"},
            {"0x100000", "0x1000", "LEN_RANGE"},
            {"0x100000", "6", "LEN_UNALIGNED"}, {"0x100000", "7", "LEN_UNALIGNED"}, {"0x100000", "511", "LEN_UNALIGNED"},
            {"0x7ffffffffffc", "16", "OUT_OF_FILE"},        // 12 digits: parses, far outside the file
            {"0x" + Long.toHexString(0x100000L + 0x7ffffffcL), "512", "OUT_OF_FILE"},
        };
        boolean allRefused = true;
        for (String[] c : bad) {
            RomSlice.Result x = rd(v, f, c[0], c[1]);
            boolean ok = x.status.name().equals(c[2]) && x.data == null && v.total() == 0;
            if (!ok) System.out.println("   unexpected for pc='" + c[0] + "' len='" + c[1] + "': " + x.status + " total " + v.total());
            allRefused &= ok;
        }
        check("every bad input refused with its own status; nothing counted", allRefused);
        check("the largest allowed request (512) is accepted", rd(new RomSlice(SIZE, good), f, "0x100000", "512").status == RomSlice.Status.OK);
        check("length 4 (smallest) is accepted", rd(new RomSlice(SIZE, good), f, "0x100000", "4").status == RomSlice.Status.OK);
        check("length as hex (0x40) is accepted", rd(new RomSlice(SIZE, good), f, "0x100000", "0x40").status == RomSlice.Status.OK);

        // --- the running total: 4096 bytes, only complete reads count ---
        RomSlice q = new RomSlice(SIZE, good);
        boolean eight = true;
        for (int i = 0; i < 8; i++) eight &= rd(q, f, "0x100000", "512").status == RomSlice.Status.OK;
        check("eight 512-byte reads reach exactly 4096", eight && q.total() == 4096);
        RomSlice.Result over = rd(q, f, "0x100000", "4");
        check("the next 4 bytes are refused (QUOTA), total stays 4096", over.status == RomSlice.Status.QUOTA && over.data == null && q.total() == 4096 && over.totalAfter == 4096);
        RomSlice q2 = new RomSlice(SIZE, good);
        for (int i = 0; i < 7; i++) rd(q2, f, "0x100000", "512");
        check("3584 + 516? (too long anyway) refused as a length, 3584 + 512 ok", rd(q2, f, "0x100000", "516").status == RomSlice.Status.LEN_RANGE
                && rd(q2, f, "0x100000", "512").status == RomSlice.Status.OK && q2.total() == 4096);
        RomSlice q3 = new RomSlice(SIZE, good);
        for (int i = 0; i < 7; i++) rd(q3, f, "0x100000", "512");
        rd(q3, f, "0x100000", "508");
        check("4092 + 8 > 4096 refused, + 4 allowed", rd(q3, f, "0x100000", "8").status == RomSlice.Status.QUOTA && rd(q3, f, "0x100000", "4").status == RomSlice.Status.OK && q3.total() == 4096);

        // --- the file is not what is expected: refused, nothing read, nothing counted ---
        RomSlice w = new RomSlice(SIZE, good);
        check("no file", rd(w, new File(dir, "nope.bin"), "0x100000", "4").status == RomSlice.Status.NO_FILE);
        check("null file", rd(w, null, "0x100000", "4").status == RomSlice.Status.NO_FILE);
        check("a directory", rd(w, dir, "0x100000", "4").status == RomSlice.Status.NOT_A_FILE);
        File shorter = write(dir, "short.bin", Arrays.copyOf(data, SIZE - 1));
        RomSlice.Result sz = rd(w, shorter, "0x100000", "4");
        check("a file one byte short: SIZE_MISMATCH, no hash computed", sz.status == RomSlice.Status.SIZE_MISMATCH && sz.sha256 == null && sz.data == null);
        File longer = write(dir, "long.bin", Arrays.copyOf(data, SIZE + 1));
        check("a file one byte long: SIZE_MISMATCH", rd(w, longer, "0x100000", "4").status == RomSlice.Status.SIZE_MISMATCH);
        byte[] flipped = data.clone();
        flipped[1000] ^= 1;
        File fl = write(dir, "flip.bin", flipped);
        RomSlice.Result hm = rd(w, fl, "0x100000", "4");
        check("one flipped bit: HASH_MISMATCH, no bytes returned", hm.status == RomSlice.Status.HASH_MISMATCH && hm.data == null && hm.sha256 != null && !hm.sha256.equals(good));
        check("the mismatch is blocked even for a range that holds the same bytes as the original",
                rd(w, fl, "0x100000", "4").data == null);
        check("none of the refusals counted", w.total() == 0);
        check("the screen text of a mismatch says it did not read and does not call the file the expected one",
                RomSlice.screenText(hm).contains("다릅니다") && !RomSlice.screenText(hm).contains("기대값과 같습니다"));
        check("an empty file", rd(w, write(dir, "empty.bin", new byte[0]), "0x100000", "4").status == RomSlice.Status.SIZE_MISMATCH);

        // --- the original is untouched ---
        check("the ROM file is unchanged after all this", Arrays.equals(Files.readAllBytes(f.toPath()), data) && f.length() == SIZE);

        // --- a read that fails does not count: the file is cut after the size check cannot be simulated, so the pieces are checked ---
        // (the size is re-checked after the read; a handle that shrinks mid-way is a race the test cannot create deterministically)
        RomSlice x = new RomSlice(SIZE, good);
        rd(x, f, "0x100000", "16");
        rd(x, shorter, "0x100000", "16");
        rd(x, f, "0x100001", "16");
        check("a mix of one success and refusals totals only the success", x.total() == 16);

        // --- screen and copy text ---
        RomSlice t = new RomSlice(SIZE, good);
        RomSlice.Result ok = rd(t, f, "0x100020", "20");
        String screen = RomSlice.screenText(ok), copy = RomSlice.copyText(ok);
        check("screen text: hash, offset, total, and the dump", screen.contains(good) && screen.contains("0x20") && screen.contains("20 / 4096") && screen.contains("100020:"));
        check("screen text states that a match is not proof of an Apple original", screen.contains("증명하는 것은 아닙니다"));
        check("copy text: only the request and the dump, 2 dump lines for 20 bytes", copy != null && copy.startsWith("ROM slice pc 0x100020 len 20 (file offset 0x20)\n")
                && copy.split("\n").length == 3);
        check("copy text carries no path and no hash line", !copy.contains(dir.getPath()) && !copy.contains(good) && !copy.contains("/"));
        check("screen text carries no path", !screen.contains(dir.getPath()));
        StringBuilder expectLine = new StringBuilder("100020:");
        for (int i = 0x20; i < 0x30; i++) expectLine.append(String.format(" %02x", data[i] & 0xff));
        check("dump line format: address, 16 bytes in file order", copy.contains(expectLine + "\n"));
        check("no copy text for a refused read", RomSlice.copyText(rd(t, f, "0x100001", "4")) == null);
        check("no ECID / seed / identity words in the texts", !screen.toLowerCase().contains("ecid") && !screen.toLowerCase().contains("seed") && !copy.toLowerCase().contains("ecid"));

        // --- the production instance really carries the constants the app uses ---
        RomSlice prod = RomSlice.forGenuineRom();
        check("production size is 214904", RomSlice.EXPECTED_LENGTH == 214904L);
        check("production hash is the app's ROM hash", RomSlice.EXPECTED_SHA256.equals("fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91"));
        check("the production instance refuses a synthetic file (size does not match)", prod.read(f, "0x100000", "4").status == RomSlice.Status.SIZE_MISMATCH && prod.total() == 0);
        File prodSized = write(dir, "prodsized.bin", new byte[(int) RomSlice.EXPECTED_LENGTH]);
        check("the production instance refuses a file of the right size and the wrong hash", prod.read(prodSized, "0x100000", "4").status == RomSlice.Status.HASH_MISMATCH && prod.total() == 0);

        // --- status messages exist for every status and differ ---
        java.util.HashSet<String> msgs = new java.util.HashSet<>();
        boolean allMsg = true;
        for (RomSlice.Status st : RomSlice.Status.values()) {
            String m = RomSlice.message(st);
            allMsg &= m != null && !m.isEmpty() && !m.equals(st.toString());
            msgs.add(m);
        }
        check("every status has its own message", allMsg && msgs.size() == RomSlice.Status.values().length);

        // --- helpers ---
        check("parseHex", RomSlice.parseHex("0x103f00") == 0x103f00L && RomSlice.parseHex(" 103F00 ") == 0x103f00L && RomSlice.parseHex("0x") == -1
                && RomSlice.parseHex(null) == -1 && RomSlice.parseHex("0xfffffffffffff") == -1 && RomSlice.parseHex("0xffffffffffff") == 0xffffffffffffL);
        check("parseLength", RomSlice.parseLength("256") == 256 && RomSlice.parseLength("0x100") == 256 && RomSlice.parseLength("000000256") == 256
                && RomSlice.parseLength("0000000256") == -1 && RomSlice.parseLength("+4") == -1 && RomSlice.parseLength(null) == -1);

        // a file that disappears is NO_FILE, an unreadable one is an open failure, neither counts
        File gone = write(dir, "gone.bin", data);
        gone.delete();
        check("deleted file: NO_FILE", rd(new RomSlice(SIZE, good), gone, "0x100000", "4").status == RomSlice.Status.NO_FILE);
        File noRead = write(dir, "noread.bin", data);
        boolean could = noRead.setReadable(false, false);
        RomSlice nr = new RomSlice(SIZE, good);
        RomSlice.Result nres = rd(nr, noRead, "0x100000", "4");
        boolean running_as_root = nres.status == RomSlice.Status.OK;   // root reads anything: the case cannot be built there
        check("unreadable file: refused and not counted (skipped as root)", running_as_root || (could && nres.status == RomSlice.Status.OPEN_FAILED && nr.total() == 0));

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
