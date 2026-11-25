package dev.muns.hengeprobe;

/**
 * [G7-EXEC] The state of a running clone, kept in a static holder so it survives the Activity being recreated (the copy thread keeps running). Only stores numbers (volatile, no locks: the copy
 * thread calls update() once per MiB); the screen reads it every 500 ms. Percent comes from real bytes of data extents: copy = first half, verify = second half, and 100 % is shown ONLY for a finished,
 * successful clone (finish(true)). A stalled copy keeps its number; nothing is invented. No Android classes.
 */
public final class G7CloneProgress implements G7Clone.Progress {
    public volatile boolean running;
    public volatile Boolean ok;               // null while running
    public volatile String from = "설치 쌍", dir = "", phase = "check", message = "";
    public volatile long done, total, started, phaseStartMs, phaseStartDone, lastUpdateMs;
    private static final G7CloneProgress HOLDER = new G7CloneProgress();
    public static G7CloneProgress current() { return HOLDER; }

    public static G7CloneProgress begin(String fromName, long nowMs) {
        G7CloneProgress p = HOLDER;
        p.from = fromName; p.dir = ""; p.phase = "check"; p.done = 0; p.total = 0; p.ok = null; p.message = "";
        p.started = nowMs; p.phaseStartMs = nowMs; p.phaseStartDone = 0; p.lastUpdateMs = nowMs; p.running = true;
        return p;
    }

    @Override public void update(String ph, long d, long t, String directory) {
        long now = System.currentTimeMillis();
        if (!ph.equals(phase)) { phase = ph; phaseStartMs = now; phaseStartDone = d; }
        done = d; total = t; dir = directory; lastUpdateMs = now;
    }

    public void finish(boolean success, String msg) { message = msg; ok = success; running = false; }

    /** 0..100; 100 only for a finished successful clone. Copy and verify are each half of the data bytes; the marker step holds at 99. */
    public int percent() {
        if (ok != null) return ok ? 100 : Math.min(99, rawPercent());
        return Math.min(99, rawPercent());
    }
    private int rawPercent() {
        long t = total; if (t <= 0) return 0;
        switch (phase) {
            case "copy": return (int) (50L * Math.min(done, t) / t);
            case "verify": return 50 + (int) (49L * Math.min(done, t) / t);
            case "marker": return 99;
            default: return 0;
        }
    }

    public static String phaseName(String ph) {
        switch (ph) { case "copy": return "디스크·AUX 복사"; case "verify": return "복사본 바이트 검증"; case "marker": return "완료 표식·출처 기록"; default: return "원본 검사·공간 확인"; }
    }

    public String render(long nowMs) {
        StringBuilder sb = new StringBuilder();
        sb.append("복제 원본 ").append(from).append(" → 새 복제본 ").append(dir.isEmpty() ? "(번호 정하는 중)" : dir).append('\n');
        sb.append("단계: ").append(phaseName(phase)).append(" · 전체 ").append(percent()).append("%");
        if (ok == null && total > 0 && (phase.equals("copy") || phase.equals("verify"))) {
            sb.append("\n처리 ").append(G7Storage.fmt(done)).append(" / ").append(G7Storage.fmt(total)).append(" (이 단계, 실제 데이터 기준)");
            long ms = Math.max(1, nowMs - phaseStartMs), bytes = done - phaseStartDone;
            if (bytes > 0 && nowMs - phaseStartMs >= 1500) {
                long perSec = bytes * 1000 / ms;
                sb.append("\n속도 ").append(G7Storage.fmt(perSec)).append("/s");
                if (perSec > 0) sb.append(" · 이 단계 남은 시간 약 ").append(clock((total - done) / perSec));
            }
        }
        sb.append("\n경과 ").append(clock((nowMs - started) / 1000));
        if (ok == null) {
            long idle = (nowMs - lastUpdateMs) / 1000;
            sb.append(" · 마지막 갱신 ").append(idle).append("초 전");
            if (idle >= 20) sb.append(" (진행이 멈춘 것처럼 보입니다. 숫자는 그대로 둡니다)");
        }
        return sb.toString();
    }

    static String clock(long sec) { if (sec < 0) sec = 0; return String.format("%02d:%02d", sec / 60, sec % 60); }
}
