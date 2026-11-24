package dev.muns.hengeprobe;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * What the screen shows about a clone-boot run, taken from the runner's output lines AS THEY ARRIVE (so a result is not lost when the 6 KB screen tail has moved on):
 * the clock, the last line seen, the memory and CPU-counter lines, the disk counters, the four E-mode steps, the important lines (everything except the noisy repeats), and the
 * errors, which stay until the run ends. It only reads text: it starts nothing, writes nothing and never looks at the log file. The full log file is unchanged.
 * Honest limits: "saveenv sent" is NOT "saved" (saveenv has no answer on the wire); a CPU counter that did not move only says it was not seen moving.
 */
final class G7CloneMonitor {
    /** Step state of the E-mode sequence. */
    static final int WAIT = 0, DONE = 1, SENT = 2, FAILED = 3, SKIPPED = 4;
    private static final int KEEP_LINES = 25, KEEP_ERRORS = 10, MAX_LINE = 160;

    private static final Pattern ALIVE = Pattern.compile("^g7-alive: t=(\\d+)s guest_blocks=cpu0:(\\d+),cpu1:(\\d+) disk_done=(\\d+) disk_inflight=(\\d+).*$");
    private static final Pattern PCS = Pattern.compile("^hengevm: cpu (\\d) pc sample #(\\d+) .*$");
    private static final Pattern MEM = Pattern.compile("^g7-mem: t=(\\d+)s rss=(\\S+) peak=(\\S+) anon=(\\S+) .*available=(\\S+).*$");
    private static final Pattern STEP1 = Pattern.compile("^usb autoboot-fix: step 1 getenv auto-boot = \"([^\"]*)\"$");
    private static final Pattern SENTOK = Pattern.compile("^usb autoboot-fix: (step [24]) .* sent -> (-?\\d+)$");
    private static final Pattern STEP3 = Pattern.compile("^usb autoboot-fix: step 3 getenv auto-boot = \"([^\"]*)\"$");
    private static final Pattern STEP5 = Pattern.compile("^usb autoboot-fix: step 5 getenv auto-boot after saveenv = \"([^\"]*)\".*$");

    private boolean replay;   // built from a saved log file: no clock was recorded, so no times are shown
    private long startMs = -1, lastLineMs = -1;
    private long usbDiagLines, otherNoise;
    private int aliveT = -1; private long blocks0, blocks1, diskDone; private long aliveMs = -1;
    private final long[] pcCount = new long[2]; private final long[] pcMs = {-1, -1};
    private String rss = "?", peak = "?", avail = "?"; private long memMs = -1;
    private int modeSeen = -1;
    private final int[] step = new int[5];
    private String step1Value = "", step3Value = "", step5Value = "";
    private boolean autobootAlreadyTrue, autobootDone, autobootStopped;
    private final ArrayDeque<String> lines = new ArrayDeque<>();
    private final List<String> errors = new ArrayList<>();

    private static boolean noisy(String l) {
        return l.startsWith("bdif usb diag:") || l.startsWith("hengevm: cpu ") || l.startsWith("g7-alive:") || l.startsWith("g7-disk:") || l.startsWith("g7-mem") || l.startsWith("hvgamma:")
                || l.startsWith("[usb diag:") || l.startsWith("Uploading:");
    }

    /** A monitor rebuilt from a saved clone-boot log (after the app was restarted, or the run object is gone). Reads the text only. */
    static G7CloneMonitor replay(String logText) {
        G7CloneMonitor m = new G7CloneMonitor();
        m.replay = true;
        for (String l : logText.split("\n")) if (!l.isEmpty()) m.feed(l, 0);
        return m;
    }

    synchronized void feed(String line, long nowMs) {
        if (startMs < 0) startMs = nowMs;
        lastLineMs = nowMs;
        if (line.startsWith("bdif usb diag:") || line.startsWith("[usb diag:")) { usbDiagLines++; return; }
        Matcher m = ALIVE.matcher(line);
        if (m.matches()) { aliveT = Integer.parseInt(m.group(1)); blocks0 = Long.parseLong(m.group(2)); blocks1 = Long.parseLong(m.group(3)); diskDone = Long.parseLong(m.group(4)); aliveMs = nowMs; return; }
        m = PCS.matcher(line);
        if (m.matches()) { int c = Integer.parseInt(m.group(1)); if (c >= 0 && c < 2) { pcCount[c] = Long.parseLong(m.group(2)); pcMs[c] = nowMs; } return; }
        m = MEM.matcher(line);
        if (m.matches()) { rss = m.group(2); peak = m.group(3); avail = m.group(5); memMs = nowMs; return; }
        if (line.startsWith("usb autoboot-fix:")) autobootLine(line);
        if (noisy(line)) { otherNoise++; return; }
        String t = (replay ? "" : (nowMs - startMs) / 60000 + ":" + String.format("%02d", ((nowMs - startMs) / 1000) % 60) + " ") + (line.length() > MAX_LINE ? line.substring(0, MAX_LINE) : line);
        lines.addLast(t);
        while (lines.size() > KEEP_LINES) lines.removeFirst();
        String low = line.toLowerCase();
        if ((low.contains("failed") || low.contains("stop") || low.contains("refused") || low.contains("error")) && !line.startsWith("g7-cboot-verdict") && errors.size() < KEEP_ERRORS) errors.add(t);
    }

    private void autobootLine(String line) {
        Matcher m = STEP1.matcher(line);
        if (m.matches()) { step1Value = m.group(1); step[0] = DONE; return; }
        if (line.contains("first getenv auto-boot failed")) { step[0] = FAILED; autobootStopped = true; return; }
        if (line.contains("already true")) { autobootAlreadyTrue = true; step[1] = SKIPPED; step[2] = SKIPPED; step[3] = SKIPPED; return; }
        m = SENTOK.matcher(line);
        if (m.matches()) { int i = m.group(1).equals("step 2") ? 1 : 3; step[i] = Integer.parseInt(m.group(2)) >= 0 ? SENT : FAILED; return; }
        m = STEP3.matcher(line);
        if (m.matches()) { step3Value = m.group(1); step[2] = "true".equals(step3Value) ? DONE : FAILED; return; }
        m = STEP5.matcher(line);
        if (m.matches()) { step5Value = m.group(1); step[4] = DONE; return; }
        if (line.contains("step 5 getenv after saveenv FAILED")) { step[4] = FAILED; return; }
        if (line.contains("STOP")) { autobootStopped = true; for (int i = 0; i < 4; i++) if (step[i] == WAIT) { step[i] = line.contains("at step " + (i + 1)) ? FAILED : WAIT; } if (!anyFailed()) step[0] = step[0] == WAIT ? FAILED : step[0]; return; }
        if (line.contains("DONE")) autobootDone = true;
    }

    private boolean anyFailed() { for (int s : step) if (s == FAILED) return true; return false; }

    private static String word(int s) { return s == DONE ? "완료" : s == SENT ? "전송됨" : s == FAILED ? "실패" : s == SKIPPED ? "건너뜀" : "대기"; }

    /** True once any E-mode line was seen (so the section is only shown for that mode). */
    synchronized boolean sawAutobootFix() { return step[0] != WAIT || autobootAlreadyTrue || autobootStopped || autobootDone || step[1] != WAIT; }

    synchronized int stepState(int i) { return step[i]; }
    synchronized boolean autobootDone() { return autobootDone; }
    synchronized long usbDiagLines() { return usbDiagLines; }
    synchronized int errorCount() { return errors.size(); }

    private String ago(long nowMs, long thenMs) { return thenMs < 0 ? "아직 없음" : replay ? "저장 로그(시각 미기록)" : Math.max(0, (nowMs - thenMs) / 1000) + "초 전"; }

    /** The status block. alive = the runner process has not ended; verdict = Run.verdict() once it has ended (else null). */
    synchronized String render(long nowMs, boolean alive, int exitCode, String stageSummary, String verdict, String frameNote) {
        StringBuilder b = new StringBuilder();
        long el = startMs < 0 ? 0 : (nowMs - startMs) / 1000;
        boolean noExit = !alive && exitCode == Integer.MIN_VALUE;
        b.append(alive ? "● 실행 중" : noExit ? "■ 종료 기록 없음(비정상 종료 가능, 원인 미확인)" : exitCode == 0 ? "■ 정상 종료(코드 0)" : "■ 종료(코드 " + exitCode + ", 비정상 또는 강제 종료 가능)");
        if (replay) b.append("  [저장된 로그를 읽은 것]\n"); else b.append("  경과 ").append(el / 60).append(':').append(String.format("%02d", el % 60)).append('\n');
        b.append("러너 프로세스: ").append(alive ? "살아 있음" : noExit ? "확인 불가(종료 줄 없음)" : "끝남").append(" / 마지막 출력 ").append(ago(nowMs, lastLineMs)).append('\n');
        b.append("CPU 실행 관측: ");
        if (pcMs[0] < 0 && pcMs[1] < 0 && aliveMs < 0) b.append("아직 관측 없음");
        else b.append("cpu0 샘플 #").append(pcCount[0]).append('(').append(ago(nowMs, pcMs[0])).append("), cpu1 #").append(pcCount[1]).append('(').append(ago(nowMs, pcMs[1])).append("), 블록 ").append(blocks0).append('/').append(blocks1).append(" (").append(ago(nowMs, aliveMs)).append(')');
        b.append("\n  (관측 주기가 길어 숫자가 안 늘어도 멈췄다는 뜻은 아닙니다)\n");
        b.append("디스크 읽기 완료 ").append(diskDone).append("건 / 메모리 RSS ").append(rss).append(" (최대 ").append(peak).append(", 폰 여유 ").append(avail).append(", ").append(ago(nowMs, memMs)).append(")\n");
        b.append("부팅 단계(추정): ").append(stageSummary == null ? "?" : stageSummary.split("\n")[0]).append('\n');
        b.append("화면: ").append(frameNote).append('\n');
        if (sawAutobootFix()) {
            b.append("\n[E 모드 진행]\n");
            b.append("1 기존 auto-boot 확인: ").append(word(step[0])).append(step1Value.isEmpty() ? "" : " (값 " + step1Value + ")").append('\n');
            b.append("2 setenv auto-boot true 전송: ").append(word(step[1])).append('\n');
            b.append("3 변경값 true 확인: ").append(word(step[2])).append(step3Value.isEmpty() ? "" : " (값 " + step3Value + ")").append('\n');
            b.append("4 saveenv 명령 전송: ").append(word(step[3])).append("  ← 전송만 확인됩니다(저장 성공이 아님)\n");
            b.append("5 saveenv 뒤 값: ").append(word(step[4])).append(step5Value.isEmpty() ? "" : " (값 " + step5Value + ", 메모리 값)").append('\n');
            b.append("VM 종료·flush: ").append(verdict == null ? "대기(끝나야 확인)" : verdict).append('\n');
            b.append("재부팅 후 유지 확인: 미검증(일반 부팅 A로 확인)\n");
        } else if (verdict != null) {
            b.append("결과: ").append(verdict).append('\n');
        }
        if (usbDiagLines > 0) b.append("\n반복 로그 집계: bdif usb diag ").append(usbDiagLines).append("줄(화면에서 생략, 로그 파일에는 보존)\n");
        if (!errors.isEmpty()) { b.append("\n[오류·중단 줄(실행이 끝날 때까지 유지)]\n"); for (String e : errors) b.append(e).append('\n'); }
        b.append("\n[최근 중요 로그]\n");
        for (String l : lines) b.append(l).append('\n');
        return b.toString();
    }
}
