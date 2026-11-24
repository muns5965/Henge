package dev.muns.hengeprobe;

import java.io.*;
import java.nio.file.Files;

public class G7ExitTraceTest {
    static int fails;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }
    public static void main(String[] a) throws Exception {
        File d = new File(Files.createTempDirectory("tr").toFile(), "g7exitinfo");
        check("kept", G7ExitTrace.keep(new ByteArrayInputStream(new byte[100]), d, "tombstone_12_345.pb") == 100);
        check("an existing file is left alone", G7ExitTrace.keep(new ByteArrayInputStream(new byte[5]), d, "tombstone_12_345.pb") == -1 && new File(d, "tombstone_12_345.pb").length() == 100);
        check("capped at 1 MiB", G7ExitTrace.keep(new ByteArrayInputStream(new byte[(2 << 20)]), d, "tombstone_1_2.pb") == (1 << 20));
        boolean bad = false; try { G7ExitTrace.keep(new ByteArrayInputStream(new byte[1]), d, "../x"); } catch (IOException e) { bad = true; }
        check("only the fixed name pattern", bad);
        System.out.println(fails == 0 ? "G7EXITTRACE OK" : "G7EXITTRACE FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
