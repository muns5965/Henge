package dev.muns.hengeprobe;

import java.util.ArrayDeque;

/**
 * Decides whether the app starts the same clone again after the guest asked for a restart (PSCI SYSTEM_RESET). Pure Java, driven by the caller's clock.
 *
 * What is required (the original Henge restarts on a guest reset, and this does the same): a parsed VERDICT whose outcome is "restart", a confirmed stop, a disk flush that
 * succeeded, and every CPU thread and disk worker finished before the flush (quiesced reported as 1; not reported is not enough), and that the person did not stop the run. What is NOT required: a minimum uptime or a
 * picture on the screen, because firmware and macOS updates restart before either. Only a reset LOOP is limited: at most maxRestarts restarts in windowMs, with a growing wait
 * between them. An error, a host stop, an unconfirmed stop, a failed flush or an older runner's line without outcome never restarts.
 */
public final class G7RestartPolicy {
    public static final class Decision {
        public final boolean restart; public final long delayMs; public final String reason;
        Decision(boolean r, long d, String why) { restart = r; delayMs = d; reason = why; }
    }

    private final int maxRestarts; private final long windowMs; private final long[] backoffMs;
    private final ArrayDeque<Long> restarts = new ArrayDeque<>();

    public G7RestartPolicy() { this(3, 10 * 60 * 1000L, new long[] {2000, 8000, 30000}); }
    public G7RestartPolicy(int maxRestarts, long windowMs, long[] backoffMs) { this.maxRestarts = maxRestarts; this.windowMs = windowMs; this.backoffMs = backoffMs.clone(); }

    /** Call when a run has ended. nowMs is the end time; cancelled is true when the person asked to stop. A restart that is allowed is counted. */
    public synchronized Decision decide(G7RunVerdict v, boolean cancelled, long nowMs) {
        if (cancelled) return no("중지를 요청해서 재시작하지 않습니다.");
        if (v == null) return no("결과 줄이 없거나 해석되지 않아 재시작하지 않습니다.");
        if (!v.structured) return no("러너가 종료 계기를 구조화해 알려주지 않아 재시작하지 않습니다.");
        if (!v.isRestart()) return no("게스트의 재시작 요청(SYSTEM_RESET)이 아니라서 재시작하지 않습니다(" + v.outcome + ").");
        if (!v.stopConfirmed) return no("VM 종료가 확인되지 않아 재시작하지 않습니다.");
        if (!v.diskFlush.equals("ok")) return no("디스크 flush가 성공하지 않아 재시작하지 않습니다.");
        if (v.quiesced != 1) return no("CPU 스레드·디스크 작업자 종료가 확인되지 않았습니다(" + (v.quiesced == 0 ? "일부 미종료" : "미보고") + "). 재시작하지 않습니다.");
        while (!restarts.isEmpty() && nowMs - restarts.peekFirst() > windowMs) restarts.pollFirst();
        if (restarts.size() >= maxRestarts) return no("재시작 반복 방지: " + (windowMs / 60000) + "분 안에 " + maxRestarts + "번 재시작했습니다. 더 이상 자동으로 재시작하지 않습니다.");
        long delay = backoffMs[Math.min(restarts.size(), backoffMs.length - 1)];
        restarts.addLast(nowMs);
        return new Decision(true, delay, "게스트가 재시작을 요청했습니다(SYSTEM_RESET). " + (delay / 1000) + "초 뒤 같은 복제본을 다시 시작합니다(" + restarts.size() + "/" + maxRestarts + ").");
    }

    /** A person's own start resets the count. */
    public synchronized void reset() { restarts.clear(); }

    private static Decision no(String why) { return new Decision(false, 0, why); }
}
