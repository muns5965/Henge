package dev.muns.hengeprobe;

import dev.muns.hengeprobe.G0Verdict.Kind;

public class G0VerdictTest {
    static int fails;
    static final String PASS = "install g0 RESULT: g0_passed stopped=before_rootfs (observed: hook called, tool returned 217), session report 2, disk and aux content verified, 0 failed row(s). This is NOT a restore.";
    static final String META = "install g0 RESULT: g0_passed_metadata_only stopped=before_rootfs (observed), session report 2, 0 failed row(s), but the disk's CONTENT was not verified. Not a full pass.";
    static final String FAIL = "install g0 RESULT: FAILED (2 row(s)). Not a restore.";
    static final String VM_OK = "vmrun RESULT: install g0 passed; machine stopped";
    static final String VM_NOSTOP = "vmrun RESULT: install g0 passed; machine did not confirm stopping within 5 s";

    static void check(String name, Kind want, G0Verdict.Result r) {
        boolean ok = r.kind == want;
        System.out.println((ok ? "PASS " : "FAIL ") + name + " -> " + r.kind + " (" + r.text + ")");
        if (!ok) { fails++; System.out.println("  wanted " + want); }
    }
    static G0Verdict.Result ev(String out, int code) { return G0Verdict.evaluate(out, false, code, false, false); }

    public static void main(String[] a) {
        check("full pass", Kind.PASS, ev(PASS + "\n" + VM_OK + "\n", 0));
        check("metadata-only pass is its own kind, never PASS", Kind.PARTIAL_PASS, ev(META + "\n" + VM_OK + "\n", 0));
        check("g0_passed with exit code 3 (the known counter-example)", Kind.FAILED, ev(PASS + "\n" + VM_NOSTOP + "\n", 3));
        check("exit code 0 but the final line says the machine did not confirm stopping", Kind.FAILED, ev(PASS + "\n" + VM_NOSTOP + "\n", 0));
        check("final line passed but machine not stopped, only in a diagnostic line elsewhere", Kind.FAILED, ev(PASS + "\nnote: machine stopped\n" + VM_NOSTOP + "\n", 0));
        check("'machine stopped' only inside another line, final line missing", Kind.NO_VERDICT, ev(PASS + "\nsomething machine stopped\n", 0));
        check("exit code 1", Kind.FAILED, ev(FAIL + "\nvmrun RESULT: install g0 FAILED; machine stopped\n", 1));
        check("exit code 4 (thread did not end)", Kind.FAILED, ev(FAIL + "\n", 4));
        check("killed by a signal (137)", Kind.FAILED, ev(PASS + "\n" + VM_OK + "\n", 137));
        check("any other non-zero code", Kind.FAILED, ev(PASS + "\n" + VM_OK + "\n", 2));
        check("no G0 result line", Kind.NO_VERDICT, ev("some output\n" + VM_OK + "\n", 0));
        check("empty output", Kind.NO_VERDICT, ev("", 0));
        check("two identical G0 result lines", Kind.FAILED, ev(PASS + "\n" + PASS + "\n" + VM_OK + "\n", 0));
        check("two conflicting G0 result lines", Kind.FAILED, ev(PASS + "\n" + FAIL + "\n" + VM_OK + "\n", 0));
        check("two final vmrun lines", Kind.FAILED, ev(PASS + "\n" + VM_OK + "\n" + VM_OK + "\n", 0));
        check("G0 says FAILED, exit 0", Kind.FAILED, ev(FAIL + "\n" + VM_OK + "\n", 0));
        check("unknown token", Kind.FAILED, ev("install g0 RESULT: g0_passed_maybe x\n" + VM_OK + "\n", 0));
        check("g0_passed_metadata_only is not matched as g0_passed by prefix", Kind.PARTIAL_PASS, ev("install g0 RESULT: g0_passed_metadata_only\n" + VM_OK, 0));
        check("CRLF line ends", Kind.PASS, ev(PASS + "\r\n" + VM_OK + "\r\n", 0));
        check("no newline after the last line", Kind.PASS, ev(PASS + "\n" + VM_OK, 0));
        check("a space changed inside the final line is not accepted", Kind.FAILED, ev(PASS + "\nvmrun RESULT: install g0 passed;  machine stopped\n", 0));
        check("time-out", Kind.FAILED, G0Verdict.evaluate(PASS + "\n" + VM_OK, true, -1, false, false));
        check("more result lines than the app keeps", Kind.FAILED, G0Verdict.evaluate(PASS + "\n" + VM_OK, false, 0, true, false));
        check("output reader not finished", Kind.FAILED, G0Verdict.evaluate(PASS + "\n" + VM_OK, false, 0, false, true));
        check("final vmrun line from an early failure", Kind.FAILED, ev(PASS + "\nvmrun RESULT: install g0 FAILED before the machine was created\n", 0));
        // the keep-filter the app applies while reading
        boolean keep = G0Verdict.isKept(PASS) && G0Verdict.isKept(VM_OK) && G0Verdict.isKept("vmrun RESULT: install g0 FAILED: the restore thread did not end")
                && !G0Verdict.isKept("vmrun RESULT: stopped by the guest") && !G0Verdict.isKept("install g0: stop_a_hook ok") && !G0Verdict.isKept(" " + PASS);
        System.out.println((keep ? "PASS " : "FAIL ") + "the keep filter takes the result lines and nothing else");
        if (!keep) fails++;
        System.out.println(fails == 0 ? "G0 VERDICT OK" : "G0 VERDICT FAILED (" + fails + ")");
        System.exit(fails == 0 ? 0 : 1);
    }
}
