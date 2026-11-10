package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Plain-java test of VmEcid (no framework): javac -d out android/.../VmEcid.java tests/java/VmEcidTest.java && java -cp out dev.muns.hengeprobe.VmEcidTest */
public class VmEcidTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }
    static File tmpdir() throws IOException { return Files.createTempDirectory("ecid").toFile(); }
    static void write(File d, String s) throws IOException { Files.write(new File(d, VmEcid.FILE).toPath(), s.getBytes(StandardCharsets.UTF_8)); }
    static String read(File d) throws IOException { return new String(Files.readAllBytes(new File(d, VmEcid.FILE).toPath()), StandardCharsets.UTF_8); }

    public static void main(String[] a) throws Exception {
        File d = tmpdir();
        VmEcid.Result r1 = VmEcid.loadOrCreate(d);
        check("first run creates", r1.status == VmEcid.Status.CREATED && VmEcid.inRange(r1.value));
        VmEcid.Result r2 = VmEcid.loadOrCreate(d);
        check("second run loads the same value", r2.status == VmEcid.Status.LOADED && r2.value == r1.value);
        check("no temp file left", !new File(d, VmEcid.FILE + ".tmp").exists());
        check("file format is ecid=0x<hex>", read(d).equals(String.format("ecid=0x%x\n", r1.value)));

        // a file in the format build 38 wrote
        File d2 = tmpdir(); write(d2, "ecid=0x2d2a8d94f32ba4d2\n");
        VmEcid.Result r3 = VmEcid.loadOrCreate(d2);
        check("build-38 file is read unchanged", r3.status == VmEcid.Status.LOADED && r3.value == 0x2d2a8d94f32ba4d2L);

        String[] bad = {"", "garbage", "ecid=0x", "ecid=0xzz", "ecid=0x0", "ecid=0xffffff", "ecid=0x4000000000000000", "ecid=0xffffffffffffffff",
                        "ecid=0x12345678901234567890", "ecid=2d2a8d94f32ba4d2"};
        for (String b : bad) {
            File d3 = tmpdir(); write(d3, b);
            VmEcid.Result r = VmEcid.loadOrCreate(d3);
            check("corrupt/out-of-range '" + b + "' is reported, not replaced", r.status == VmEcid.Status.CORRUPT && read(d3).equals(b));
        }

        // random values stay in range
        boolean allIn = true;
        for (int i = 0; i < 200; i++) { VmEcid.Result r = VmEcid.loadOrCreate(tmpdir()); allIn &= r.status == VmEcid.Status.CREATED && VmEcid.inRange(r.value); }
        check("200 fresh values all in [2^40, 2^62)", allIn);

        // the atomic rename fails: no ecid file appears and the temp file is removed
        File d4 = tmpdir();
        VmEcid.Mover saved = VmEcid.mover;
        VmEcid.mover = (from, to) -> { throw new IOException("injected"); };
        VmEcid.Result r4 = VmEcid.loadOrCreate(d4);
        VmEcid.mover = saved;
        check("failed save is reported", r4.status == VmEcid.Status.ERROR);
        check("failed save leaves no ecid file and no temp file", !new File(d4, VmEcid.FILE).exists() && !new File(d4, VmEcid.FILE + ".tmp").exists());
        VmEcid.Result r5 = VmEcid.loadOrCreate(d4);
        check("after a failed save the next run creates normally", r5.status == VmEcid.Status.CREATED);

        // a stale temp file from a crash does not block or leak into the result
        File d5 = tmpdir(); Files.write(new File(d5, VmEcid.FILE + ".tmp").toPath(), "stale".getBytes());
        VmEcid.Result r6 = VmEcid.loadOrCreate(d5);
        check("stale temp file is overwritten", r6.status == VmEcid.Status.CREATED && read(d5).startsWith("ecid=0x") && !new File(d5, VmEcid.FILE + ".tmp").exists());

        // existing good file survives a failing mover (the mover is never reached)
        VmEcid.mover = (from, to) -> { throw new IOException("injected"); };
        VmEcid.Result r7 = VmEcid.loadOrCreate(d);
        VmEcid.mover = saved;
        check("existing good file is untouched and reused", r7.status == VmEcid.Status.LOADED && r7.value == r1.value && read(d).equals(String.format("ecid=0x%x\n", r1.value)));

        System.out.println(fails == 0 ? "ALL PASS" : "FAILED: " + fails);
        System.exit(fails == 0 ? 0 : 1);
    }
}
