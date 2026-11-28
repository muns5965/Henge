package dev.muns.hengeprobe;

public class G7ProgressTest {
    static int bad;
    static void ok(boolean c, String m) { System.out.println((c ? "PASS " : "FAIL ") + m); if (!c) bad++; }
    public static void main(String[] a) {
        G7Progress p = new G7Progress();
        ok(p.banner(5000).isEmpty(), "nothing before begin()");
        p.begin(1000);
        ok(p.banner(66000).contains("대기 중") && p.banner(66000).contains("01:05"), "before the first step line: waiting, with the elapsed time: " + p.banner(66000));
        ok(p.feed("g7-phase: name=ipsw-sha256 secs=16.0 bytes=16800000000 mb_per_s=1001.4", 17000) && p.banner(18000).contains("ipsw-sha256 16.0초 (1001.4 MB/s)"), "a phase line is shown with its MB/s");
        ok(p.feed("g7-step: step=2 name=UPLOAD_FS pct=37 t=100.0", 100000), "step line accepted");
        String b = p.banner(100000 + 398000);
        ok(b.contains("파일시스템 전송 (UPLOAD_FS) 37% (이 단계 기준)") && b.contains("이 단계 06:38") && b.contains("전체 경과 08:17"), "step, percent of the step, step time and total time: " + b);
        ok(p.feed("g7-step: step=2 name=UPLOAD_FS pct=40 t=130.0", 130000) && p.banner(130000 + 1000).contains("40%") && p.banner(131000).contains("이 단계 00:31"), "same step, a new percent does not restart the step clock");
        ok(p.feed("g7-step: step=3 name=VERIFY_FS pct=0 t=500.0 prev=UPLOAD_FS prev_secs=400.0", 500000) && p.banner(501000).contains("파일시스템 검증") && p.banner(501000).contains("이 단계 00:01"), "a new step restarts the step clock");
        p.feed("g7-steptimes: PREPARE=7.0 UPLOAD_FS=401.0", 600000);
        ok(p.banner(601000).contains("단계별 소요(초): PREPARE=7.0 UPLOAD_FS=401.0"), "the closing times are shown");
        ok(!p.feed("g7: something else", 1) && !p.feed("g7-result: x", 1), "other lines are not progress lines");
        ok(p.feed("g7-step: step=2 pct=oops", 1), "a malformed step line is swallowed without a crash");
        ok(G7Progress.clock(3_725_000).equals("1:02:05") && G7Progress.clock(-5).equals("00:00"), "clock format");
        System.out.println(bad == 0 ? "ALL PASS" : "FAIL: " + bad);
        System.exit(bad == 0 ? 0 : 1);
    }
}
