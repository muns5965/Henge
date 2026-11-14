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
 * Finds the places in the boot ROM that branch directly to a given address (AArch64 "BL imm26" and "B imm26"). Plain Java, no Android
 * classes, so it has a JVM test.
 *
 * What a hit is: a 4-byte aligned word whose bits are a B or BL with the target the user asked for. It is a candidate, not a proven caller:
 * data that happens to have the same bit pattern matches too, and a function can also be reached by BLR (register), by a conditional branch
 * or through a table, none of which this looks for. A search with no hit therefore does not mean "nobody calls it".
 *
 * What it does and does not do:
 *   - The ROM is the app's own copy, opened "r" only. Its size is checked before anything is read (so no more than the expected size is ever
 *     allocated), the whole file is read into one buffer (a short read is a failure), the SHA-256 is taken over that same buffer and must match,
 *     and the scan runs over that same buffer: what was checked is what was searched.
 *   - The whole ROM is searched, hits are counted in full, and at most {@link #MAX_SHOWN} addresses of each kind are kept for display.
 *   - Only addresses are reported. No ROM bytes are returned or shown, so this does not draw on RomSlice's 4096-byte limit and leaves it alone.
 * The ROM is mapped at guest address 0x100000 from the start of the file (RomSlice.BASE), so the guest pc of word i is BASE + 4*i.
 */
public final class RomBranchFinder {
    public static final int MAX_SHOWN = 32;
    public static final String DEFAULT_TARGET = "0x103DC4";
    private static final int OPCODE_MASK = 0xFC000000;
    private static final int OP_B = 0x14000000;
    private static final int OP_BL = 0x94000000;

    public enum Status {
        OK,
        BAD_TARGET_FORMAT, TARGET_BELOW_ROM, TARGET_UNALIGNED, TARGET_OUT_OF_ROM,
        NO_FILE, NOT_A_FILE, OPEN_FAILED, SIZE_MISMATCH, READ_FAILED, CHANGED, HASH_FAILED, HASH_MISMATCH
    }

    public static final class Result {
        public Status status = Status.OK;
        public long target;
        public long fileSize = -1;
        public String sha256;                           // set once the hash was computed
        public int blTotal, bTotal;                     // every hit in the ROM, shown or not
        public final List<Long> blShown = new ArrayList<>();   // guest pcs, at most MAX_SHOWN, in address order
        public final List<Long> bShown = new ArrayList<>();
        public String message() { return RomBranchFinder.message(status); }
    }

    private final long expectLength;
    private final String expectSha;

    /** The production instance: the size and SHA-256 the app accepts (the same constants RomSlice uses). */
    public static RomBranchFinder forGenuineRom() { return new RomBranchFinder(RomSlice.EXPECTED_LENGTH, RomSlice.EXPECTED_SHA256); }

    /** Tests inject their own expectation (a synthetic ROM and its hash). */
    public RomBranchFinder(long expectLength, String expectSha256) {
        this.expectLength = expectLength;
        this.expectSha = expectSha256.toLowerCase(Locale.ROOT);
    }

    /** targetText is a guest address, "0x103dc4" or "103dc4". Returns the hits, or the reason the search was refused. */
    public Result find(File f, String targetText) {
        Result r = new Result();
        long target = RomSlice.parseHex(targetText);
        if (target < 0) return fail(r, Status.BAD_TARGET_FORMAT);
        r.target = target;
        if (target < RomSlice.BASE) return fail(r, Status.TARGET_BELOW_ROM);
        if ((target & 3) != 0) return fail(r, Status.TARGET_UNALIGNED);
        if (target - RomSlice.BASE >= expectLength) return fail(r, Status.TARGET_OUT_OF_ROM);
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

    /** Walks every 4-byte aligned word of rom from its first byte (little endian) and records the B/BL words whose target is target. */
    static void scan(byte[] rom, long base, long target, Result r) {
        for (int off = 0; off + 4 <= rom.length; off += 4) {
            int w = (rom[off] & 0xff) | (rom[off + 1] & 0xff) << 8 | (rom[off + 2] & 0xff) << 16 | (rom[off + 3] & 0xff) << 24;
            int op = w & OPCODE_MASK;
            if (op != OP_BL && op != OP_B) continue;
            long pc = base + off;
            long dest = pc + ((w << 6) >> 4);      // imm26 sign-extended and times 4; w must stay a 32-bit int here
            if (dest != target) continue;
            if (op == OP_BL) {
                r.blTotal++;
                if (r.blShown.size() < MAX_SHOWN) r.blShown.add(pc);
            } else {
                r.bTotal++;
                if (r.bShown.size() < MAX_SHOWN) r.bShown.add(pc);
            }
        }
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
            case BAD_TARGET_FORMAT: return "대상 주소 형식이 틀렸습니다 (16진수, 예: 0x103dc4)";
            case TARGET_BELOW_ROM: return "대상 주소가 ROM 영역(0x100000 이상) 밖입니다";
            case TARGET_UNALIGNED: return "대상 주소가 4바이트 정렬이 아닙니다";
            case TARGET_OUT_OF_ROM: return "대상 주소가 ROM 파일 크기를 넘습니다";
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

    /** What the screen shows: the verdict, the target, the hits (addresses only) and what the search cannot say. */
    public static String screenText(Result r) {
        StringBuilder sb = new StringBuilder("직접 분기 참조 검색 (읽기 전용, VM 실행 없음)\n");
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
        sb.append("※ 호출자 확정이 아닌 후보입니다: 데이터가 우연히 같은 비트 패턴일 수 있습니다.\n");
        sb.append("※ 간접 호출(BLR), 조건 분기, 표를 통한 분기는 검색 대상이 아닙니다. 0건이어도 호출자가 없다는 뜻이 아닙니다.\n");
        return sb.toString();
    }

    /** What the copy button puts on the clipboard: the target and the hit addresses, nothing else. Null unless the search ran. */
    public static String copyText(Result r) {
        if (r.status != Status.OK) return null;
        StringBuilder sb = new StringBuilder();
        appendResult(sb, r);
        sb.append("(직접 B/BL 참조 후보. 호출자 확정 아님)\n");
        return sb.toString();
    }

    private static void appendResult(StringBuilder sb, Result r) {
        sb.append(String.format(Locale.ROOT, "검색 대상: 0x%x\n", r.target));
        appendKind(sb, "BL 직접 참조", r.blTotal, r.blShown);
        appendKind(sb, "B 직접 참조", r.bTotal, r.bShown);
    }

    private static void appendKind(StringBuilder sb, String name, int total, List<Long> shown) {
        if (total == 0) {
            sb.append(name).append(": 0건\n");
            return;
        }
        sb.append(name).append(": 총 ").append(total).append("건");
        if (total > shown.size()) sb.append(" 중 ").append(shown.size()).append("건 표시");
        sb.append('\n');
        for (long pc : shown) sb.append(String.format(Locale.ROOT, "  0x%x\n", pc));
    }
}
