package dev.muns.hengeprobe;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * [G7-EXEC] Which part of the boot the guest seems to be in, from the CPU samples the engine already prints ("hengevm: cpu N pc sample #K pc=0x... el=E") and the runner's own status lines.
 * This is a HEURISTIC and says so wherever it is shown: an exception level of 0 is user space; EL1 at a kernel-space address (0xfffffe00_00000000 and up, where XNU lives) is the kernel; any other
 * sample is earlier firmware (iBoot, the ROM, SEP-less boot code). "screen" means the runner published at least one frame. No Android classes: runs on a plain JVM.
 */
public final class G7BootStage {
    private G7BootStage() {}
    public enum Stage { FIRMWARE, KERNEL, USER, SCREEN, RECOVERY }
    static final Pattern PC = Pattern.compile("hengevm: cpu [0-9]+ pc sample #[0-9]+ pc=(0x[0-9a-fA-F]+) el=([0-9])");
    static final Pattern FRAMES = Pattern.compile("frames published ([0-9]+)");
    /** iBootStage1 prints this when it gives up on the normal boot and waits at its command prompt. */
    static final Pattern RECOVERY = Pattern.compile("iBoot[A-Za-z0-9]* recovery mode");
    static final long KERNEL_BASE = 0xfffffe0000000000L;

    /** The stage of one pc sample, or null for a line that is not one. */
    static Stage of(String line) {
        Matcher m = PC.matcher(line);
        if (!m.find()) return null;
        long pc; try { pc = Long.parseUnsignedLong(m.group(1).substring(2), 16); } catch (NumberFormatException e) { return null; }
        int el = m.group(2).charAt(0) - '0';
        if (el == 0) return Stage.USER;
        if (el >= 1 && Long.compareUnsigned(pc, KERNEL_BASE) >= 0) return Stage.KERNEL;
        return Stage.FIRMWARE;
    }

    /** Remembers when each stage was first seen (seconds since the start of the run), and the last stage seen. */
    public static final class Tracker {
        private final long[] first = {-1, -1, -1, -1, -1};
        private Stage last;
        /** Feeds one output line; returns the stage when this line is the FIRST sign of it (so the caller can write one marker line), else null. */
        public synchronized Stage feed(String line, long elapsedMs) {
            if (RECOVERY.matcher(line).find()) {
                last = Stage.RECOVERY;
                if (first[Stage.RECOVERY.ordinal()] < 0) { first[Stage.RECOVERY.ordinal()] = elapsedMs; return Stage.RECOVERY; }
                return null;
            }
            Stage s = of(line);
            // After the firmware reported recovery mode, EL0 code is the firmware's own (iBoot runs part of itself at EL0): it is NOT macOS user space.
            if (s == Stage.USER && first[Stage.RECOVERY.ordinal()] >= 0) s = Stage.FIRMWARE;
            if (s == null) {
                Matcher f = FRAMES.matcher(line);
                if (f.find() && !f.group(1).equals("0") && first[Stage.SCREEN.ordinal()] < 0) { first[Stage.SCREEN.ordinal()] = elapsedMs; return Stage.SCREEN; }
                return null;
            }
            last = s;
            if (first[s.ordinal()] < 0) { first[s.ordinal()] = elapsedMs; return s; }
            return null;
        }
        public synchronized String summary() {
            StringBuilder sb = new StringBuilder("부팅 단계(CPU 샘플로 추정한 값이며 확정이 아님): ");
            String[] names = {"펌웨어", "커널", "사용자 공간", "화면 출력"};
            for (int i = 0; i < 4; i++) sb.append(names[i]).append(' ').append(first[i] < 0 ? "아직 보이지 않음" : (first[i] / 1000) + "초").append(i < 3 ? ", " : "");
            if (first[Stage.RECOVERY.ordinal()] >= 0) sb.append(" | iBoot 복구 모드 진입 ").append(first[Stage.RECOVERY.ordinal()] / 1000).append("초(이후 EL0 샘플은 사용자 공간으로 세지 않음)");
            if (last != null) sb.append(" | 마지막 샘플: ").append(last.name().toLowerCase());
            return sb.toString();
        }
        public synchronized boolean seen(Stage s) { return first[s.ordinal()] >= 0; }
    }
}
