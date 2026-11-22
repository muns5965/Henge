package dev.muns.hengeprobe;

/** The app's final verdict on a G4 UI diagnostic run (the G4 UI shared library / host/ui_runner_vm_g4.c), reached
 *  purely from what that process actually reported -- never by assuming success from a clean waitFor() alone.
 *  Plain Java, no Android classes: tests/java/VmRunnerG4VerdictTest.java runs it on a JVM.
 *
 *  host/ui_runner_vm_g4.c's own documented "Exit codes:" line: 0 stopped and confirmed, G4 passed; 1 settings
 *  refused / machine could not be made / G4 did not start / G4 ran but failed; 2 bad arguments or the shared
 *  file could not be made; 3 the stop was NOT confirmed within the wait; 4 hv_g4_wait() returned -1 (did not
 *  end even after cancellation) -- per host/install_g4.h nothing could be freed or closed, so the process
 *  ends immediately via _exit(4), right after an explicit fflush(stdout) that file already has (checked by
 *  reading it: the RESULT line is not lost to _exit()'s own usual unflushed-stdio risk). Code 4 is judged
 *  FAILED from the exit code alone, before any RESULT line is even looked at -- per that same contract,
 *  "nothing could be freed or closed" already means failure whether or not the RESULT line made it through;
 *  a missing RESULT line on code 4 is noted as a separate, secondary thing to look into (log collection), not
 *  something that changes or softens the FAILED verdict.
 *
 *  Only exit code 0 is ever even considered for PASS, and only when the run's single
 *  "ui_runner_vm_g4 RESULT: " line itself says so: starts with "stopped, confirmed" AND contains
 *  ", g4 passed (" -- the exact two conditions host/ui_runner_vm_g4.c's own main() requires jointly before it
 *  ever returns 0 (see its "return (!confirmed) ? 3 : g4_start_failed ? 1 : (g_g4_finish_failed == 0) ? 0 :
 *  1;"). A 0 with no such line, or with a line that does not say so, is never treated as a pass. */
final class VmRunnerG4Verdict {
    enum Kind { PASS, FAILED, NO_VERDICT }

    static final class Result {
        final Kind kind;
        final String text;
        Result(Kind kind, String text) { this.kind = kind; this.text = text; }
    }

    static final String RESULT_PREFIX = "ui_runner_vm_g4 RESULT: ";
    static final String PASS_CONFIRMED = RESULT_PREFIX + "stopped, confirmed";
    static final String PASS_MARKER = ", g4 passed (";
    static final int MAX_KEPT = 50;

    static boolean isKept(String line) { return line.startsWith(RESULT_PREFIX); }

    static Result evaluate(String output, boolean timedOut, int exitCode, boolean resultOverflow, boolean readerUnfinished) {
        if (timedOut) return new Result(Kind.FAILED, "제한 시간 초과로 프로세스를 강제 종료했습니다(최종 판정 없음, 세션 정리 확인 불가)");
        if (exitCode == 4) {
            boolean hasResult = hasResultLine(output);
            return new Result(Kind.FAILED, "hv_g4_wait()가 취소 후에도 끝나지 않아 아무것도 안전하게 해제/종료하지 못한 채 즉시 종료했습니다(종료 코드 4)"
                    + (hasResult ? "" : " -- RESULT 줄도 수신되지 않음(로그 수집 경로는 별도로 확인해야 함)"));
        }
        if (exitCode != 0) {
            String why = exitCode == 1 ? "설정 거부, 머신 생성 실패, G4 시작 실패 또는 G4 실패 중 하나입니다 (종료 코드 1)"
                    : exitCode == 2 ? "인자가 잘못되었거나 공유 파일을 만들지 못했습니다 (종료 코드 2)"
                    : exitCode == 3 ? "정지가 대기 시간 안에 확인되지 않았습니다 (종료 코드 3)"
                    : exitCode > 128 ? "시그널로 종료된 것 같습니다 (종료 코드 " + exitCode + ", 확정 아님)"
                    : "비정상 종료 (종료 코드 " + exitCode + ")";
            return new Result(Kind.FAILED, why);
        }
        if (readerUnfinished) return new Result(Kind.FAILED, "출력 수집이 끝나지 않아 판정할 수 없습니다(통과 아님)");
        if (resultOverflow) return new Result(Kind.FAILED, "결과 줄이 " + MAX_KEPT + "개를 넘었습니다(중복·이상 출력, 통과 아님)");
        java.util.List<String> lines = new java.util.ArrayList<>();
        for (String raw : output.split("\n", -1)) {
            String line = raw.endsWith("\r") ? raw.substring(0, raw.length() - 1) : raw;
            if (line.startsWith(RESULT_PREFIX)) lines.add(line);
        }
        if (lines.isEmpty()) return new Result(Kind.NO_VERDICT, "RESULT 줄이 없습니다(종료 코드 0이라도 성공으로 처리하지 않음; 결과 계약 위반)");
        if (lines.size() > 1) return new Result(Kind.FAILED, "RESULT 줄이 " + lines.size() + "개입니다(중복 또는 충돌, 통과 아님)");
        String line = lines.get(0);
        boolean passed = line.startsWith(PASS_CONFIRMED) && line.contains(PASS_MARKER);
        if (!passed) return new Result(Kind.FAILED, "종료 코드는 0이지만 RESULT 줄이 성공을 확인하지 않습니다(종료 결과 불일치): " + line);
        return new Result(Kind.PASS, "정지 확인됨, G4 통과(RESULT: g4 passed)");
    }

    private static boolean hasResultLine(String output) {
        for (String raw : output.split("\n", -1)) if (raw.startsWith(RESULT_PREFIX)) return true;
        return false;
    }
}
