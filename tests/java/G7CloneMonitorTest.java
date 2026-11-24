package dev.muns.hengeprobe;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public class G7CloneMonitorTest {
    static int fail;
    static void check(boolean ok, String what) { if (!ok) { fail++; System.out.println("FAIL: " + what); } }

    public static void main(String[] a) throws Exception {
        G7CloneMonitor m = new G7CloneMonitor();
        long t = 1000;
        for (int i = 0; i < 500; i++) m.feed("bdif usb diag: REG_BUSY x", t);
        m.feed("g7-alive: t=30s guest_blocks=cpu0:12,cpu1:0 disk_done=77 disk_inflight=0 usb_cmds=3 progress=yes", t + 30000);
        m.feed("g7-mem: t=30s rss=812MiB peak=900MiB anon=700MiB file=1MiB swap=0MiB available=2000MiB", t + 30000);
        m.feed("hengevm: cpu 0 pc sample #5 pc=0xfffffe0010", t + 31000);
        m.feed("usb autoboot-fix: step 1 getenv auto-boot = \"false\"", t + 32000);
        m.feed("usb autoboot-fix: step 2 setenv auto-boot true sent -> 0", t + 33000);
        m.feed("usb autoboot-fix: step 3 getenv auto-boot = \"true\"", t + 34000);
        m.feed("usb autoboot-fix: step 4 saveenv sent -> 0", t + 35000);
        String s = m.render(t + 40000, true, 0, "recovery(estimate)", null, "화면 없음");
        check(m.usbDiagLines() == 500, "usb diag counted");
        check(!s.contains("REG_BUSY"), "noise not shown in lines");
        check(s.contains("500줄"), "aggregate shown");
        check(s.contains("812MiB"), "rss");
        check(s.contains("cpu0 샘플 #5"), "pc sample");
        check(!s.contains("CPU 멈춤") && !s.contains("CPU stopped"), "never says cpu stopped");
        check(s.contains("전송됨"), "saveenv sent wording");
        check(!s.contains("저장 성공") || s.contains("저장 성공이 아님"), "no saved-success claim");
        check(s.contains("미검증"), "persistence unverified");
        check(m.stepState(3) == G7CloneMonitor.SENT && m.stepState(1) == G7CloneMonitor.SENT && m.stepState(2) == G7CloneMonitor.DONE, "step states");
        // errors pinned past the ring
        G7CloneMonitor e = new G7CloneMonitor();
        e.feed("usb autoboot-fix: STOP at step 2: setenv failed", 5);
        for (int i = 0; i < 200; i++) e.feed("some line " + i, 6 + i);
        String es = e.render(300, true, 0, "x", null, "");
        check(es.contains("setenv failed") && e.errorCount() >= 1, "error pinned");
        check(!es.contains("some line 0\n"), "ring is bounded");
        // replay from a saved log: no times, ended without exit line
        G7CloneMonitor r = G7CloneMonitor.replay("g7-cboot-start: 1\nusb autoboot-fix: step 1 getenv auto-boot = \"false\"\nbdif usb diag: x\n");
        String rs = r.render(0, false, Integer.MIN_VALUE, "x", null, "");
        check(rs.contains("종료 기록 없음") && rs.contains("저장된 로그"), "replay without exit line");
        String rs2 = G7CloneMonitor.replay("g7-cboot-exit: 0\n").render(0, false, 0, "x", "v", "");
        check(rs2.contains("정상 종료"), "clean exit shown");
        // saved log works without any Run object
        File d = Files.createTempDirectory("g7m").toFile();
        check(G7CloneBoot.newestCloneLog(d) == null && G7CloneBoot.describeSaved(d).isEmpty(), "no log");
        Files.write(new File(d, "g7_cboot_1795559654770.txt").toPath(), "g7-cboot-start: 1\nusb autoboot-fix: step 1 getenv auto-boot = \"false\"\ng7-cboot-exit: 0\ng7-cboot-verdict: VM 종료 확인: 예\n".getBytes(StandardCharsets.UTF_8));
        String ds = G7CloneBoot.describeSaved(d);
        check(ds.contains("g7_cboot_1795559654770.txt") && ds.contains("정상 종료") && ds.contains("VM 종료 확인: 예"), "describeSaved from disk");
        check(G7CloneBoot.readSavedCloneLog(d).contains("auto-boot"), "readSavedCloneLog");
        System.out.println(fail == 0 ? "G7CloneMonitorTest OK" : "G7CloneMonitorTest FAILED " + fail);
        System.exit(fail == 0 ? 0 : 1);
    }
}
