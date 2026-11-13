package dev.muns.hengeprobe;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.util.Locale;

/**
 * Reads a small, checked slice of the boot ROM for analysis. Plain Java, no Android classes, so it has a JVM test.
 *
 * The ROM is mapped at guest address 0x100000 from the start of the file (devices/vmapple.c: VMAPPLE_FIRMWARE, hv_vmapple_load_rom),
 * so file offset = guest pc - 0x100000.
 *
 * What it guarantees:
 *   - the file must be exactly the expected size and SHA-256 (both are checked on the same open handle that the slice is then read from);
 *     any other state refuses, with its own status;
 *   - the request must be a 4-byte aligned pc inside the file, a length of 4..512 that is a multiple of 4, and must fit in the file;
 *   - at most 4096 bytes in all are returned over this object's life (the caller keeps one object for the process: it is a guard
 *     against mistakes, not a hard limit, the count restarts with the process); only a read that returned every requested byte counts;
 *   - the file is only ever opened "r"; the file's size is checked again after the read.
 * The expected size and SHA-256 are the values the app already uses to accept a ROM (MainActivity refers to the constants here);
 * a match means "the file the app expects", it does not prove the file is Apple's original.
 */
public final class RomSlice {
    public static final long EXPECTED_LENGTH = 214904L;
    public static final String EXPECTED_SHA256 = "fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91";
    public static final long BASE = 0x100000L;
    public static final int MAX_REQUEST = 512;
    public static final int MAX_TOTAL = 4096;
    public static final String DEFAULT_PC = "0x103F00";
    public static final String DEFAULT_LEN = "256";

    public enum Status {
        OK,
        BAD_PC_FORMAT, BAD_LEN_FORMAT, PC_BELOW_ROM, PC_UNALIGNED, LEN_RANGE, LEN_UNALIGNED, QUOTA,
        NO_FILE, NOT_A_FILE, OPEN_FAILED, SIZE_MISMATCH, HASH_FAILED, HASH_MISMATCH,
        OUT_OF_FILE, READ_FAILED, CHANGED
    }

    public static final class Result {
        public Status status = Status.OK;
        public long pc;
        public int length;
        public byte[] data;        // set only for OK
        public long fileSize = -1;
        public String sha256;      // set once the hash was computed
        public int totalAfter;
        public String message() { return RomSlice.message(status); }
    }

    private final long expectLength;
    private final String expectSha;
    private int total;

    /** The production instance: the size and SHA-256 the app accepts. */
    public static RomSlice forGenuineRom() { return new RomSlice(EXPECTED_LENGTH, EXPECTED_SHA256); }

    /** Tests inject their own expectation (a synthetic ROM and its hash). */
    public RomSlice(long expectLength, String expectSha256) {
        this.expectLength = expectLength;
        this.expectSha = expectSha256.toLowerCase(Locale.ROOT);
    }

    public synchronized int total() { return total; }

    /** pc is "0x103f00" or "103f00"; length is decimal ("256") or "0x100". Returns the slice or the reason it was refused. */
    public synchronized Result read(File f, String pcText, String lenText) {
        Result r = new Result();
        r.totalAfter = total;
        long pc = parseHex(pcText);
        if (pc < 0) return fail(r, Status.BAD_PC_FORMAT);
        long len = parseLength(lenText);
        if (len < 0) return fail(r, Status.BAD_LEN_FORMAT);
        r.pc = pc;
        r.length = len > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) len;
        if (pc < BASE) return fail(r, Status.PC_BELOW_ROM);
        if ((pc & 3) != 0) return fail(r, Status.PC_UNALIGNED);
        if (len < 4 || len > MAX_REQUEST) return fail(r, Status.LEN_RANGE);
        if ((len & 3) != 0) return fail(r, Status.LEN_UNALIGNED);
        if (len > MAX_TOTAL - total) return fail(r, Status.QUOTA);
        if (f == null || !f.exists()) return fail(r, Status.NO_FILE);
        if (!f.isFile()) return fail(r, Status.NOT_A_FILE);

        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {   // one handle: the check and the read are of the same file
            long size = raf.length();
            r.fileSize = size;
            if (size != expectLength) return fail(r, Status.SIZE_MISMATCH);
            String sha;
            try {
                sha = sha256(raf, size);
            } catch (Exception e) {
                return fail(r, Status.HASH_FAILED);
            }
            r.sha256 = sha;
            if (!sha.equals(expectSha)) return fail(r, Status.HASH_MISMATCH);
            long offset = pc - BASE;
            if (offset > size || len > size - offset) return fail(r, Status.OUT_OF_FILE);   // subtraction form: nothing can overflow
            byte[] buf = new byte[(int) len];
            try {
                raf.seek(offset);   // the hash read left the pointer at the end
                raf.readFully(buf);
            } catch (EOFException e) {
                return fail(r, Status.READ_FAILED);
            }
            if (raf.length() != size) return fail(r, Status.CHANGED);
            total += (int) len;    // only a complete read counts
            r.totalAfter = total;
            r.data = buf;
            return r;
        } catch (IOException e) {
            return fail(r, r.fileSize < 0 ? Status.OPEN_FAILED : Status.READ_FAILED);
        }
    }

    private static Result fail(Result r, Status s) {
        r.status = s;
        r.data = null;
        return r;
    }

    private static String sha256(RandomAccessFile raf, long size) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        raf.seek(0);
        byte[] buf = new byte[65536];
        long left = size;
        while (left > 0) {
            int n = raf.read(buf, 0, (int) Math.min(buf.length, left));
            if (n <= 0) throw new EOFException();
            md.update(buf, 0, n);
            left -= n;
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest()) sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }

    /** Hex with an optional 0x prefix, at most 12 digits (so it is a positive long). -1 if it is not that. */
    static long parseHex(String s) {
        if (s == null) return -1;
        s = s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
        if (s.isEmpty() || s.length() > 12) return -1;
        long v = 0;
        for (int i = 0; i < s.length(); i++) {
            int d = Character.digit(s.charAt(i), 16);
            if (d < 0 || s.charAt(i) > 'f') return -1;   // Character.digit also accepts non-ASCII digits
            v = v << 4 | d;
        }
        return v;
    }

    /** Decimal (at most 9 digits) or 0x-prefixed hex. -1 if it is not that. */
    static long parseLength(String s) {
        if (s == null) return -1;
        s = s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) return parseHex(s);
        if (s.isEmpty() || s.length() > 9) return -1;
        long v = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    }

    public static String message(Status s) {
        switch (s) {
            case OK: return "읽기 성공";
            case BAD_PC_FORMAT: return "PC 형식이 틀렸습니다 (16진수, 예: 0x103f00)";
            case BAD_LEN_FORMAT: return "길이 형식이 틀렸습니다 (10진수 또는 0x16진수)";
            case PC_BELOW_ROM: return "PC가 ROM 영역(0x100000 이상) 밖입니다";
            case PC_UNALIGNED: return "PC가 4바이트 정렬이 아닙니다";
            case LEN_RANGE: return "길이는 4~" + MAX_REQUEST + " 바이트여야 합니다";
            case LEN_UNALIGNED: return "길이는 4의 배수여야 합니다";
            case QUOTA: return "누적 읽기 한도(" + MAX_TOTAL + "바이트)를 넘습니다. 앱을 다시 시작하면 처음부터 셉니다";
            case NO_FILE: return "내부 ROM 파일이 없습니다";
            case NOT_A_FILE: return "내부 ROM 경로가 파일이 아닙니다";
            case OPEN_FAILED: return "ROM 파일을 열지 못했습니다";
            case SIZE_MISMATCH: return "ROM 파일 크기가 기대와 다릅니다 (읽지 않았습니다)";
            case HASH_FAILED: return "SHA-256 계산에 실패했습니다 (읽지 않았습니다)";
            case HASH_MISMATCH: return "SHA-256이 앱의 기대값과 다릅니다 (읽지 않았습니다)";
            case OUT_OF_FILE: return "요청 범위가 ROM 파일 크기를 넘습니다";
            case READ_FAILED: return "ROM을 끝까지 읽지 못했습니다 (누적량에 넣지 않았습니다)";
            case CHANGED: return "읽는 동안 파일 크기가 바뀌었습니다 (누적량에 넣지 않았습니다)";
            default: return s.toString();
        }
    }

    /** What the screen shows: the verdict, the request, and the bytes. No paths, nothing about the machine. */
    public static String screenText(Result r) {
        StringBuilder sb = new StringBuilder("ROM 구간 읽기 (읽기 전용, VM 실행 없음)\n");
        if (r.fileSize >= 0) sb.append("파일 크기: ").append(r.fileSize).append(" 바이트\n");
        if (r.sha256 != null) {
            sb.append("SHA-256: ").append(r.sha256).append('\n');
            sb.append(r.status == Status.HASH_MISMATCH ? "→ 앱의 기대값과 다릅니다\n"
                    : "→ 앱의 기대값과 같습니다 (Apple 원본임을 증명하는 것은 아닙니다)\n");
        }
        if (r.status != Status.OK) {
            sb.append("거부/실패: ").append(r.message()).append('\n');
            sb.append("누적 읽기: ").append(r.totalAfter).append(" / ").append(MAX_TOTAL).append(" 바이트\n");
            return sb.toString();
        }
        sb.append(String.format(Locale.ROOT, "요청: pc 0x%x 길이 %d → 파일 오프셋 0x%x\n", r.pc, r.length, r.pc - BASE));
        sb.append("누적 읽기: ").append(r.totalAfter).append(" / ").append(MAX_TOTAL).append(" 바이트 (앱을 다시 시작하면 처음부터)\n");
        sb.append(dump(r));
        return sb.toString();
    }

    /** What the copy button puts on the clipboard: the request and the bytes, nothing else. */
    public static String copyText(Result r) {
        if (r.status != Status.OK || r.data == null) return null;
        return String.format(Locale.ROOT, "ROM slice pc 0x%x len %d (file offset 0x%x)\n", r.pc, r.length, r.pc - BASE) + dump(r);
    }

    static String dump(Result r) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < r.data.length; i += 16) {
            sb.append(String.format(Locale.ROOT, "%x:", r.pc + i));
            for (int j = i; j < i + 16 && j < r.data.length; j++) sb.append(String.format(Locale.ROOT, " %02x", r.data[j] & 0xff));
            sb.append('\n');
        }
        return sb.toString();
    }
}
