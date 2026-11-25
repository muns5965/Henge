package dev.muns.hengeprobe;

public class G7RestartPolicyTest {
    static int fail;
    static void check(boolean ok, String what) { if (!ok) { fail++; System.out.println("FAIL: " + what); } }

    static String line(int confirmed, String kind, String flush, String outcome, String psci, String q, int up, int refused) {
        return "ui_runner_vm_clone VERDICT: vm_stop_confirmed=" + confirmed + " stop_kind=" + kind + " disk_flush=" + flush + " macos_boot=not-determined outcome=" + outcome + " psci=" + psci
                + " quiesced=" + q + " uptime_s=" + up + " disk_writes=12 disk_write_refused=" + refused + " (text)";
    }
    static final String RESET = line(1, "guest-initiated", "ok", "restart", "SYSTEM_RESET", "1", 3, 0);

    public static void main(String[] a) {
        // parsing: the new line, the old runner's line, junk
        G7RunVerdict v = G7RunVerdict.parse(RESET);
        check(v != null && v.structured && v.isRestart() && v.cleanStop() && v.uptimeS == 3 && v.psci.equals("SYSTEM_RESET") && v.diskWrites == 12, "new line parsed");
        G7RunVerdict old = G7RunVerdict.parse("ui_runner_vm_clone VERDICT: vm_stop_confirmed=1 stop_kind=guest-initiated disk_flush=ok macos_boot=not-determined (a confirmed stop says only ...)");
        check(old != null && !old.structured && old.outcome == null && !old.isRestart() && old.stopConfirmed, "old runner's line parsed without outcome");
        check(G7RunVerdict.parse("garbage") == null && G7RunVerdict.parse(null) == null && G7RunVerdict.parse(RESET.replace("outcome=restart", "outcome=bogus")) != null
                && !G7RunVerdict.parse(RESET.replace("outcome=restart", "outcome=bogus")).structured, "junk is not interpreted (an unknown outcome is not structured)");
        check(!G7RunVerdict.parse(line(1, "guest-initiated", "ok", "restart", "SYSTEM_RESET", "0", 3, 0)).cleanStop(), "unquiesced is not a clean stop");

        long t = 1_000_000;
        G7RestartPolicy p = new G7RestartPolicy();
        G7RestartPolicy.Decision d = p.decide(v, false, t);
        check(d.restart && d.delayMs == 2000, "first reset restarts after 2 s even with 3 s uptime and no picture (no minimum uptime)");
        d = p.decide(v, false, t + 10_000); check(d.restart && d.delayMs == 8000, "second: 8 s");
        d = p.decide(v, false, t + 20_000); check(d.restart && d.delayMs == 30000, "third: 30 s");
        d = p.decide(v, false, t + 30_000); check(!d.restart && d.reason.contains("반복 방지"), "fourth within 10 min: loop limit");
        d = p.decide(v, false, t + 11 * 60_000); check(d.restart && d.delayMs == 2000, "after the window every old restart has expired: allowed again, from the first wait");
        p = new G7RestartPolicy();
        check(p.decide(v, false, t).restart, "fresh policy restarts");
        check(!p.decide(v, true, t + 1).restart, "cancelled by the person: no restart");
        check(!p.decide(null, false, t + 1).restart, "no verdict: no restart");
        check(!p.decide(old, false, t + 1).restart, "older runner without outcome: no restart");
        String[][] no = {{"shutdown", "SYSTEM_OFF"}, {"halt-other", "none"}, {"host-stop", "none"}, {"not-confirmed", "none"}};
        for (String[] n : no) check(!p.decide(G7RunVerdict.parse(line(1, "guest-initiated", "ok", n[0], n[1], "1", 50, 0)), false, t + 2).restart, n[0] + " never restarts");
        check(!p.decide(G7RunVerdict.parse(line(0, "guest-initiated", "not-attempted", "restart", "SYSTEM_RESET", "n/a", 50, 0)), false, t + 3).restart, "unconfirmed stop: no restart");
        check(!p.decide(G7RunVerdict.parse(line(1, "guest-initiated", "FAILED", "restart", "SYSTEM_RESET", "1", 50, 0)), false, t + 4).restart, "flush failed: no restart");
        check(!p.decide(G7RunVerdict.parse(line(1, "guest-initiated", "ok", "restart", "SYSTEM_RESET", "0", 50, 0)), false, t + 5).restart, "threads/workers not finished: no restart");
        // reset() by a person's own start; a restart only counts when allowed
        G7RestartPolicy q = new G7RestartPolicy(1, 60_000, new long[] {1000});
        check(q.decide(v, false, 0).restart && !q.decide(v, false, 1000).restart, "limit 1");
        q.reset(); check(q.decide(v, false, 2000).restart, "reset clears the count");
        check(q.decide(v, false, 70_000).restart, "window expired: allowed again");
        System.out.println(fail == 0 ? "G7RestartPolicyTest OK" : fail + " FAILED");
        if (fail != 0) System.exit(1);
    }
}
