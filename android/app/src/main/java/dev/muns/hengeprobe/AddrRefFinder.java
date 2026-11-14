package dev.muns.hengeprobe;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Finds the places in the boot ROM whose instructions build a given DATA address (a RAM address such as a list head) the way AArch64 code
 * usually does: ADRP for the 4 KiB page, then ADD / LDR / STR with the offset inside the page. Plain Java, no Android classes, so it has a JVM test.
 *
 * Stage 1: every ADRP word whose page is the page of the target (total counted over the whole ROM, the first MAX_SHOWN shown with their
 * destination register).
 * Stage 2: after each such ADRP, the next WINDOW instructions (the 1st to the 8th after it, the 9th is not looked at) are checked for a use of
 * that register that makes exactly the target:
 *   - ADD (immediate, 64 bit, no flags): base register = the ADRP register, offset = imm12 << (12 * sh)
 *   - LDR / STR (unsigned immediate, general register, any of byte/half-word/32-bit/64-bit): base register = the ADRP register,
 *     offset = imm12 * access size (1, 2, 4 or 8)
 * Per instruction the order is: first the candidate check, then "does this instruction overwrite the ADRP register" (if so, stop looking after it),
 * then "is it a branch or call" (B, BL, BR, BLR, RET: stop). So ADD x8,x8,#off is still a candidate and ends the search, and ADD x9,x8,#off or
 * STR x0,[x8,#off] leave x8 tracked.
 * Overwrites that are recognised (and only these; nothing else is guessed): MOV (ORR Rd,XZR,Rm), MOVZ, MOVN, MOVK, ADRP, ADD/SUB immediate,
 * LDR unsigned immediate (any size) with Rt = the register, each in the 32 bit form too (a w write clears the top half). MOVK is a conservative
 * stop: it keeps part of the old value, but nothing guarantees the page is still there, so it ends the search; a real address built that way can
 * be missed.
 *
 * What a hit is: a syntactic candidate. It is NOT proof that the code uses the register that way at run time: conditional branches
 * (B.cond, CBZ, CBNZ, TBZ, TBNZ) are not followed or stopped at, so a pair can be separated by a branch that skips one of them; other
 * writes to the register are not recognised; and the address can also be built another way (a literal pool, MOVZ/MOVK, a pointer passed in a
 * register) which this does not look for. Encodings it does not decode (LDUR, pre/post-index, LDRSB/LDRSH/LDRSW, STP, SIMD, flag-setting ADD) never
 * become candidates. A search with no hit therefore does not mean "nothing uses that address". ADRP with destination 31 (XZR) is counted in
 * stage 1 but not followed.
 *
 * File handling is the same as RomBranchFinder: the app's own ROM copy, opened "r" only, size checked before allocating, read whole in one buffer,
 * SHA-256 over that buffer, search over that buffer. Only addresses and register numbers are reported, never ROM bytes, so RomSlice's 4096-byte
 * limit is not drawn on. The ROM is mapped at guest address 0x100000 from the first byte of the file.
 */
public final class AddrRefFinder {
    public static final int MAX_SHOWN = 32;
    public static final int WINDOW = 8;
    public static final String DEFAULT_TARGET = "0x70030198";

    public enum Status {
        OK,
        BAD_TARGET_FORMAT,
        NO_FILE, NOT_A_FILE, OPEN_FAILED, SIZE_MISMATCH, READ_FAILED, CHANGED, HASH_FAILED, HASH_MISMATCH
    }

    public enum Kind { ADD, LDR, STR }

    /** An ADRP that builds the page of the target. */
    public static final class Page {
        public final long pc;
        public final int reg;
        Page(long pc, int reg) { this.pc = pc; this.reg = reg; }
    }

    /** One (ADRP, following instruction, kind) pair. */
    public static final class Use {
        public final long adrpPc, pc;
        public final Kind kind;
        public final int reg;          // the ADRP register (the base register of the instruction)
        Use(long adrpPc, long pc, Kind kind, int reg) { this.adrpPc = adrpPc; this.pc = pc; this.kind = kind; this.reg = reg; }
    }

    public static final class Result {
        public Status status = Status.OK;
        public long target;
        public long fileSize = -1;
        public String sha256;
        public int pageTotal;                                   // every ADRP of the target's page in the ROM, shown or not
        public final List<Page> pagesShown = new ArrayList<>();
        public int useTotal, addTotal, ldrTotal, strTotal;      // every (ADRP, instruction, kind) pair
        public final List<Use> usesShown = new ArrayList<>();
        public String message() { return AddrRefFinder.message(status); }
    }

    private final long expectLength;
    private final String expectSha;

    public static AddrRefFinder forGenuineRom() { return new AddrRefFinder(RomSlice.EXPECTED_LENGTH, RomSlice.EXPECTED_SHA256); }

    public AddrRefFinder(long expectLength, String expectSha256) {
        this.expectLength = expectLength;
        this.expectSha = expectSha256.toLowerCase(Locale.ROOT);
    }

    /** targetText is a guest (RAM) address, "0x70030198". Returns the candidates, or the reason the search was refused. */
    public Result find(File f, String targetText) {
        Result r = new Result();
        long target = RomSlice.parseHex(targetText);
        if (target < 0) return fail(r, Status.BAD_TARGET_FORMAT);
        r.target = target;
        if (f == null || !f.exists()) return fail(r, Status.NO_FILE);
        if (!f.isFile()) return fail(r, Status.NOT_A_FILE);

        byte[] rom;
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            long size = raf.length();
            r.fileSize = size;
            if (size != expectLength || size > Integer.MAX_VALUE) return fail(r, Status.SIZE_MISMATCH);   // before allocating anything
            rom = new byte[(int) size];
            try {
                raf.readFully(rom);
            } catch (EOFException e) {
                return fail(r, Status.READ_FAILED);
            }
            if (raf.length() != size) return fail(r, Status.CHANGED);
        } catch (IOException e) {
            return fail(r, r.fileSize < 0 ? Status.OPEN_FAILED : Status.READ_FAILED);
        }
        String sha;
        try {
            sha = sha256(rom);
        } catch (Exception e) {
            return fail(r, Status.HASH_FAILED);
        }
        r.sha256 = sha;
        if (!sha.equals(expectSha)) return fail(r, Status.HASH_MISMATCH);
        scan(rom, RomSlice.BASE, target, r);
        return r;
    }

    static int word(byte[] rom, int off) {
        return (rom[off] & 0xff) | (rom[off + 1] & 0xff) << 8 | (rom[off + 2] & 0xff) << 16 | (rom[off + 3] & 0xff) << 24;
    }

    /** The page an ADRP word at pc builds: (pc & ~0xfff) + (SignExtend(immhi:immlo) << 12). w must be an ADRP. */
    static long adrpPage(int w, long pc) {
        int immlo = (w >>> 29) & 3, immhi = (w >>> 5) & 0x7ffff;
        int imm21 = (immhi << 2) | immlo;
        imm21 = (imm21 << 11) >> 11;                 // sign-extend 21 bits
        return (pc & ~0xfffL) + ((long) imm21 << 12);
    }

    static boolean isAdrp(int w) { return (w & 0x9f000000) == 0x90000000; }

    /** Walks every 4-byte aligned word of rom from its first byte (little endian); counts and records as described in the class comment. */
    static void scan(byte[] rom, long base, long target, Result r) {
        long targetPage = target & ~0xfffL;
        int words = rom.length / 4;
        for (int i = 0; i < words; i++) {
            int w = word(rom, i * 4);
            if (!isAdrp(w)) continue;
            long pc = base + 4L * i;
            if (adrpPage(w, pc) != targetPage) continue;
            int reg = w & 31;
            r.pageTotal++;
            if (r.pagesShown.size() < MAX_SHOWN) r.pagesShown.add(new Page(pc, reg));
            if (reg == 31) continue;                 // XZR: nothing to follow
            for (int j = i + 1; j <= i + WINDOW && j < words; j++) {
                int x = word(rom, j * 4);
                long xpc = base + 4L * j;
                Kind k = candidate(x, reg, target);
                if (k != null) record(r, new Use(pc, xpc, k, reg));
                if (overwrites(x, reg)) break;
                if (endsStraightLine(x)) break;
            }
        }
    }

    private static void record(Result r, Use u) {
        r.useTotal++;
        switch (u.kind) {
            case ADD: r.addTotal++; break;
            case LDR: r.ldrTotal++; break;
            default: r.strTotal++; break;
        }
        if (r.usesShown.size() < MAX_SHOWN) r.usesShown.add(u);
    }

    /** ADD (immediate, 64 bit, no flags) or LDR/STR (unsigned immediate, any size) from reg that makes exactly target; else null. */
    static Kind candidate(int x, int reg, long target) {
        long inPage = target & 0xfffL;               // the ADRP page is the target's page, so the instruction must add exactly this
        int rn = (x >>> 5) & 31;
        if (rn != reg) return null;
        int imm12 = (x >>> 10) & 0xfff;
        if ((x & 0xff000000) == 0x91000000) {        // ADD (immediate), 64 bit, no flags; sh (bit 22) 1 = imm12 << 12
            long off = ((x >>> 22) & 1) != 0 ? (long) imm12 << 12 : imm12;
            return off == inPage ? Kind.ADD : null;
        }
        if ((x & 0x3f000000) == 0x39000000) {        // load/store register, unsigned immediate, general registers (V = 0)
            int size = x >>> 30, opc = (x >>> 22) & 3;
            if (opc > 1) return null;   // only STR (opc 0) and LDR (opc 1), any size (byte/half-word/32/64-bit); LDRSB/LDRSH/LDRSW, PRFM are not decoded
            long off = (long) imm12 << size;
            if (off != inPage) return null;
            return opc == 0 ? Kind.STR : Kind.LDR;
        }
        return null;
    }

    /** True if x is one of the recognised instructions that write the register reg (see the class comment); anything else is assumed not to. */
    static boolean overwrites(int x, int reg) {
        int rd = x & 31;
        if ((x & 0x7f200000) == 0x2a000000 && ((x >>> 5) & 31) == 31) return rd == reg;      // ORR Rd, XZR, Rm (MOV, register), 32 or 64 bit
        if ((x & 0x1f800000) == 0x12800000 && ((x >>> 29) & 3) != 1) return rd == reg;       // MOVN (opc 0), MOVZ (2), MOVK (3)
        if (isAdrp(x)) return rd == reg;
        if ((x & 0x7f000000) == 0x11000000 || (x & 0x7f000000) == 0x51000000) return rd == reg;   // ADD / SUB (immediate), no flags
        if ((x & 0x3f000000) == 0x39000000) {                                                     // LDR unsigned immediate, any size, into reg
            int opc = (x >>> 22) & 3;
            return opc == 1 && rd == reg;
        }
        return false;
    }

    /** B, BL, BR, BLR, RET: a call may change the register, an unconditional branch ends the straight line. */
    static boolean endsStraightLine(int x) {
        int op = x & 0xfc000000;
        if (op == 0x14000000 || op == 0x94000000) return true;   // B, BL
        int r = x & 0xfffffc1f;
        return r == 0xd61f0000 || r == 0xd63f0000 || r == 0xd65f0000;   // BR, BLR, RET
    }

    private static Result fail(Result r, Status s) {
        r.status = s;
        return r;
    }

    private static String sha256(byte[] data) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest(data)) sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }

    public static String message(Status s) {
        switch (s) {
            case OK: return "검색 성공";
            case BAD_TARGET_FORMAT: return "대상 주소 형식이 틀렸습니다 (16진수, 예: 0x70030198)";
            case NO_FILE: return "내부 ROM 파일이 없습니다";
            case NOT_A_FILE: return "내부 ROM 경로가 파일이 아닙니다";
            case OPEN_FAILED: return "ROM 파일을 열지 못했습니다";
            case SIZE_MISMATCH: return "ROM 파일 크기가 기대와 다릅니다 (읽지 않았습니다)";
            case READ_FAILED: return "ROM을 끝까지 읽지 못했습니다 (검색하지 않았습니다)";
            case CHANGED: return "읽는 동안 파일 크기가 바뀌었습니다 (검색하지 않았습니다)";
            case HASH_FAILED: return "SHA-256 계산에 실패했습니다 (검색하지 않았습니다)";
            case HASH_MISMATCH: return "SHA-256이 앱의 기대값과 다릅니다 (검색하지 않았습니다)";
            default: return s.toString();
        }
    }

    /** What the screen shows: the verdict, the target, both stages (addresses and register numbers only) and what the search cannot say. */
    public static String screenText(Result r) {
        StringBuilder sb = new StringBuilder("주소 참조 검색 (ADRP + ADD/LDR/STR, 읽기 전용, VM 실행 없음)\n");
        if (r.fileSize >= 0) sb.append("파일 크기: ").append(r.fileSize).append(" 바이트\n");
        if (r.sha256 != null) {
            sb.append("SHA-256: ").append(r.sha256).append('\n');
            sb.append(r.status == Status.HASH_MISMATCH ? "→ 앱의 기대값과 다릅니다\n"
                    : "→ 앱의 기대값과 같습니다 (Apple 원본임을 증명하는 것은 아닙니다)\n");
        }
        if (r.status != Status.OK) {
            sb.append("거부/실패: ").append(r.message()).append('\n');
            return sb.toString();
        }
        appendResult(sb, r);
        sb.append("※ 구문상 후보입니다. 실제 실행에서 그 레지스터로 그 주소를 만든다는 증거가 아닙니다.\n");
        sb.append("※ ADRP 뒤 ").append(WINDOW).append("개 명령까지만 봅니다. 분기·호출(B, BL, BR, BLR, RET)에서 멈추고, 조건 분기(B.cond, CBZ, CBNZ, TBZ, TBNZ)는 가로지르므로 건너뛰는 경로의 조합이 섞일 수 있습니다.\n");
        sb.append("※ 기준 레지스터를 MOV·MOVZ·MOVN·MOVK·ADRP·ADD/SUB·LDR로 덮어쓰면 멈춥니다. MOVK에서 멈추는 것은 보수적 규칙이라 그렇게 만든 실제 주소는 놓칠 수 있습니다. 그 밖의 쓰기는 알아보지 않습니다.\n");
        sb.append("※ 해석하지 않는 인코딩(LDUR, pre/post-index, LDRSB·LDRSH·LDRSW, STP, SIMD, 플래그 설정 ADD)은 후보가 되지 않습니다. 0건이어도 그 주소를 쓰는 코드가 없다는 뜻이 아닙니다.\n");
        return sb.toString();
    }

    /** What the copy button puts on the clipboard: the target and the addresses, nothing else. Null unless the search ran. */
    public static String copyText(Result r) {
        if (r.status != Status.OK) return null;
        StringBuilder sb = new StringBuilder();
        appendResult(sb, r);
        sb.append("(ADRP+ADD/LDR/STR 구문상 후보. 사용 확정 아님)\n");
        return sb.toString();
    }

    private static void appendResult(StringBuilder sb, Result r) {
        sb.append(String.format(Locale.ROOT, "검색 대상: 0x%x (페이지 0x%x, 페이지 내 0x%x)\n", r.target, r.target & ~0xfffL, r.target & 0xfffL));
        if (r.pageTotal == 0) {
            sb.append("1단계 ADRP(같은 페이지): 0건\n");
        } else {
            sb.append("1단계 ADRP(같은 페이지): 총 ").append(r.pageTotal).append("건");
            if (r.pageTotal > r.pagesShown.size()) sb.append(" 중 ").append(r.pagesShown.size()).append("건 표시");
            sb.append('\n');
            for (Page p : r.pagesShown) sb.append(String.format(Locale.ROOT, "  0x%x  ADRP x%d\n", p.pc, p.reg));
        }
        if (r.useTotal == 0) {
            sb.append("2단계 정밀 후보(ADRP + ").append(WINDOW).append("명령 이내): 0건\n");
        } else {
            sb.append("2단계 정밀 후보(ADRP + ").append(WINDOW).append("명령 이내): 총 ").append(r.useTotal).append("건 (ADD ")
                    .append(r.addTotal).append(", LDR ").append(r.ldrTotal).append(", STR ").append(r.strTotal).append(")");
            if (r.useTotal > r.usesShown.size()) sb.append(" 중 ").append(r.usesShown.size()).append("건 표시");
            sb.append('\n');
            for (Use u : r.usesShown) {
                sb.append(String.format(Locale.ROOT, "  ADRP 0x%x → %s 0x%x  (기준 x%d)\n", u.adrpPc, u.kind, u.pc, u.reg));
            }
        }
    }
}
