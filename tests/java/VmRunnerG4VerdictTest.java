package dev.muns.hengeprobe;

import dev.muns.hengeprobe.VmRunnerG4Verdict.Kind;

public class VmRunnerG4VerdictTest {
    static int fails;
    static final String PASS_LINE = "ui_runner_vm_g4 RESULT: stopped, confirmed (quit), g4 passed (FAILED rows=0), ignored input lines 0";
    static final String NOTCONF_LINE = "ui_runner_vm_g4 RESULT: STOP NOT CONFIRMED within the wait; ending the process anyway (quit), g4 passed (FAILED rows=0), ignored input lines 0";
    static final String G4FAIL_LINE = "ui_runner_vm_g4 RESULT: stopped, confirmed (quit), g4 FAILED (FAILED rows=2), ignored input lines 0";
    static final String EARLY_FAIL_LINE = "ui_runner_vm_g4 RESULT: FAILED, settings refused (read-only disk required)";
    static final String CANCEL4_LINE = "ui_runner_vm_g4 RESULT: FAILED, g4 did not end even after cancellation -- nothing can be safely freed or closed, ending the process now (why quit, g4 FAILED rows=1)";

    static void check(String name, Kind want, VmRunnerG4Verdict.Result r) {
        boolean ok = r.kind == want;
        System.out.println((ok ? "PASS " : "FAIL ") + name + " -> " + r.kind + " (" + r.text + ")");
        if (!ok) { fails++; System.out.println("  wanted " + want); }
    }
    static VmRunnerG4Verdict.Result ev(String out, int code) { return VmRunnerG4Verdict.evaluate(out, false, code, false, false); }

    public static void main(String[] a) {
        check("exit 0, RESULT says g4 passed", Kind.PASS, ev(PASS_LINE + "\n", 0));
        check("exit 0, RESULT missing entirely -- contract violation, never a pass", Kind.NO_VERDICT, ev("some other line\n", 0));
        check("exit 0, RESULT line says g4 FAILED -- mismatch with the exit code", Kind.FAILED, ev(G4FAIL_LINE + "\n", 0));
        check("exit 0, stop not confirmed in the RESULT line -- cannot happen per the native contract, still not a pass", Kind.FAILED, ev(NOTCONF_LINE + "\n", 0));
        check("exit 0, two RESULT lines (duplicate/conflicting)", Kind.FAILED, ev(PASS_LINE + "\n" + PASS_LINE + "\n", 0));
        check("exit 0, empty output", Kind.NO_VERDICT, ev("", 0));

        check("exit 1: settings/machine/start/G4 failure", Kind.FAILED, ev(EARLY_FAIL_LINE + "\n", 1));
        check("exit 2: bad arguments or shared file", Kind.FAILED, ev("", 2));
        check("exit 3: stop not confirmed within the wait", Kind.FAILED, ev(NOTCONF_LINE + "\n", 3));
        check("exit 4 with the FAILED RESULT line present: still FAILED (not upgraded or downgraded by the log)", Kind.FAILED, ev(CANCEL4_LINE + "\n", 4));
        check("exit 4 with NO RESULT line at all: still FAILED on the exit code alone", Kind.FAILED, ev("unrelated noise\n", 4));
        check("exit 4 even if, hypothetically, a PASS-shaped line were present: still FAILED", Kind.FAILED, ev(PASS_LINE + "\n", 4));
        check("killed by a signal (137)", Kind.FAILED, ev(PASS_LINE + "\n", 137));
        check("some other non-zero code (9)", Kind.FAILED, ev("", 9));

        check("time-out: forced kill, no verdict reached, always FAILED", Kind.FAILED, VmRunnerG4Verdict.evaluate(PASS_LINE, true, -1, false, false));
        check("output reader did not finish (exit 0): FAILED, not assumed to be fine", Kind.FAILED, VmRunnerG4Verdict.evaluate(PASS_LINE, false, 0, false, true));
        check("more RESULT lines than kept (exit 0): FAILED", Kind.FAILED, VmRunnerG4Verdict.evaluate(PASS_LINE, false, 0, true, false));
        // readerUnfinished/resultOverflow are not even consulted for a non-zero/timeout exit -- the exit code
        // alone already decided FAILED, exactly mirroring G0Verdict's own precedent.
        check("exit 4, readerUnfinished true: still just FAILED on the exit code, no crash", Kind.FAILED, VmRunnerG4Verdict.evaluate("", false, 4, false, true));

        check("CRLF line ending still parses as PASS", Kind.PASS, ev(PASS_LINE + "\r\n", 0));
        check("no trailing newline after the RESULT line still parses as PASS", Kind.PASS, ev(PASS_LINE, 0));
        check("a single space difference inside the RESULT line is not accepted", Kind.FAILED,
                ev(PASS_LINE.replace(", g4 passed (", ",  g4 passed (") + "\n", 0));

        boolean keep = VmRunnerG4Verdict.isKept(PASS_LINE) && VmRunnerG4Verdict.isKept(EARLY_FAIL_LINE) && VmRunnerG4Verdict.isKept(CANCEL4_LINE)
                && !VmRunnerG4Verdict.isKept("ui_runner_vm_g4: status line") && !VmRunnerG4Verdict.isKept(" " + PASS_LINE)
                && !VmRunnerG4Verdict.isKept("ui_runner_vm RESULT: unrelated binary");
        System.out.println((keep ? "PASS " : "FAIL ") + "the keep filter takes only ui_runner_vm_g4's own RESULT lines");
        if (!keep) fails++;

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
