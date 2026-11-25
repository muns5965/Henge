package dev.muns.hengeprobe;

public class G7BootStageTest {
    static int fails;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }
    public static void main(String[] a) {
        check("kernel address at EL1", G7BootStage.of("hengevm: cpu 0 pc sample #10400001 pc=0xfffffe0012567080 el=1") == G7BootStage.Stage.KERNEL);
        check("EL0 is user space whatever the address", G7BootStage.of("hengevm: cpu 1 pc sample #5 pc=0x0000000100003f00 el=0") == G7BootStage.Stage.USER);
        check("a low address at EL1 is firmware", G7BootStage.of("hengevm: cpu 0 pc sample #1 pc=0x0000000800001000 el=1") == G7BootStage.Stage.FIRMWARE);
        check("EL2 low address is firmware", G7BootStage.of("hengevm: cpu 0 pc sample #1 pc=0x100 el=2") == G7BootStage.Stage.FIRMWARE);
        check("other lines are not samples", G7BootStage.of("ui_runner_vm_clone: started") == null && G7BootStage.of("hengevm: cpu 0 pc sample #x pc=zz el=1") == null);
        G7BootStage.Tracker t = new G7BootStage.Tracker();
        check("first sign of a stage is reported once", t.feed("hengevm: cpu 0 pc sample #1 pc=0x800 el=1", 1000) == G7BootStage.Stage.FIRMWARE && t.feed("hengevm: cpu 0 pc sample #2 pc=0x900 el=1", 2000) == null);
        check("kernel then user", t.feed("hengevm: cpu 0 pc sample #3 pc=0xfffffe0000001000 el=1", 40000) == G7BootStage.Stage.KERNEL && t.feed("hengevm: cpu 1 pc sample #4 pc=0x1000 el=0", 90000) == G7BootStage.Stage.USER);
        check("frames published 0 is not a screen", t.feed("ui_runner_vm_clone status: pid 1 | frames published 0 (pvg 0)", 95000) == null && !t.seen(G7BootStage.Stage.SCREEN));
        check("frames published > 0 is the screen, once", t.feed("ui_runner_vm_clone status: pid 1 | frames published 12 (pvg 12)", 100000) == G7BootStage.Stage.SCREEN && t.feed("frames published 40", 101000) == null);
        String s = t.summary();
        check("the summary has the times and says it is an estimate", s.contains("펌웨어 1초") && s.contains("커널 40초") && s.contains("사용자 공간 90초") && s.contains("화면 출력 100초") && s.contains("확정이 아님"));
        check("a tracker that saw nothing says so", new G7BootStage.Tracker().summary().contains("아직 보이지 않음"));
        G7BootStage.Tracker r = new G7BootStage.Tracker();
        check("recovery mode line is its own stage, once", r.feed("Entering iBootStage1 recovery mode, starting command prompt", 5000) == G7BootStage.Stage.RECOVERY && r.feed("Entering iBootStage1 recovery mode, starting command prompt", 6000) == null && r.seen(G7BootStage.Stage.RECOVERY));
        check("EL0 after recovery is firmware, not user space", r.feed("hengevm: cpu 0 pc sample #200001 pc=0x7006c524 el=0 x30=0x70079a10", 205000) == G7BootStage.Stage.FIRMWARE && !r.seen(G7BootStage.Stage.USER));
        check("summary says recovery", r.summary().contains("복구 모드") && r.summary().contains("사용자 공간 아직 보이지 않음"));
        G7BootStage.Tracker u = new G7BootStage.Tracker();
        check("without recovery EL0 is still user space", u.feed("hengevm: cpu 0 pc sample #1 pc=0x1000 el=0", 1000) == G7BootStage.Stage.USER);
        System.out.println(fails == 0 ? "G7BOOTSTAGE OK" : "G7BOOTSTAGE FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
