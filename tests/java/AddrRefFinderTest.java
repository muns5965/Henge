package dev.muns.hengeprobe;

import dev.muns.hengeprobe.AddrRefFinder.Kind;
import dev.muns.hengeprobe.AddrRefFinder.Result;
import dev.muns.hengeprobe.AddrRefFinder.Status;
import java.io.File;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.regex.Pattern;

/**
 * Plain-java test of AddrRefFinder (no framework), on synthetic ROMs only (plus the two real instruction words of the list-head address):
 *   javac -encoding UTF-8 -d out android/app/src/main/java/dev/muns/hengeprobe/RomSlice.java android/app/src/main/java/dev/muns/hengeprobe/AddrRefFinder.java tests/java/AddrRefFinderTest.java && java -cp out dev.muns.hengeprobe.AddrRefFinderTest
 * Every instruction word below is built from its bit fields here, independently of the decoder in AddrRefFinder.
 */
public class AddrRefFinderTest {
    static int fails;
    static final long BASE = RomSlice.BASE;
    static final long T = 0x70030198L;
    interface Check { boolean get() throws Exception; }
    static void check(String name, Check c) { boolean ok; try { ok = c.get(); } catch (Exception e) { ok = false; } check(name, ok); }
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    static final int NOP = 0xd503201f;

    // ---- encoders (from the ARM encoding tables, not from the decoder) ----
    static int adrp(long pc, int rd, long targetPage) {
        long pages = (targetPage >> 12) - (pc >> 12);
        int imm21 = (int) (pages & 0x1fffff);
        return 0x90000000 | (imm21 & 3) << 29 | ((imm21 >> 2) & 0x7ffff) << 5 | rd;
    }
    static int addImm(int sf, int sh, int imm12, int rn, int rd) { return (sf << 31) | 0x11000000 | sh << 22 | imm12 << 10 | rn << 5 | rd; }
    static int subImm(int sf, int sh, int imm12, int rn, int rd) { return (sf << 31) | 0x51000000 | sh << 22 | imm12 << 10 | rn << 5 | rd; }
    static int addsImm(int imm12, int rn, int rd) { return 0xb1000000 | imm12 << 10 | rn << 5 | rd; }
    static int ldrU(int size, int imm12, int rn, int rt) { return size << 30 | 0x39000000 | 1 << 22 | imm12 << 10 | rn << 5 | rt; }
    static int strU(int size, int imm12, int rn, int rt) { return size << 30 | 0x39000000 | imm12 << 10 | rn << 5 | rt; }
    static int movReg(int sf, int rd, int rm) { return (sf << 31) | 0x2a0003e0 | rm << 16 | rd; }              // ORR Rd, XZR, Rm
    static int orrReg(int sf, int rd, int rn, int rm) { return (sf << 31) | 0x2a000000 | rm << 16 | rn << 5 | rd; }   // ORR Rd, Rn, Rm (not a MOV)
    static int movz(int sf, int imm16, int rd) { return (sf << 31) | 0x52800000 | imm16 << 5 | rd; }
    static int movn(int sf, int imm16, int rd) { return (sf << 31) | 0x12800000 | imm16 << 5 | rd; }
    static int movk(int sf, int imm16, int rd) { return (sf << 31) | 0x72800000 | imm16 << 5 | rd; }
    static int bl(long from, long to) { return 0x94000000 | (int) (((to - from) >> 2) & 0x3ffffff); }
    static int b(long from, long to) { return 0x14000000 | (int) (((to - from) >> 2) & 0x3ffffff); }
    static int br(int rn) { return 0xd61f0000 | rn << 5; }
    static int blr(int rn) { return 0xd63f0000 | rn << 5; }
    static int ret(int rn) { return 0xd65f0000 | rn << 5; }
    static int bCond(int off) { return 0x54000000 | ((off >> 2) & 0x7ffff) << 5; }
    static int cbz(int rt, int off) { return 0xb4000000 | ((off >> 2) & 0x7ffff) << 5 | rt; }
    static int tbz(int rt, int off) { return 0x36000000 | ((off >> 2) & 0x3fff) << 5 | rt; }

    static byte[] rom(int words) {   // filler is NOP, which is none of the things searched for
        byte[] r = new byte[words * 4];
        for (int i = 0; i < words; i++) put(r, i, NOP);
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
        File f = File.createTempFile("addrref-secret-name", ".bin");
        f.deleteOnExit();
        Files.write(f.toPath(), d);
        return f;
    }
    static Result run(byte[] d, String target) throws Exception {
        return new AddrRefFinder(d.length, sha(d)).find(file(d), target);
    }
    /** ADRP x8 of T's page at word i, then the given words right after it; returns the ROM. */
    static byte[] seq(int adrpRd, int... follow) {
        byte[] d = rom(256);
        put(d, 10, adrp(pcOf(10), adrpRd, T & ~0xfffL));
        for (int k = 0; k < follow.length; k++) put(d, 11 + k, follow[k]);
        return d;
    }
    static int uses(byte[] d) throws Exception { Result r = run(d, "0x70030198"); return r.status == Status.OK ? r.useTotal : -1; }

    public static void main(String[] a) throws Exception {
        // --- decoder on the real instruction words (ROM 0x10e844 / 0x10e848) ---
        {
            byte[] d = rom(0x10e850 / 4 - 0x100000 / 4 + 4);
            int i = (0x10e844 - 0x100000) / 4;
            put(d, i, 0xD037F908);
            put(d, i + 1, 0x91066108);
            Result r = run(d, "0x70030198");
            check("real words: status OK", r.status == Status.OK);
            check("real words: 1 ADRP page hit at 0x10e844, x8", r.pageTotal == 1 && r.pagesShown.get(0).pc == 0x10e844 && r.pagesShown.get(0).reg == 8);
            check("real words: 1 use, ADD at 0x10e848 from 0x10e844 via x8", r.useTotal == 1 && r.addTotal == 1 && r.usesShown.get(0).pc == 0x10e848
                    && r.usesShown.get(0).adrpPc == 0x10e844 && r.usesShown.get(0).kind == Kind.ADD && r.usesShown.get(0).reg == 8);
            check("adrp encoder agrees with the real word", adrp(0x10e844, 8, 0x70030000L) == 0xD037F908);
            check("add encoder agrees with the real word", addImm(1, 0, 0x198, 8, 8) == 0x91066108);
        }

        // --- ADRP page arithmetic: positive and negative displacement, page boundaries ---
        {
            byte[] d = rom(256);
            long pcLow = pcOf(3), pcHigh = pcOf(200);
            long pageHigh = 0x70030000L, pageLow = 0x00010000L;     // far forward / backward from the ROM
            put(d, 3, adrp(pcLow, 1, pageHigh));
            put(d, 200, adrp(pcHigh, 2, pageLow));
            Result r = run(d, "0x70030fff");
            check("positive displacement ADRP counted for its page", r.status == Status.OK && r.pageTotal == 1 && r.pagesShown.get(0).pc == pcLow && r.pagesShown.get(0).reg == 1);
            r = run(d, "0x10ffc");
            check("negative displacement ADRP counted for its page", r.pageTotal == 1 && r.pagesShown.get(0).pc == pcHigh && r.pagesShown.get(0).reg == 2);
            r = run(d, "0x70031000");
            check("next page is not the page (offset 0xfff / 0x1000 boundary)", r.pageTotal == 0);
            r = run(d, "0x7002ffff");
            check("previous page is not the page", r.pageTotal == 0);
            // ADRP at the last bytes of a 4 KiB page of the ROM uses that page as its base, not the next one
            byte[] e = rom(2048);
            int w = 1023;                                            // word 1023 = pc 0x100ffc (last word of the page 0x100000)
            put(e, w, adrp(pcOf(w), 5, 0x100000L));                  // same page: displacement 0
            put(e, w + 1, adrp(pcOf(w + 1), 6, 0x100000L));          // first word of the next page: displacement -1 page
            r = run(e, "0x100000");
            check("ADRP page base is pc & ~0xfff (last and first word around a page edge)", r.pageTotal == 2);
        }

        // --- the ADR word (bit 31 clear) is not an ADRP, even with the same fields; an ADRP in the very first word is found ---
        {
            byte[] d = rom(64);
            put(d, 10, adrp(pcOf(10), 8, T & ~0xfffL) & 0x7fffffff);       // ADR x8, <same imm>
            put(d, 11, addImm(1, 0, 0x198, 8, 9));
            Result r = run(d, "0x70030198");
            check("ADR (not ADRP) with the page's fields is not counted", r.pageTotal == 0 && r.useTotal == 0);
            byte[] e = rom(64);
            put(e, 0, adrp(pcOf(0), 8, T & ~0xfffL));
            put(e, 1, addImm(1, 0, 0x198, 8, 9));
            r = run(e, "0x70030198");
            check("ADRP in the first word of the ROM is found with its use", r.pageTotal == 1 && r.pagesShown.get(0).pc == pcOf(0) && r.useTotal == 1);
        }

        // --- accepted forms ---
        check("ADRP x8; ADD x8,x8,#0x198 is a use", uses(seq(8, addImm(1, 0, 0x198, 8, 8))) == 1);
        check("ADRP x8; ADD x9,x8,#0x198 is a use, x8 still tracked", uses(seq(8, addImm(1, 0, 0x198, 8, 9), addImm(1, 0, 0x198, 8, 10))) == 2);
        check("ADRP x8; LDR x0,[x8,#0x198] (64-bit, imm12=0x33) is a use", uses(seq(8, ldrU(3, 0x198 / 8, 8, 0))) == 1);
        check("ADRP x8; LDR w0,[x8,#0x198] (32-bit, imm12=0x66) is a use", uses(seq(8, ldrU(2, 0x198 / 4, 8, 0))) == 1);
        check("ADRP x8; STR x0,[x8,#0x198] is a use", uses(seq(8, strU(3, 0x198 / 8, 8, 0))) == 1);
        check("ADRP x8; STR w0,[x8,#0x198] is a use", uses(seq(8, strU(2, 0x198 / 4, 8, 0))) == 1);
        {
            Result r = run(seq(8, addImm(1, 0, 0x198, 8, 9), ldrU(3, 0x33, 8, 0), strU(2, 0x66, 8, 1)), "0x70030198");
            check("kinds counted separately", r.useTotal == 3 && r.addTotal == 1 && r.ldrTotal == 1 && r.strTotal == 1);
            check("kinds recorded in address order", r.usesShown.get(0).kind == Kind.ADD && r.usesShown.get(1).kind == Kind.LDR && r.usesShown.get(2).kind == Kind.STR);
        }
        check("ADD with sh=1: imm12<<12 only matches an offset that big (target page offset 0)", uses(seq(8, addImm(1, 1, 0x198, 8, 9))) == 0);
        {
            byte[] d = rom(64);
            put(d, 1, adrp(pcOf(1), 3, 0x70031000L));
            put(d, 2, addImm(1, 1, 1, 3, 4));      // ADD x4,x3,#1,LSL #12 = page + 0x1000 -> not inside the page
            Result r = run(d, "0x70031000");
            check("ADD sh=1 imm12=1 is page+0x1000, not the target of the page (offset 0)", r.useTotal == 0);
            put(d, 2, addImm(1, 1, 0, 3, 4));      // ADD x4,x3,#0,LSL #12 = the page itself
            r = run(d, "0x70031000");
            check("ADD sh=1 imm12=0 makes the page itself (offset 0)", r.useTotal == 1);
            put(d, 2, addImm(1, 0, 0, 3, 4));
            r = run(d, "0x70031000");
            check("ADD sh=0 imm12=0 makes the page itself (offset 0)", r.useTotal == 1);
        }

        // --- rejected: wrong offset, wrong register, unsupported encodings ---
        check("same page, other offset: no use", uses(seq(8, addImm(1, 0, 0x19c, 8, 9), ldrU(3, 0x34, 8, 0))) == 0);
        check("other base register: no use", uses(seq(8, addImm(1, 0, 0x198, 9, 8), ldrU(3, 0x33, 9, 0), strU(3, 0x33, 7, 0))) == 0);
        check("32-bit ADD is not an address build: no use", uses(seq(8, addImm(0, 0, 0x198, 8, 9))) == 0);
        check("ADDS (flags) is not decoded: no use", uses(seq(8, addsImm(0x198, 8, 9))) == 0);
        check("SUB is not a use", uses(seq(8, subImm(1, 0, 0x198, 8, 9))) == 0);
        check("LDRB / LDRH / LDRSW (other size or opc) are not decoded", uses(seq(8,
                0x39400000 | 0x198 << 10 | 8 << 5,                 // LDRB w0,[x8,#0x198]
                0x79400000 | 0xcc << 10 | 8 << 5,                  // LDRH w0,[x8,#0x198]
                0xb9800000 | 0x66 << 10 | 8 << 5,                  // LDRSW x0,[x8,#0x198]
                0x39000000 | 0x198 << 10 | 8 << 5)) == 0);         // STRB w0,[x8,#0x198]
        check("PRFM unsigned immediate (size 3, opc 2) is not decoded", uses(seq(8, 0xf9800000 | 0x33 << 10 | 8 << 5)) == 0);
        check("SIMD / FP LDR (V=1) is not decoded", uses(seq(8, ldrU(3, 0x33, 8, 0) | 1 << 26, strU(2, 0x66, 8, 0) | 1 << 26)) == 0);
        check("LDUR / STUR (unscaled, imm9) not decoded", uses(seq(8, 0xf8400000 | (0x198 & 0x1ff) << 12 | 8 << 5, 0xf8000000 | (0x198 & 0x1ff) << 12 | 8 << 5)) == 0);
        check("LDR pre-index / post-index not decoded", uses(seq(8, 0xf8400c00 | (0x198 & 0x1ff) << 12 | 8 << 5, 0xf8400400 | (0x198 & 0x1ff) << 12 | 8 << 5)) == 0);
        check("STP / LDP not decoded", uses(seq(8, 0xa9000000 | 0x33 << 15 | 8 << 5, 0xa9400000 | 0x33 << 15 | 8 << 5)) == 0);
        check("ADRP with destination XZR is counted in stage 1 but not followed", () -> {
            try {
                byte[] d = seq(31, addImm(1, 0, 0x198, 31, 9));
                Result r = run(d, "0x70030198");
                return r.pageTotal == 1 && r.useTotal == 0;
            } catch (Exception e) { return false; }
        });

        // --- overwrite rules: candidate first, then stop ---
        check("MOV x8,x0 then ADD x9,x8: excluded", uses(seq(8, movReg(1, 8, 0), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("MOV w8,w0 (32-bit) then ADD x9,x8: excluded", uses(seq(8, movReg(0, 8, 0), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("MOV x9,x0 (other register) does not stop", uses(seq(8, movReg(1, 9, 0), addImm(1, 0, 0x198, 8, 10))) == 1);
        check("ORR x8,x1,x0 (not a MOV form) is not recognised as an overwrite", uses(seq(8, orrReg(1, 8, 1, 0), addImm(1, 0, 0x198, 8, 9))) == 1);
        check("MOVZ x8 stops", uses(seq(8, movz(1, 5, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("MOVZ w8 (32-bit) stops", uses(seq(8, movz(0, 5, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("MOVN x8 stops", uses(seq(8, movn(1, 5, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("MOVK x8 stops (conservative): matching ADD after it is excluded", uses(seq(8, movk(1, 0x1234, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("MOVK x9 (other register) does not stop", uses(seq(8, movk(1, 0x1234, 9), addImm(1, 0, 0x198, 8, 10))) == 1);
        check("second ADRP x8 stops", uses(seq(8, adrp(pcOf(11), 8, 0x80000000L), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("second ADRP into x9 does not stop", uses(seq(8, adrp(pcOf(11), 9, 0x80000000L), addImm(1, 0, 0x198, 8, 9))) == 1);
        check("ADD x8,x8,#0x198: candidate is recorded, then the search ends", () -> {
            try { return uses(seq(8, addImm(1, 0, 0x198, 8, 8), addImm(1, 0, 0x198, 8, 9))) == 1; } catch (Exception e) { return false; }
        });
        check("ADD w8,w8,#1 (32-bit write) stops", uses(seq(8, addImm(0, 0, 1, 8, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("SUB x8,x8,#4 stops", uses(seq(8, subImm(1, 0, 4, 8, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("ADD x9,x8,#4 (other destination) does not stop", uses(seq(8, addImm(1, 0, 4, 8, 9), addImm(1, 0, 0x198, 8, 10))) == 1);
        check("LDR x8,[x8,#0x198] matching: candidate recorded, then stop", uses(seq(8, ldrU(3, 0x33, 8, 8), addImm(1, 0, 0x198, 8, 9))) == 1);
        check("LDR x8,[x8,#0x10] (not matching) overwrites: later ADD excluded", uses(seq(8, ldrU(3, 2, 8, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("LDR w8,[x8,#0x10] (32-bit load into the register) overwrites", uses(seq(8, ldrU(2, 4, 8, 8), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("LDR x0,[x8,#0x10] (other target) does not stop", uses(seq(8, ldrU(3, 2, 8, 0), addImm(1, 0, 0x198, 8, 9))) == 1);
        check("STR x8,[x9,#0] (reads x8) does not stop", uses(seq(8, strU(3, 0, 9, 8), addImm(1, 0, 0x198, 8, 9))) == 1);
        check("STR x0,[x8,#0x198] then ADD x9,x8: STR does not stop (both counted)", uses(seq(8, strU(3, 0x33, 8, 0), addImm(1, 0, 0x198, 8, 9))) == 2);

        // --- branches and calls ---
        check("BL stops: ADD after it excluded", uses(seq(8, bl(pcOf(11), pcOf(100)), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("BLR stops", uses(seq(8, blr(16), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("B stops", uses(seq(8, b(pcOf(11), pcOf(100)), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("BR stops", uses(seq(8, br(16), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("RET stops", uses(seq(8, ret(30), addImm(1, 0, 0x198, 8, 9))) == 0);
        check("candidate before the BL is kept", uses(seq(8, addImm(1, 0, 0x198, 8, 9), bl(pcOf(12), pcOf(100)), addImm(1, 0, 0x198, 8, 10))) == 1);
        check("B.cond is crossed (documented: syntactic candidates)", uses(seq(8, bCond(8), addImm(1, 0, 0x198, 8, 9))) == 1);
        check("CBZ is crossed", uses(seq(8, cbz(0, 8), addImm(1, 0, 0x198, 8, 9))) == 1);
        check("TBZ is crossed", uses(seq(8, tbz(0, 8), addImm(1, 0, 0x198, 8, 9))) == 1);

        // --- window: 1st..8th after the ADRP counted, 9th not (independent of any stop rule) ---
        {
            for (int pos = 1; pos <= 10; pos++) {
                int[] f = new int[pos];
                java.util.Arrays.fill(f, NOP);
                f[pos - 1] = addImm(1, 0, 0x198, 8, 9);
                boolean want = pos <= 8;
                check("ADD as instruction " + pos + " after the ADRP: " + (want ? "found" : "not found"), uses(seq(8, f)) == (want ? 1 : 0));
            }
            int[] f = new int[9];
            java.util.Arrays.fill(f, NOP);
            f[7] = bl(pcOf(18), pcOf(100));
            f[8] = addImm(1, 0, 0x198, 8, 9);
            check("BL as 8th and ADD as 9th: not found", uses(seq(8, f)) == 0);
            f = new int[4];
            java.util.Arrays.fill(f, NOP);
            f[2] = bl(pcOf(13), pcOf(100));
            f[3] = addImm(1, 0, 0x198, 8, 9);
            check("BL as 3rd and ADD as 4th: not found (stop rule, inside the window)", uses(seq(8, f)) == 0);
        }
        {   // ADRP at the very end of the ROM: the window is cut by the end of the file, not read past it
            byte[] d = rom(16);
            put(d, 14, adrp(pcOf(14), 8, T & ~0xfffL));
            put(d, 15, addImm(1, 0, 0x198, 8, 9));
            Result r = run(d, "0x70030198");
            check("ADRP near the end of the ROM: window ends at the file end", r.status == Status.OK && r.useTotal == 1);
            byte[] e = rom(16);
            put(e, 15, adrp(pcOf(15), 8, T & ~0xfffL));
            r = run(e, "0x70030198");
            check("ADRP as the last word: counted, no use, no crash", r.pageTotal == 1 && r.useTotal == 0);
        }

        // --- counting: whole ROM, shown limit, unique (ADRP, instruction, kind) ---
        {
            byte[] d = rom(2000);
            int n = 40;
            for (int k = 0; k < n; k++) {
                int w = 10 + k * 30;
                put(d, w, adrp(pcOf(w), 8, T & ~0xfffL));
                put(d, w + 1, addImm(1, 0, 0x198, 8, 9));
            }
            Result r = run(d, "0x70030198");
            check("40 pages: total 40, 32 shown", r.pageTotal == 40 && r.pagesShown.size() == 32);
            check("40 uses: total 40, 32 shown, search did not stop at 32", r.useTotal == 40 && r.usesShown.size() == 32 && r.addTotal == 40);
            check("shown are the first 32 in address order", r.pagesShown.get(0).pc == pcOf(10) && r.pagesShown.get(31).pc == pcOf(10 + 31 * 30) && r.usesShown.get(31).adrpPc == pcOf(10 + 31 * 30));
            String s = AddrRefFinder.screenText(r);
            check("screen says 총 40건 중 32건 표시 for both stages", s.contains("1단계 ADRP(같은 페이지): 총 40건 중 32건 표시") && s.contains("(ADD 40, LDR 0, STR 0) 중 32건 표시"));
        }
        {   // one instruction reachable from two ADRPs of the same register: two pairs, not one
            byte[] d = rom(64);
            put(d, 10, adrp(pcOf(10), 8, T & ~0xfffL));
            put(d, 11, adrp(pcOf(11), 9, T & ~0xfffL));
            put(d, 12, addImm(1, 0, 0x198, 8, 12));
            put(d, 13, addImm(1, 0, 0x198, 9, 13));
            Result r = run(d, "0x70030198");
            check("two ADRPs, each with its own base register: 2 pages, 2 pairs", r.pageTotal == 2 && r.useTotal == 2);
            put(d, 11, adrp(pcOf(11), 8, T & ~0xfffL));          // the second ADRP rewrites x8; the first one's search ends there
            put(d, 13, NOP);
            r = run(d, "0x70030198");
            check("second ADRP of the same register ends the first search: the ADD pairs with the second only", r.pageTotal == 2 && r.useTotal == 1 && r.usesShown.get(0).adrpPc == pcOf(11));
            put(d, 11, adrp(pcOf(11), 9, T & ~0xfffL));
            put(d, 12, NOP);
            put(d, 13, NOP);
            put(d, 14, addImm(1, 0, 0x198, 8, 14));
            put(d, 15, ldrU(3, 0x33, 9, 0));
            r = run(d, "0x70030198");
            check("same follow instruction word position is its own pair per ADRP (x8 -> ADD@14, x9 -> LDR@15)", r.useTotal == 2 && r.addTotal == 1 && r.ldrTotal == 1);
        }

        // --- target handling ---
        {
            byte[] d = seq(8, addImm(1, 0, 0x198, 8, 9));
            check("target without 0x accepted", run(d, "70030198").status == Status.OK);
            check("target that is not hex is refused", run(d, "zz").status == Status.BAD_TARGET_FORMAT);
            check("empty target is refused", run(d, "").status == Status.BAD_TARGET_FORMAT);
            check("null target is refused", new AddrRefFinder(d.length, sha(d)).find(file(d), null).status == Status.BAD_TARGET_FORMAT);
            check("RAM address (above the ROM) is accepted, not range-checked against the ROM", run(d, "0x70030198").status == Status.OK);
            check("odd target accepted (no alignment rule): finds nothing", run(d, "0x70030199").useTotal == 0);
            check("target 0 accepted: 0 hits, not a refusal", () -> {
                try { Result r = run(d, "0"); return r.status == Status.OK && r.pageTotal == 0; } catch (Exception e) { return false; }
            });
        }

        // --- file refusals (same gate as the branch search) ---
        {
            byte[] d = seq(8, addImm(1, 0, 0x198, 8, 9));
            check("missing file", new AddrRefFinder(d.length, sha(d)).find(new File("/nonexistent/x.bin"), "0x70030198").status == Status.NO_FILE);
            check("null file", new AddrRefFinder(d.length, sha(d)).find(null, "0x70030198").status == Status.NO_FILE);
            File dir = Files.createTempDirectory("addrref-dir").toFile();
            dir.deleteOnExit();
            check("directory", new AddrRefFinder(d.length, sha(d)).find(dir, "0x70030198").status == Status.NOT_A_FILE);
            check("size mismatch is refused before any search", new AddrRefFinder(d.length + 4, sha(d)).find(file(d), "0x70030198").status == Status.SIZE_MISMATCH);
            byte[] other = d.clone();
            other[0] ^= 1;
            Result r = new AddrRefFinder(d.length, sha(other)).find(file(d), "0x70030198");
            check("hash mismatch is refused, nothing searched", r.status == Status.HASH_MISMATCH && r.pageTotal == 0 && r.useTotal == 0 && r.pagesShown.isEmpty());
            check("3-byte tail: size is not a multiple of 4 is a size refusal here (expectation says so)", new AddrRefFinder(d.length - 1, sha(d)).find(file(d), "0x70030198").status == Status.SIZE_MISMATCH);
            byte[] odd = new byte[d.length + 3];
            System.arraycopy(d, 0, odd, 0, d.length);
            r = new AddrRefFinder(odd.length, sha(odd)).find(file(odd), "0x70030198");
            check("size with a 3-byte tail: the whole words are searched, the tail is ignored", r.status == Status.OK && r.useTotal == 1);
        }
        check("production constants are the ones RomSlice uses", () -> {
            try {
                AddrRefFinder g = AddrRefFinder.forGenuineRom();
                boolean small = g.find(file(rom(10)), "0x70030198").status == Status.SIZE_MISMATCH;
                boolean exactSizeWrongHash = g.find(file(rom((int) (RomSlice.EXPECTED_LENGTH / 4))), "0x70030198").status == Status.HASH_MISMATCH;
                return small && exactSizeWrongHash;
            } catch (Exception e) { return false; }
        });

        // --- output: addresses only, no ROM bytes, no path; 0 hits differs from a refusal ---
        {
            byte[] d = seq(8, addImm(1, 0, 0x198, 8, 9));
            Result r = run(d, "0x70030198");
            String s = AddrRefFinder.screenText(r), c = AddrRefFinder.copyText(r);
            check("screen shows the use, the register and the caveats", s.contains("0x10002c") && s.contains("ADRP x8") && s.contains("구문상 후보") && s.contains("MOVK") && s.contains("조건 분기"));
            check("copy text has the pair and no file name", c != null && c.contains("ADRP 0x100028 → ADD 0x10002c") && !c.contains("secret-name") && !s.contains("secret-name"));
            check("no 8-hex-digit instruction words in the output", !Pattern.compile("(?i)\\b0x[0-9a-f]{8}\\b").matcher(s.replace("0x70030198", "").replace("0x70030000", "")).find());
            Result none = run(rom(64), "0x70030198");
            check("no hit: 0건 shown for both stages, copy text exists", AddrRefFinder.screenText(none).contains("1단계 ADRP(같은 페이지): 0건") && AddrRefFinder.screenText(none).contains("2단계 정밀 후보(ADRP + 8명령 이내): 0건") && AddrRefFinder.copyText(none) != null);
            Result bad = run(d, "zz");
            check("refusal: reason shown, no 0건, nothing to copy", AddrRefFinder.screenText(bad).contains("거부/실패") && !AddrRefFinder.screenText(bad).contains("0건") && AddrRefFinder.copyText(bad) == null);
        }

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        if (fails != 0) System.exit(1);
    }
}
