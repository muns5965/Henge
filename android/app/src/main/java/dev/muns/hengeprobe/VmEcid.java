package dev.muns.hengeprobe;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.SecureRandom;

/** The per-VM ECID: made once (random, 2^40 <= ecid < 2^62, like the iOS app), kept in dir/vm_ecid.txt as "ecid=0x<hex>" and reused.
 *  A file that exists but cannot be read back as a valid value is reported (CORRUPT) and never replaced. No Android classes here,
 *  so tests/java/VmEcidTest.java runs it with plain javac/java. */
final class VmEcid {
    enum Status { CREATED, LOADED, CORRUPT, ERROR }

    static final class Result {
        final Status status;
        final long value;
        final String detail;
        Result(Status s, long v, String d) { status = s; value = v; detail = d; }
    }

    static final String FILE = "vm_ecid.txt";
    static final long MIN = 1L << 40, LIMIT = 1L << 62;

    /** Test hook: replaces the final atomic rename. */
    interface Mover { void move(File from, File to) throws IOException; }
    static Mover mover = (from, to) -> Files.move(from.toPath(), to.toPath(), StandardCopyOption.ATOMIC_MOVE);

    static boolean inRange(long v) { return v >= MIN && v < LIMIT; }

    static Result loadOrCreate(File dir) {
        File f = new File(dir, FILE);
        if (f.exists()) {
            try {
                String t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
                if (t.startsWith("ecid=0x") && t.length() > 7 && t.length() <= 23) {
                    long v = Long.parseUnsignedLong(t.substring(7), 16);
                    if (inRange(v)) return new Result(Status.LOADED, v, FILE);
                }
                return new Result(Status.CORRUPT, 0, FILE + " 내용이 형식 또는 범위에 맞지 않습니다");
            } catch (NumberFormatException e) {
                return new Result(Status.CORRUPT, 0, FILE + " 값을 해석하지 못했습니다");
            } catch (IOException e) {
                return new Result(Status.ERROR, 0, FILE + " 읽기 실패: " + e);
            }
        }
        SecureRandom rnd = new SecureRandom();
        long v;
        do { v = rnd.nextLong() & (LIMIT - 1); } while (v < MIN);
        File tmp = new File(dir, FILE + ".tmp");
        try {
            try (FileOutputStream o = new FileOutputStream(tmp)) {
                o.write(String.format("ecid=0x%x\n", v).getBytes(StandardCharsets.UTF_8));
                o.getFD().sync();
            }
            mover.move(tmp, f);
            return new Result(Status.CREATED, v, FILE);
        } catch (IOException e) {
            tmp.delete();
            return new Result(Status.ERROR, 0, FILE + " 저장 실패: " + e);
        }
    }
}
