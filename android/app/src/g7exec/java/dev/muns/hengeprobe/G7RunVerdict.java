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
    public static final String RESTART = "restart", SHUTDOWN = "shutdown", HALT_OTHER = "halt-other", HOST_STOP = "host-stop", NOT_CONFIRMED = "not-confirmed";

    private static final Pattern P = Pattern.compile("^ui_runner_vm_clone VERDICT: vm_stop_confirmed=([01]) stop_kind=([a-z-]{1,24}) disk_flush=(ok|FAILED|not-attempted) macos_boot=not-determined"
            + "(?: outcome=(restart|shutdown|halt-other|host-stop|not-confirmed) psci=(none|SYSTEM_OFF|SYSTEM_RESET) quiesced=(1|0|n/a) uptime_s=(\\d{1,9}) disk_writes=(\\d{1,18}) disk_write_refused=(\\d{1,18}))?.*$");

    public final boolean stopConfirmed;
    public final String stopKind, diskFlush;
    /** null when the runner is an older one that did not print the structured fields. */
    public final String outcome, psci;
    public final int quiesced;          // 1 yes, 0 no, -1 not reported / not run
    public final long uptimeS, diskWrites, diskWriteRefused;   // -1 when not reported
    public final boolean structured;

    private G7RunVerdict(boolean sc, String kind, String flush, String outcome, String psci, int q, long up, long w, long wr) {
        stopConfirmed = sc; stopKind = kind; diskFlush = flush; this.outcome = outcome; this.psci = psci; quiesced = q; uptimeS = up; diskWrites = w; diskWriteRefused = wr;
        structured = outcome != null;
    }

    public static G7RunVerdict parse(String line) {
        if (line == null) return null;
        Matcher m = P.matcher(line);
        if (!m.matches()) return null;
        boolean s = m.group(4) != null;
        int q = !s ? -1 : m.group(6).equals("1") ? 1 : m.group(6).equals("0") ? 0 : -1;
        return new G7RunVerdict(m.group(1).equals("1"), m.group(2), m.group(3), s ? m.group(4) : null, s ? m.group(5) : null, q,
                s ? Long.parseLong(m.group(7)) : -1, s ? Long.parseLong(m.group(8)) : -1, s ? Long.parseLong(m.group(9)) : -1);
    }

    /** The stop is confirmed, every disk write was flushed with success, and (when the runner says so) every CPU thread and disk worker was finished first. */
    public boolean cleanStop() {
        return stopConfirmed && diskFlush.equals("ok") && quiesced != 0;
    }

    public boolean isRestart() { return RESTART.equals(outcome); }
}
