package dev.muns.hengeprobe;

/** Plain-java test: javac -d out android/.../NativeStart.java tests/java/NativeStartTest.java && java -cp out dev.muns.hengeprobe.NativeStartTest
 *
 *  Covers the exact decision both DisplayTestActivity and VmDisplayActivity make from ui_jni.c's
 *  impl_nativeStart() return value, after the fix for the bug found via review: r == 1 ("already running",
 *  owned by the OTHER screen) was previously treated as success because only `r < 0` was checked. */
public class NativeStartTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    public static void main(String[] a) {
        check("r == 0 is the only success", NativeStart.succeeded(0));
        check("r == 1 (already running elsewhere) is a failure for this caller", !NativeStart.succeeded(1));
        check("r == -1 (build failed) is a failure", !NativeStart.succeeded(-1));
        check("r == -2 (previous instance still closing) is a failure", !NativeStart.succeeded(-2));
        check("some other unrecognised negative value is still a failure", !NativeStart.succeeded(-99));

        check("r == 1's reason names the other screen, not a build failure", NativeStart.refusalReason(1).contains("another screen"));
        check("r == -2's reason mentions the previous screen still closing", NativeStart.refusalReason(-2).contains("still being closed"));
        check("r == -1 (plain build failure) gets no extra reason text", NativeStart.refusalReason(-1).isEmpty());
        check("r == 1's reason does not say 'closing' (that would misdescribe it as the -2 case)", !NativeStart.refusalReason(1).contains("closing"));
        check("r == -2's reason does not say 'another screen' (that would misdescribe it as the 1 case)", !NativeStart.refusalReason(-2).contains("another screen"));

        if (fails > 0) { System.out.println(fails + " FAILED"); System.exit(1); }
        System.out.println("all passed");
    }
}
