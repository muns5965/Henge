package dev.muns.hengeprobe;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * Formats Android's ApplicationExitInfo records for the screen. No Android classes here, so it runs on a plain JVM.
 * The records describe the Java app process only: the native runner is a child of the app, and the system does not
 * promise a record of its own for it. A record with the app's reason is therefore never read as the runner's reason.
 */
public final class G7ExitInfo {
    private G7ExitInfo() {}

    /** One record, copied from ApplicationExitInfo by the Android side. Negative or missing numbers mean unknown. */
    public static final class Record {
        public final int pid; public final String process; public final long timestampMs; public final int reason;
        public final int status; public final String description; public final long pssKb; public final long rssKb;
        /** The reason name taken from the SDK constants by the Android side (null: the fallback table below is used), and a one-line note on the trace (ANR text or tombstone file), or null. */
        public final String reasonName; public final String traceNote;
        public Record(int pid, String process, long timestampMs, int reason, int status, String description, long pssKb, long rssKb) {
            this.pid = pid; this.process = process; this.timestampMs = timestampMs; this.reason = reason;
            this.status = status; this.description = description; this.pssKb = pssKb; this.rssKb = rssKb; this.reasonName = null; this.traceNote = null;
        }
        public Record(int pid, String process, long timestampMs, int reason, int status, String description, long pssKb, long rssKb, String reasonName, String traceNote) {
            this.pid = pid; this.process = process; this.timestampMs = timestampMs; this.reason = reason;
            this.status = status; this.description = description; this.pssKb = pssKb; this.rssKb = rssKb; this.reasonName = reasonName; this.traceNote = traceNote;
        }
    }

    /** ApplicationExitInfo.REASON_* values (API 30+), written from memory: check them against the SDK before relying on a name. */
    public static String reasonName(int r) {
        switch (r) {
            case 0: return "UNKNOWN"; case 1: return "EXIT_SELF"; case 2: return "SIGNALED"; case 3: return "LOW_MEMORY";
            case 4: return "CRASH"; case 5: return "CRASH_NATIVE"; case 6: return "ANR"; case 7: return "INITIALIZATION_FAILURE";
            case 8: return "PERMISSION_CHANGE"; case 9: return "EXCESSIVE_RESOURCE_USAGE"; case 10: return "USER_REQUESTED";
            case 11: return "USER_STOPPED"; case 12: return "DEPENDENCY_DIED"; case 13: return "OTHER"; case 14: return "FREEZER";
            case 15: return "PACKAGE_STATE_CHANGE"; case 16: return "PACKAGE_UPDATED";
            default: return "REASON_" + r;
        }
    }

    /** For SIGNALED the status is the signal number; for EXIT_SELF it is the exit code; otherwise it is only printed. */
    static String statusText(int reason, int status) {
        if (reason == 2) return "시그널 " + status + (status == 9 ? "(SIGKILL)" : status == 11 ? "(SIGSEGV)" : status == 6 ? "(SIGABRT)" : status == 7 ? "(SIGBUS)" : "");
        if (reason == 1) return "종료 코드 " + status;
        return "상태 " + status;
    }

    /** 0 is what Android returns when PSS/RSS was not measured, so it is shown as unknown, never as 0 MiB. */
    static String mib(long kb) { return kb <= 0 ? "?" : (kb / 1024) + "MiB"; }

    static String time(long ms) {
        SimpleDateFormat f = new SimpleDateFormat("MM-dd HH:mm:ss", Locale.US);
        f.setTimeZone(TimeZone.getTimeZone("Asia/Seoul"));
        return f.format(new Date(ms));
    }

    /** At most `max` newest records (given newest first), one line each, with the caveat once at the end. */
    public static String format(List<Record> records, int currentPid, int max) {
        StringBuilder sb = new StringBuilder("시스템 종료 기록(ApplicationExitInfo), 현재 앱 PID " + currentPid + "\n");
        if (records == null || records.isEmpty()) {
            sb.append("  기록 없음(설치 갱신 등으로 지워졌을 수 있음)\n");
        } else {
            List<Record> shown = new ArrayList<>(records.subList(0, Math.min(max, records.size())));
            for (Record r : shown) {
                sb.append("  ").append(time(r.timestampMs)).append(" pid=").append(r.pid).append(' ').append(r.process == null ? "?" : r.process)
                  .append(" ").append(r.reasonName != null ? r.reasonName : reasonName(r.reason)).append(" (").append(statusText(r.reason, r.status)).append(")")
                  .append(" PSS=").append(mib(r.pssKb)).append(" RSS=").append(mib(r.rssKb));
                if (r.description != null && !r.description.isEmpty()) sb.append(" — ").append(r.description);
                sb.append('\n');
                if (r.traceNote != null && !r.traceNote.isEmpty()) sb.append("    trace: ").append(r.traceNote).append('\n');
            }
        }
        sb.append("  주의: 이 기록은 앱 프로세스 기준입니다. 실행기(자식 프로세스)의 별도 기록이 없을 수 있으며, 있어도 앱의 사유를 실행기의 사유로 읽지 마세요.\n");
        return sb.toString();
    }
}
