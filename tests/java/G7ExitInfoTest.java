package dev.muns.hengeprobe;

import java.util.*;

/** G7ExitInfo on a plain JVM: pure formatting only. */
public class G7ExitInfoTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    public static void main(String[] a) {
        check("known reason names", G7ExitInfo.reasonName(3).equals("LOW_MEMORY") && G7ExitInfo.reasonName(5).equals("CRASH_NATIVE") && G7ExitInfo.reasonName(14).equals("FREEZER"));
        check("unknown reason is printed as a number", G7ExitInfo.reasonName(99).equals("REASON_99"));
        List<G7ExitInfo.Record> rs = new ArrayList<>();
        rs.add(new G7ExitInfo.Record(1234, "dev.muns.hengeprobe", 1795522141000L, 2, 9, "", 3 * 1024 * 1024L, 4 * 1024 * 1024L));
        rs.add(new G7ExitInfo.Record(1200, null, 1795500000000L, 1, 0, "stop", -1, -1));
        String s = G7ExitInfo.format(rs, 4321, 10);
        check("pid and reason shown", s.contains("pid=1234") && s.contains("SIGNALED") && s.contains("시그널 9(SIGKILL)"));
        check("PSS and RSS in MiB, unknown as ?", s.contains("PSS=3072MiB") && s.contains("RSS=4096MiB") && s.contains("PSS=?"));
        check("null process name and description handled", s.contains("pid=1200 ?") && s.contains("— stop"));
        check("current pid and the child-process caveat", s.contains("현재 앱 PID 4321") && s.contains("자식 프로세스"));
        check("max limits the lines", G7ExitInfo.format(rs, 1, 1).split("\n").length == G7ExitInfo.format(rs, 1, 2).split("\n").length - 1);
        check("empty and null lists", G7ExitInfo.format(null, 1, 5).contains("기록 없음") && G7ExitInfo.format(new ArrayList<>(), 1, 5).contains("기록 없음"));
        check("time is Asia/Seoul", G7ExitInfo.time(0L).equals("01-01 09:00:00"));
        System.out.println(fails == 0 ? "G7EXITINFO OK" : "G7EXITINFO FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
