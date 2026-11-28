package dev.muns.hengeprobe;

import java.util.ArrayList;
import java.util.List;

/** The live progress banner of a G7 run, built only from the runner's own "g7-step:", "g7-phase:" and "g7-steptimes:" lines (host/g7_phase.h). Pure Java (no Android classes) so a plain JVM test covers it.
 *  The restore tool reports a percent of the CURRENT step only; there is no overall percent here and none is invented. */
final class G7Progress {
    private long startMs = -1, stepStartMs;
    private String step = "";
    private int pct = -1;
    private final List<String> phases = new ArrayList<>();
    private String stepTimes = "";

    synchronized void begin(long nowMs) { startMs = nowMs; step = ""; pct = -1; phases.clear(); stepTimes = ""; }

    static String stepLabel(String n) {
        switch (n) {
            case "DETECT": return "기기 감지";
            case "PREPARE": return "준비";
            case "UPLOAD_FS": return "파일시스템 전송";
            case "VERIFY_FS": return "파일시스템 검증";
            case "FLASH_FW": return "펌웨어 기록";
            case "FLASH_BB": return "베이스밴드";
            case "FUD": return "FUD";
            case "UPLOAD_IMG": return "이미지 전송";
            default: return n;
        }
    }

    private static String field(String line, String key) {
        int i = line.indexOf(" " + key + "=");
        if (i < 0) return null;
        int s = i + key.length() + 2, e = line.indexOf(' ', s);
        return line.substring(s, e < 0 ? line.length() : e).trim();
    }

    /** Takes one runner line; returns true if it was one of the progress lines. */
    synchronized boolean feed(String line, long nowMs) {
        if (line.startsWith("g7-step:")) {
            String n = field(line, "name"), p = field(line, "pct");
            if (n == null) return true;
            if (!n.equals(step)) { step = n; stepStartMs = nowMs; }
            try { pct = p == null ? -1 : Integer.parseInt(p); } catch (NumberFormatException e) { pct = -1; }
            return true;
        }
        if (line.startsWith("g7-phase:")) {
            String n = field(line, "name"), s = field(line, "secs"), m = field(line, "mb_per_s");
            if (n != null && s != null) { phases.add(n + " " + s + "초" + (m != null ? " (" + m + " MB/s)" : "")); if (phases.size() > 6) phases.remove(0); }
            return true;
        }
        if (line.startsWith("g7-steptimes:")) { stepTimes = line.substring("g7-steptimes:".length()).trim(); return true; }
        return false;
    }

    static String clock(long ms) {
        long s = Math.max(0, ms / 1000);
        return s >= 3600 ? String.format("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60) : String.format("%02d:%02d", s / 60, s % 60);
    }

    /** Empty before begin(). */
    synchronized String banner(long nowMs) {
        if (startMs < 0) return "";
        StringBuilder b = new StringBuilder();
        if (step.isEmpty()) b.append("복원 단계 보고 대기 중 (시작 준비) · 경과 ").append(clock(nowMs - startMs));
        else {
            b.append("복원 진행 — 현재 단계: ").append(stepLabel(step)).append(" (").append(step).append(')');
            if (pct >= 0) b.append(' ').append(pct).append("% (이 단계 기준)");
            b.append(" · 이 단계 ").append(clock(nowMs - stepStartMs)).append(" · 전체 경과 ").append(clock(nowMs - startMs));
        }
        if (!phases.isEmpty()) b.append("\n시작 구간: ").append(String.join(", ", phases));
        if (!stepTimes.isEmpty()) b.append("\n단계별 소요(초): ").append(stepTimes);
        return b.toString();
    }
}
