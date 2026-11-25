package dev.muns.hengeprobe;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public class G7CloneHistoryTest {
    static int failed = 0;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) failed++; }
    static void log(Path files, long ms, String clone, String body) throws Exception {
        Files.write(files.resolve("g7_cboot_" + ms + ".txt"), ("g7-cboot-start: " + ms + "\nui_runner_vm_clone: clone folder /data/data/x/files/vm/clones/" + clone + " verified (same vm folder)\n" + body).getBytes("UTF-8"));
    }
    public static void main(String[] a) throws Exception {
        Path files = Files.createTempDirectory("hist"), priv = Files.createDirectories(files.resolve("vm"));
        log(files, 1795600000000L, "clone-001", "Supervisor iBootStage1\nEnd of iBootStage2 serial output\ng7-stage: kernel first seen at 40s (estimate)\nui_runner_vm_clone status: pid 1 cmds popped 5 dispatched 5 | frames published 8356 (pvg 0)\nvm_stop_confirmed=1 stop_kind=time-limit disk_flush=ok macos_boot=not-determined\ng7-cboot-exit: 0\n");
        log(files, 1795622923682L, "clone-006", "Supervisor iBootStage1\nEntering iBootStage1 recovery mode, starting command prompt\nui_runner_vm_clone status: pid 1 cmds popped 0 dispatched 0 | frames published 0 (pvg 0)\n");
        log(files, 1795622999999L, "clone-001", "Supervisor iBootStage1\nui_runner_vm_clone status: pid 1 cmds popped 0 | frames published 12 (pvg 0)\n");
        Files.write(files.resolve("g7_cboot_1795623000000.txt"), "no clone line in this one frames published 99999".getBytes());
        G7CloneHistory.Boots b1 = G7CloneHistory.bootsOf(files.toFile(), "clone-001"), b6 = G7CloneHistory.bootsOf(files.toFile(), "clone-006"), b2 = G7CloneHistory.bootsOf(files.toFile(), "clone-002");
        check("clone-001: 2 runs, stage2 and kernel estimate from an earlier run, max frames and gpu cmds over runs", b1.runs == 2 && b1.stage2 && b1.kernelEstimate && b1.maxFrames == 8356 && b1.maxGpuCmds == 5);
        check("clone-001: the last run was not a recovery run", !b1.recoveryLast && !b1.recoveryEver);
        check("clone-006: recovery mode, no stage 2, no kernel, nothing from a log without a clone line", b6.runs == 1 && b6.recoveryLast && b6.recoveryEver && !b6.stage2 && !b6.kernelEstimate && b6.maxFrames == 0);
        check("clone-002: no log = no runs, text says unknown, never good", b2.runs == 0 && G7CloneHistory.summary(b2).contains("미확인"));
        String s6 = G7CloneHistory.summary(b6), s1 = G7CloneHistory.summary(b1);
        check("summary names recovery for 006; kernel is called an estimate; macOS boot is 'not confirmed' for 001", s6.contains("복구 모드") && s1.contains("추정") && s1.contains("확인 안 됨"));
        // label: origin + size from the marker, date from the marker file
        Path cd = Files.createDirectories(priv.resolve("clones").resolve("clone-001"));
        Files.write(cd.resolve("clone_state.txt"), "state=clone_complete\nsource_state=in_progress\nlogical_disk_bytes=5\nallocated_bytes=1048576\nmembers=3\necid_tag=none\n".getBytes());
        Files.write(priv.resolve("clone_origins.txt"), "clone-001 from=install-pair source_state=in_progress t=1\n".getBytes());
        String lab = G7CloneHistory.label(files.toFile(), priv.toFile(), cd.toFile());
        check("label: name, made, origin, real size, boot summary", lab.startsWith("clone-001\n") && lab.contains("생성") && lab.contains("출처 설치 쌍") && lab.contains("실제") && lab.contains("저장된 부팅 로그 2회"));
        Files.delete(priv.resolve("clone_origins.txt"));
        check("label without an origin record says so", G7CloneHistory.label(files.toFile(), priv.toFile(), cd.toFile()).contains("출처 기록 없음"));
        System.out.println(failed == 0 ? "G7CLONEHISTORY OK" : "G7CLONEHISTORY FAILED " + failed);
        System.exit(failed == 0 ? 0 : 1);
    }
}
