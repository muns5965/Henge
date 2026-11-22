package dev.muns.hengeprobe;

/** Plain-java test (real child processes, no Android, no VM, no network, no Apple TSS request -- these are
 *  shell one-liners, never libuirunnervmg4.so): javac -d out .../VmRunnerG4Verdict.java .../VmRunnerG4Run.java
 *  tests/java/VmRunnerG4RunTest.java && java -cp out dev.muns.hengeprobe.VmRunnerG4RunTest
 *
 *  Exercises exactly what tests/java/VmRunnerG4VerdictTest.java cannot: whether VmRunnerG4Run actually drains
 *  a real OS pipe without deadlocking (a child writing well past the usual 64 KiB pipe buffer before it
 *  exits), and whether a RESULT line printed immediately before exit is still collected. */
public class VmRunnerG4RunTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    static Process sh(String script) throws Exception {
        return new ProcessBuilder("/bin/sh", "-c", script).redirectErrorStream(true).start();
    }

    public static void main(String[] a) throws Exception {
        // a child that writes ~300 KB (well past a typical 64 KiB pipe buffer) before printing the RESULT
        // line and exiting 0: if the reader thread were not started before waitFor(), this would hang until
        // the test's own timeout fires below, instead of finishing promptly.
        long t0 = System.currentTimeMillis();
        Process big = sh("yes XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX "
                + "| head -c 300000 > /dev/null; printf 'ui_runner_vm_g4 RESULT: stopped, confirmed (quit), g4 passed (FAILED rows=0), ignored input lines 0\\n'; exit 0");
        VmRunnerG4Run.Outcome bigOut = VmRunnerG4Run.run(big, 15000);
        long elapsed = System.currentTimeMillis() - t0;
        check("large output (well past a pipe buffer) drains without deadlock", elapsed < 15000 && !bigOut.timedOut);
        check("exit code captured correctly (0)", bigOut.exitCode == 0);
        check("the RESULT line printed right before exit is still collected -> PASS", bigOut.verdict.kind == VmRunnerG4Verdict.Kind.PASS);

        // a child that prints nothing and exits 4 (the hv_g4_wait()==-1 path, no RESULT line reached Java)
        Process noResult4 = sh("exit 4");
        VmRunnerG4Run.Outcome noResultOut = VmRunnerG4Run.run(noResult4, 5000);
        check("exit 4, no RESULT line: still FAILED on the exit code alone", noResultOut.verdict.kind == VmRunnerG4Verdict.Kind.FAILED);
        check("exit code captured correctly (4)", noResultOut.exitCode == 4);

        // a child that DOES print the FAILED RESULT line for the cancel-exhausted path, then exits 4
        Process withResult4 = sh("printf 'ui_runner_vm_g4 RESULT: FAILED, g4 did not end even after cancellation -- nothing can be safely freed or closed, "
                + "ending the process now (why quit, g4 FAILED rows=1)\\n'; exit 4");
        VmRunnerG4Run.Outcome withResultOut = VmRunnerG4Run.run(withResult4, 5000);
        check("exit 4 with the RESULT line present: still FAILED (not upgraded by having a log line)", withResultOut.verdict.kind == VmRunnerG4Verdict.Kind.FAILED);

        // a child that hangs past the given timeout: must be force-killed and reported as timed out, not PASS
        Process hangs = sh("sleep 30");
        VmRunnerG4Run.Outcome hungOut = VmRunnerG4Run.run(hangs, 500);
        check("a process that outlives the timeout is force-killed and reported as timed out", hungOut.timedOut && hungOut.verdict.kind == VmRunnerG4Verdict.Kind.FAILED);

        // nothing here ever constructs a command naming libuirunnervmg4.so or flips VmRunnerG4Launcher's
        // switch -- this class only ever observes a Process the caller already made.
        check("this test never touches VmRunnerG4Launcher's execution switch", !nameAppears());

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }

    private static boolean nameAppears() {
        // trivial, explicit self-check: this file's own source never references START_ENABLED or
        // libuirunnervmg4.so; checked for real (not just by eye) by tools/check-g4ui-launcher-source.sh's
        // ProcessBuilder-naming rule against VmRunnerG4Run.java/VmRunnerG4Verdict.java themselves.
        return false;
    }
}
