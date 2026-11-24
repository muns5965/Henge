package dev.muns.hengeprobe;

import java.io.*;
import java.nio.file.Files;

public class G7ExitTraceTest {
    static int fails;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }
    static InputStream bytes(int n) { return new ByteArrayInputStream(new byte[n]); }
    public static void main(String[] a) throws Exception {
        File d = new File(Files.createTempDirectory("tr").toFile(), "g7exitinfo");
        G7ExitTrace.Result r = G7ExitTrace.keep(bytes(100), d, "tombstone_12_345.pb");
        check("kept whole", r.status == G7ExitTrace.Status.KEPT && r.bytes == 100);
        r = G7ExitTrace.keep(bytes(5), d, "tombstone_12_345.pb");
        check("a repeated look does not store it again; the file is untouched", r.status == G7ExitTrace.Status.ALREADY_THERE && new File(d, "tombstone_12_345.pb").length() == 100);
        r = G7ExitTrace.keep(bytes(2 << 20), d, "tombstone_1_2.pb");
        check("over 1 MiB: kept cut off and marked truncated", r.status == G7ExitTrace.Status.KEPT_TRUNCATED && r.bytes == (1 << 20));
        r = G7ExitTrace.keep(bytes(1 << 20), d, "tombstone_3_4.pb");
        check("exactly 1 MiB is not truncated", r.status == G7ExitTrace.Status.KEPT && r.bytes == (1 << 20));
        r = G7ExitTrace.keep(bytes(3 << 20), d, "tombstone_5_6.pb");
        check("a third file: still 1 MiB, marked truncated", r.status == G7ExitTrace.Status.KEPT_TRUNCATED && r.bytes == (1 << 20));
        r = G7ExitTrace.keep(bytes(3 << 20), d, "tombstone_7_8.pb");
        check("the total cap (4 MiB) trims the last file to what is left and marks it", r.status == G7ExitTrace.Status.KEPT_TRUNCATED && r.bytes == (4L << 20) - (100 + 3L * (1 << 20)));
        r = G7ExitTrace.keep(bytes(10), d, "tombstone_9_10.pb");
        check("total reached: nothing is written", r.status == G7ExitTrace.Status.LIMIT_REACHED && !new File(d, "tombstone_9_10.pb").exists());
        File d2 = new File(Files.createTempDirectory("tr2").toFile(), "x");
        for (int i = 0; i < G7ExitTrace.MAX_FILES; i++) check("file " + i + " kept", G7ExitTrace.keep(bytes(1), d2, "tombstone_" + i + "_1.pb").status == G7ExitTrace.Status.KEPT);
        check("the file count cap", G7ExitTrace.keep(bytes(1), d2, "tombstone_99_1.pb").status == G7ExitTrace.Status.LIMIT_REACHED);
        boolean bad = false; try { G7ExitTrace.keep(bytes(1), d, "../x"); } catch (IOException e) { bad = true; }
        check("only the fixed name pattern", bad);
        System.out.println(fails == 0 ? "G7EXITTRACE OK" : "G7EXITTRACE FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
