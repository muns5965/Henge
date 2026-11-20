package dev.muns.hengeprobe;

/** The app's final verdict on a G4 run (vmrun --install-g4). Plain Java, no Android classes; mirrors G3Verdict exactly, only the line prefixes and the final-line
 *  text differ (host/install_g4.c, host/vmrun.c). G4 runs G3's whole TSS + DFU->Recovery firmware transfer first, then continues into recovery_enter_restore()
 *  (AppleLogo, the components iBoot loads, the restore ramdisk, the device tree, SEP, the kernelcache -- all signed by the SAME TSS tickets, no new Apple
 *  request) and stops only once the device is confirmed in MODE_RESTORE (RecoveryOS running off its own ramdisk). A pass here is NOT a restore and does not
 *  mean a real install would succeed -- it says the engine's own TSS stage, the DFU->Recovery firmware transfer and RecoveryOS entry, reached through the
 *  real install entry point, all completed and the tool stopped cleanly right after, before anything was written to the virtual disk.
 *  Input: the lines the app kept for the verdict (every line starting "install g4 RESULT: " or "vmrun RESULT: install g4 ", joined by '\n'), whether the process hit
 *  the time limit, its exit code, and two reasons the kept lines may be unreliable (more result lines than the app keeps; the output reader had not finished).
 *  Whatever the engine's own table says, the verdict is a pass only when ALL hold: no time-out, exit code 0, the output complete, exactly one G4 result line,
 *  exactly one final vmrun line that is exactly "vmrun RESULT: install g4 passed; machine stopped". A trailing '\r' is dropped from each line. */
final class G4Verdict {
    enum Kind { PASS, PARTIAL_PASS, FAILED, NO_VERDICT }

    static final class Result {
        final Kind kind;
        final String text;
        Result(Kind kind, String text) { this.kind = kind; this.text = text; }
    }

    static final String G4_PREFIX = "install g4 RESULT: ";
    static final String VM_PREFIX = "vmrun RESULT: install g4 ";
    static final String VM_OK = "vmrun RESULT: install g4 passed; machine stopped";
    static final int MAX_KEPT = 50;

    static boolean isKept(String line) { return line.startsWith(G4_PREFIX) || line.startsWith(VM_PREFIX); }

    static Result evaluate(String output, boolean timedOut, int exitCode, boolean resultOverflow, boolean readerUnfinished) {
        if (timedOut) return new Result(Kind.FAILED, "제한 시간 초과로 프로세스를 강제 종료했습니다(최종 판정 없음, 세션 정리 확인 불가)");
        if (exitCode != 0) {
            String why = exitCode == 1 ? "G4 판정표에 실패 행이 있습니다 (종료 코드 1)"
                    : exitCode == 3 ? "G4 검사 뒤 가상 머신 정지를 확인하지 못했습니다 (종료 코드 3)"
                    : exitCode == 4 ? "복원 스레드가 끝나지 않았습니다 (종료 코드 4)"
                    : exitCode > 128 ? "시그널로 종료된 것 같습니다 (종료 코드 " + exitCode + ", 확정 아님)"
                    : "비정상 종료 (종료 코드 " + exitCode + ")";
            return new Result(Kind.FAILED, why);
        }
        if (readerUnfinished) return new Result(Kind.FAILED, "출력 수집이 끝나지 않아 판정할 수 없습니다(통과 아님)");
        if (resultOverflow) return new Result(Kind.FAILED, "결과 줄이 " + MAX_KEPT + "개를 넘었습니다(중복·이상 출력, 통과 아님)");
        java.util.List<String> g4 = new java.util.ArrayList<>(), vm = new java.util.ArrayList<>();
        for (String raw : output.split("\n", -1)) {
            String line = raw.endsWith("\r") ? raw.substring(0, raw.length() - 1) : raw;
            if (line.startsWith(G4_PREFIX)) g4.add(line);
            else if (line.startsWith(VM_PREFIX)) vm.add(line);
        }
        if (g4.isEmpty()) return new Result(Kind.NO_VERDICT, "G4 최종 RESULT 줄이 없습니다(성공으로 처리하지 않음)");
        if (g4.size() > 1) return new Result(Kind.FAILED, "G4 RESULT 줄이 " + g4.size() + "개입니다(중복 또는 충돌, 통과 아님)");
        if (vm.isEmpty()) return new Result(Kind.NO_VERDICT, "최종 vmrun RESULT 줄이 없습니다(성공으로 처리하지 않음)");
        if (vm.size() > 1) return new Result(Kind.FAILED, "최종 vmrun RESULT 줄이 " + vm.size() + "개입니다(통과 아님)");
        String rest = g4.get(0).substring(G4_PREFIX.length());
        int sp = rest.indexOf(' ');
        String token = sp < 0 ? rest : rest.substring(0, sp);
        if (!vm.get(0).equals(VM_OK)) return new Result(Kind.FAILED, "최종 줄이 '" + VM_OK + "'가 아닙니다: " + vm.get(0));
        if (token.equals("g4_passed")) return new Result(Kind.PASS, "실제 Apple TSS 응답 수신 + 서명된 펌웨어(iBSS/iBEC) 기기 전송 + RecoveryOS 구성요소 전송 + Recovery→Restore 전환(RecoveryOS 자체 램디스크로 구동) 확인, 디스크·aux 불변 검증, 가상 머신 정지 확인까지 완료 (복원 성공을 뜻하지 않음, 디스크 쓰기는 아직 없음)");
        if (token.equals("g4_passed_metadata_only")) return new Result(Kind.PARTIAL_PASS, "나머지 검사는 통과, 디스크 내용은 검증하지 못함(크기·수정시각만) — 완전 통과로 세지 마세요");
        if (token.equals("FAILED")) return new Result(Kind.FAILED, "G4 판정표 실패 (위 FAILED 행 참조)");
        return new Result(Kind.FAILED, "알 수 없는 결과 토큰: " + token);
    }
}
