package dev.muns.hengeprobe;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The clone runner's final VERDICT line, parsed once (pure Java, no Android). Older runners print only the first four fields; the fields added later are optional, and a
 * line that does not match is not interpreted at all (parse() returns null), never guessed.
 *
 * outcome says what the app may do next: only "restart" (a confirmed guest PSCI SYSTEM_RESET) can lead to a restart. Everything else (shutdown, a host stop, a stop that was not
 * confirmed, a halt without a PSCI record) is not a restart.
 */
public final class G7RunVerdict {
    public static final String RESTART = "restart", SHUTDOWN = "shutdown", HALT_OTHER = "halt-other", HOST_STOP = "host-stop", NOT_CONFIRMED = "not-confirmed", ENGINE_FATAL = "engine-fatal";

    private static final Pattern P = Pattern.compile("^ui_runner_vm_clone VERDICT: vm_stop_confirmed=([01]) stop_kind=([a-z-]{1,24}) disk_flush=(ok|FAILED|not-attempted) macos_boot=not-determined"
            + "(?: outcome=(restart|shutdown|halt-other|host-stop|not-confirmed|engine-fatal) psci=(none|SYSTEM_OFF|SYSTEM_RESET) quiesced=(1|0|n/a) uptime_s=(\\d{1,9}) disk_writes=(\\d{1,18}) disk_write_refused=(\\d{1,18})"
            // NEW optional trailing fields (a runner that prints them: root and AUX counted separately). An older runner stops after disk_write_refused=; the whole group is optional so such a line parses exactly as before.
            + "(?: root_writes=(\\d{1,18}) root_write_bytes=(\\d{1,18}) root_write_refused=(\\d{1,18}) root_write_failed=(\\d{1,18}) root_flush=(ok|FAILED|skipped|never) root_flush_calls=(\\d{1,18}) root_flush_failed=(\\d{1,18})"
            + " aux_writes=(\\d{1,18}) aux_write_bytes=(\\d{1,18}) aux_write_refused=(\\d{1,18}) aux_write_failed=(\\d{1,18}) aux_flush=(ok|FAILED|skipped|never) aux_flush_calls=(\\d{1,18}) aux_flush_failed=(\\d{1,18}))?)?.*$");

    public final boolean stopConfirmed;
    public final String stopKind, diskFlush;
    /** null when the runner is an older one that did not print the structured fields. */
    public final String outcome, psci;
    public final int quiesced;          // 1 yes, 0 no, -1 not reported / not run
    public final long uptimeS, diskWrites, diskWriteRefused;   // -1 when not reported
    public final boolean structured;
    /** Host-side counts for one disk (the runner's own counters; NOT a statement about what the guest's boot device kept). */
    public static final class DiskSide {
        public final long writes, writeBytes, refused, failed, flushCalls, flushFailed;
        public final String flush;   // ok | FAILED | skipped (nothing synced) | never
        DiskSide(long w, long b, long r, long f, String fl, long fc, long ff) { writes = w; writeBytes = b; refused = r; failed = f; flush = fl; flushCalls = fc; flushFailed = ff; }
    }
    /** null when the runner is older than the split counters (then only diskWrites / diskWriteRefused, the sums, are known). */
    public final DiskSide root, aux;

    private G7RunVerdict(boolean sc, String kind, String flush, String outcome, String psci, int q, long up, long w, long wr, DiskSide root, DiskSide aux) {
        stopConfirmed = sc; stopKind = kind; diskFlush = flush; this.outcome = outcome; this.psci = psci; quiesced = q; uptimeS = up; diskWrites = w; diskWriteRefused = wr;
        structured = outcome != null; this.root = root; this.aux = aux;
    }

    private static DiskSide side(Matcher m, int g) {
        if (m.group(g) == null) return null;
        return new DiskSide(Long.parseLong(m.group(g)), Long.parseLong(m.group(g + 1)), Long.parseLong(m.group(g + 2)), Long.parseLong(m.group(g + 3)), m.group(g + 4), Long.parseLong(m.group(g + 5)), Long.parseLong(m.group(g + 6)));
    }

    /** One line for the app's log; says what the counts are and are not. Empty when the runner did not report them. */
    public String diskSplitText() {
        if (root == null || aux == null) return "";
        return "disk counters (host side) root: writes " + root.writes + ", bytes " + root.writeBytes + ", refused " + root.refused + ", failed " + root.failed + ", flush " + root.flush
            + " | AUX: writes " + aux.writes + ", bytes " + aux.writeBytes + ", refused " + aux.refused + ", failed " + aux.failed + ", flush " + aux.flush
            + " (counts of what the runner did with the files; they do not show that the guest's boot device persisted anything)";
    }

    public static G7RunVerdict parse(String line) {
        if (line == null) return null;
        Matcher m = P.matcher(line);
        if (!m.matches()) return null;
        boolean s = m.group(4) != null;
        int q = !s ? -1 : m.group(6).equals("1") ? 1 : m.group(6).equals("0") ? 0 : -1;
        return new G7RunVerdict(m.group(1).equals("1"), m.group(2), m.group(3), s ? m.group(4) : null, s ? m.group(5) : null, q,
                s ? Long.parseLong(m.group(7)) : -1, s ? Long.parseLong(m.group(8)) : -1, s ? Long.parseLong(m.group(9)) : -1,
                s ? side(m, 10) : null, s ? side(m, 17) : null);
    }

    /** For READING a line (old logs included): the stop is confirmed, the flush succeeded and nothing says the threads/workers were left running (an older runner that did not report it passes). */
    public boolean cleanStop() {
        return stopConfirmed && diskFlush.equals("ok") && quiesced != 0;
    }

    /** For ACTING on a line (a restart): the runner SAID every CPU thread, the kicker and the disk workers were finished before the flush. Not reported (-1) is not enough. */
    public boolean verifiedQuiesced() {
        return stopConfirmed && diskFlush.equals("ok") && quiesced == 1;
    }

    public boolean isRestart() { return RESTART.equals(outcome); }
}
