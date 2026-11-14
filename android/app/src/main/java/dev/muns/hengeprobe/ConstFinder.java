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
 * Finds where the boot ROM could produce a given 32-bit value: as raw data bytes, or as an adjacent MOVZ+MOVK instruction pair that
 * builds it. Plain Java, no Android classes, so it has a JVM test.
 *
 * Two independent, read-only searches over the app's own ROM copy (same file-checking discipline as RomBranchFinder/AddrRefFinder: size
 * checked before allocating, the whole file read into one buffer, SHA-256 taken over that same buffer, both searches run over that same
 * buffer). Neither search returns ROM bytes, so RomSlice's 4096-byte quota is not drawn on.
 *
 * Search 1 (raw data): every byte offset (not just 4-aligned; data is not necessarily instruction-aligned) whose next 4 bytes, read
 * little-endian, equal the target's low 32 bits. A hit here means the ROM HOLDS that value at that offset — it says nothing about
 * whether any code reads it, or whether the match is just a coincidence of unrelated bytes.
 *
 * Search 2 (instruction pair): every 4-byte aligned word i where word[i] is MOVZ Rd,#imm16{,lsl #(16*hw)} and word[i+1] is
 * MOVK Rd,#imm16{,lsl #(16*hw)} — same Rd, hw of each in {0,1}, and the two hw different from each other. This is the one shape the
 * search looks for: an immediately adjacent MOVZ-then-MOVK pair building one register. MOVZ always zeroes the whole register first, so
 * only its own immediate (at its hw) and MOVK's immediate (at its hw) can be set; with hw restricted to {0,1} the result is always in
 * 0..0xffffffff, which is why this model cannot reach a target above 32 bits (reported as a known limitation, not a silent zero).
 *
 * What a hit from either search is NOT:
 *   - a confirmed caller: the ROM can build the same value with MOVN, ORR, ADD, a memory load, a non-adjacent MOVZ/MOVK pair (something,
 *     even a NOP, between them), or a conditional branch that only ever executes one of the two instructions — none of that is looked for;
 *   - proof the code runs: data can accidentally carry the MOVZ/MOVK bit pattern, and a found pair may sit on a path that never executes.
 */
public final class ConstFinder {
    public static final int MAX_SHOWN = 32;
    public static final String DEFAULT_TARGET = "0x40200003";

    private static final int MOVZ_MASK = 0x7F800000, MOVZ_VAL = 0x52800000;
    private static final int MOVK_MASK = 0x7F800000, MOVK_VAL = 0x72800000;

    public enum Status {
        OK,
        BAD_TARGET_FORMAT,
        NO_FILE, NOT_A_FILE, OPEN_FAILED, SIZE_MISMATCH, READ_FAILED, CHANGED, HASH_FAILED, HASH_MISMATCH
    }

    /** One byte offset whose next 4 bytes equal the target's low 32 bits, little-endian. */
    public static final class Literal {
        public final long pc;
        Literal(long pc) { this.pc = pc; }
    }

    /** One (MOVZ, MOVK) pair on the same register that together build the target's low 32 bits. */
    public static final class Pair {
        public final long movzPc, movkPc;
        public final int reg;
        public final boolean isX;   // sf of the pair (both instructions agree); the built value does not depend on this
        Pair(long movzPc, long movkPc, int reg, boolean isX) {
            this.movzPc = movzPc;
            this.movkPc = movkPc;
            this.reg = reg;
            this.isX = isX;
        }
    }

    public static final class Result {
        public Status status = Status.OK;
        public long target;
        public boolean targetAbove32;   // needs more than 32 bits; search 2's model cannot reach it at all
        public long fileSize = -1;
        public String sha256;
        public int literalTotal;
        public final List<Literal> literalShown = new ArrayList<>();
        public int pairTotal;
        public final List<Pair> pairsShown = new ArrayList<>();
        public String message() { return ConstFinder.message(status); }
    }

    private final long expectLength;
    private final String expectSha;

    /** The production instance: the size and SHA-256 the app accepts (the same constants RomSlice uses). */
    public static ConstFinder forGenuineRom() { return new ConstFinder(RomSlice.EXPECTED_LENGTH, RomSlice.EXPECTED_SHA256); }

    /** Tests inject their own expectation (a synthetic ROM and its hash). */
    public ConstFinder(long expectLength, String expectSha256) {
        this.expectLength = expectLength;
        this.expectSha = expectSha256.toLowerCase(Locale.ROOT);
    }

    /** targetText is a 32-bit value, "0x40200003" or "40200003". Returns the hits, or the reason the search was refused. */
    public Result find(File f, String targetText) {
        Result r = new Result();
        long target = RomSlice.parseHex(targetText);
        if (target < 0) return fail(r, Status.BAD_TARGET_FORMAT);
        r.target = target;
        r.targetAbove32 = (target & ~0xFFFFFFFFL) != 0;
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
        scanLiteral(rom, RomSlice.BASE, target, r);
        scanPairs(rom, RomSlice.BASE, target, r);
        return r;
    }

    /** Every byte offset (not 4-aligned only) whose next 4 bytes, little-endian, equal target's low 32 bits. */
    static void scanLiteral(byte[] rom, long base, long target, Result r) {
        int want = (int) target;
        for (int off = 0; off + 4 <= rom.length; off++) {
            int w = (rom[off] & 0xff) | (rom[off + 1] & 0xff) << 8 | (rom[off + 2] & 0xff) << 16 | (rom[off + 3] & 0xff) << 24;
            if (w != want) continue;
            r.literalTotal++;
            if (r.literalShown.size() < MAX_SHOWN) r.literalShown.add(new Literal(base + off));
        }
    }

    /** Every 4-byte aligned i where word[i] is MOVZ Rd and word[i+1] is MOVK Rd, hw in {0,1} and differing, combining to target's low 32 bits. */
    static void scanPairs(byte[] rom, long base, long target, Result r) {
        int want = (int) target;
        int words = rom.length / 4;
        for (int i = 0; i + 1 < words; i++) {
            int a = word(rom, i * 4);
            if ((a & MOVZ_MASK) != MOVZ_VAL) continue;
            int b = word(rom, (i + 1) * 4);
            if ((b & MOVK_MASK) != MOVK_VAL) continue;
            int regA = a & 31, regB = b & 31;
            if (regA != regB) continue;
            int hwA = (a >>> 21) & 3, hwB = (b >>> 21) & 3;
            if (hwA > 1 || hwB > 1 || hwA == hwB) continue;   // only the low-32-bit shapes this model covers; see the class comment
            boolean sfA = ((a >>> 31) & 1) != 0, sfB = ((b >>> 31) & 1) != 0;
            if (sfA != sfB) continue;                          // a real pair keeps the same register width throughout
            int immA = (a >>> 5) & 0xFFFF, immB = (b >>> 5) & 0xFFFF;
            int value = ((immA << (hwA * 16)) & ~(0xFFFF << (hwB * 16))) | (immB << (hwB * 16));
            if (value != want) continue;
            r.pairTotal++;
            if (r.pairsShown.size() < MAX_SHOWN) r.pairsShown.add(new Pair(base + 4L * i, base + 4L * (i + 1), regA, sfA));
        }
    }

    static int word(byte[] rom, int off) {
        return (rom[off] & 0xff) | (rom[off + 1] & 0xff) << 8 | (rom[off + 2] & 0xff) << 16 | (rom[off + 3] & 0xff) << 24;
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
            case BAD_TARGET_FORMAT: return "대상 값 형식이 틀렸습니다 (16진수, 예: 0x40200003)";
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

    /** What the screen shows: the verdict, the target, both searches' hits and what neither search can say. */
    public static String screenText(Result r) {
        StringBuilder sb = new StringBuilder("상수 생성 검색 (원시 4바이트 + 인접 MOVZ→MOVK, 읽기 전용, VM 실행 없음)\n");
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
        sb.append("※ 검색 1(원시 바이트)의 결과는 '그 위치에 그 값이 데이터로 있다'는 뜻일 뿐이며, 코드가 그 값을 만든다는 증거가 아닙니다. 우연한 일치일 수 있습니다.\n");
        sb.append("※ 검색 2(명령어 쌍)는 서로 바로 붙어 있는 MOVZ→MOVK 한 쌍만 찾습니다. 둘 사이에 다른 명령(NOP 포함)이 있거나, MOVN·ORR·ADD·메모리 로드로 값을 만들거나, "
                + "조건 분기로 둘 중 하나만 실행되는 경우는 찾지 못합니다. 데이터가 우연히 같은 비트 패턴을 가질 수도 있어 '명령어 쌍 후보'일 뿐 실행 확정이 아닙니다.\n");
        if (r.targetAbove32) sb.append("※ 대상 값이 32비트를 넘어 이 명령어 쌍 모델로는 만들 수 없습니다 (검색 2는 항상 0건입니다). 검색 1만 유효합니다.\n");
        return sb.toString();
    }

    /** What the copy button puts on the clipboard: the target and the hits, nothing else. Null unless the search ran. */
    public static String copyText(Result r) {
        if (r.status != Status.OK) return null;
        StringBuilder sb = new StringBuilder();
        appendResult(sb, r);
        sb.append("(원시 바이트/명령어 쌍 후보. 실행 확정 아님)\n");
        return sb.toString();
    }

    private static void appendResult(StringBuilder sb, Result r) {
        sb.append(String.format(Locale.ROOT, "검색 대상: 0x%x\n", r.target));
        if (r.literalTotal == 0) {
            sb.append("검색 1 (원시 4바이트, 리틀엔디안): 0건\n");
        } else {
            sb.append("검색 1 (원시 4바이트, 리틀엔디안): 총 ").append(r.literalTotal).append("건");
            if (r.literalTotal > r.literalShown.size()) sb.append(" 중 ").append(r.literalShown.size()).append("건 표시");
            sb.append('\n');
            for (Literal l : r.literalShown) sb.append(String.format(Locale.ROOT, "  0x%x\n", l.pc));
        }
        if (r.pairTotal == 0) {
            sb.append("검색 2 (인접 MOVZ→MOVK 쌍): 0건\n");
        } else {
            sb.append("검색 2 (인접 MOVZ→MOVK 쌍): 총 ").append(r.pairTotal).append("건");
            if (r.pairTotal > r.pairsShown.size()) sb.append(" 중 ").append(r.pairsShown.size()).append("건 표시");
            sb.append('\n');
            for (Pair p : r.pairsShown) {
                sb.append(String.format(Locale.ROOT, "  0x%x → 0x%x  (%s%d)\n", p.movzPc, p.movkPc, p.isX ? "x" : "w", p.reg));
            }
        }
    }
}
