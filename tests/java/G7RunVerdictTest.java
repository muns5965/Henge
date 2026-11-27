package dev.muns.hengeprobe;

/** G7RunVerdict: the VERDICT line of the clone runner, old formats and the NEW optional trailing root/AUX disk counters. Pure Java. */
public class G7RunVerdictTest {
    static int fail;
    static void check(boolean ok, String what) { System.out.println((ok ? "PASS " : "FAIL ") + what); if (!ok) fail++; }

    static final String HEAD = "ui_runner_vm_clone VERDICT: vm_stop_confirmed=1 stop_kind=guest-initiated disk_flush=ok macos_boot=not-determined";
    static final String STRUCT = HEAD + " outcome=restart psci=SYSTEM_RESET quiesced=1 uptime_s=42 disk_writes=17 disk_write_refused=2";
    static final String SPLIT = " root_writes=10 root_write_bytes=40960 root_write_refused=1 root_write_failed=0 root_flush=ok root_flush_calls=1 root_flush_failed=0"
        + " aux_writes=7 aux_write_bytes=3584 aux_write_refused=1 aux_write_failed=3 aux_flush=skipped aux_flush_calls=2 aux_flush_failed=0";
    static final String TAIL = " (a confirmed stop says only that the machine ended)";

    public static void main(String[] a) {
        // 1. the oldest line (no structured fields)
        G7RunVerdict o1 = G7RunVerdict.parse(HEAD + TAIL);
        check(o1 != null && !o1.structured && o1.root == null && o1.aux == null && o1.diskSplitText().isEmpty() && o1.diskWrites == -1, "oldest line: parsed, no split counters");
        // 2. the structured line of the previous runner (the exact old format): parses unchanged, split counters absent
        G7RunVerdict o2 = G7RunVerdict.parse(STRUCT + TAIL);
        check(o2 != null && o2.structured && o2.isRestart() && o2.verifiedQuiesced() && o2.diskWrites == 17 && o2.diskWriteRefused == 2 && o2.uptimeS == 42 && o2.root == null && o2.aux == null, "previous runner's line: all old fields as before, root/aux null");
        // 3. the new line: old fields identical, split counters present
        G7RunVerdict n = G7RunVerdict.parse(STRUCT + SPLIT + TAIL);
        check(n != null && n.structured && n.isRestart() && n.diskWrites == 17 && n.diskWriteRefused == 2 && n.uptimeS == 42 && n.psci.equals("SYSTEM_RESET"), "new line: old fields unchanged");
        check(n.root != null && n.root.writes == 10 && n.root.writeBytes == 40960 && n.root.refused == 1 && n.root.failed == 0 && n.root.flush.equals("ok") && n.root.flushCalls == 1 && n.root.flushFailed == 0, "new line: root counters");
        check(n.aux != null && n.aux.writes == 7 && n.aux.writeBytes == 3584 && n.aux.refused == 1 && n.aux.failed == 3 && n.aux.flush.equals("skipped") && n.aux.flushCalls == 2 && n.aux.flushFailed == 0, "new line: AUX counters (separate from root)");
        String t = n.diskSplitText();
        check(t.contains("root: writes 10") && t.contains("AUX: writes 7") && t.contains("do not show that the guest's boot device persisted"), "log text names root and AUX apart and does not claim persistence");
        // 4. the new line without the trailing text, and with a trailing text that itself looks like fields
        check(G7RunVerdict.parse(STRUCT + SPLIT) != null && G7RunVerdict.parse(STRUCT + SPLIT).aux.writes == 7, "new line without the explanatory text");
        // 5. an incomplete split block is NOT half-read: the old fields still parse, the split block is ignored
        String half = STRUCT + " root_writes=10 root_write_bytes=40960" + TAIL;
        G7RunVerdict h = G7RunVerdict.parse(half);
        check(h != null && h.structured && h.diskWrites == 17 && h.root == null && h.aux == null, "a truncated split block: old fields kept, no partial counters");
        // 6. unknown flush word / junk: split ignored, never guessed
        G7RunVerdict j = G7RunVerdict.parse(STRUCT + SPLIT.replace("root_flush=ok", "root_flush=maybe") + TAIL);
        check(j != null && j.root == null && j.aux == null && j.structured, "unknown flush word: split block ignored");
        // 7. all four flush words
        for (String w : new String[]{"ok", "FAILED", "skipped", "never"}) {
            G7RunVerdict x = G7RunVerdict.parse(STRUCT + SPLIT.replace("aux_flush=skipped", "aux_flush=" + w) + TAIL);
            check(x != null && x.aux != null && x.aux.flush.equals(w), "aux_flush=" + w);
        }
        // 8. non-VERDICT input
        check(G7RunVerdict.parse(null) == null && G7RunVerdict.parse("root_writes=1") == null, "junk is not interpreted");
        // 9. the split counters do not change the restart decision
        check(G7RunVerdict.parse(STRUCT.replace("quiesced=1", "quiesced=0") + SPLIT + TAIL).verifiedQuiesced() == false && n.cleanStop(), "restart/clean-stop logic ignores the counters");
        // 10. the REAL format string of host/ui_runner_vm_clone.c (run from the repository root), filled with values: the parser must read what the runner prints
        try {
            String src = new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get("host/ui_runner_vm_clone.c")), java.nio.charset.StandardCharsets.UTF_8);
            int i = src.indexOf("printf(\"ui_runner_vm_clone VERDICT:"); int e = src.indexOf("\",", i);
            String fmt = src.substring(i + 8, e).replace("\\n", "");
            String[] vals = {"guest-initiated", "ok", "restart", "SYSTEM_RESET", "1", "42", "17", "2", "10", "4096", "1", "0", "ok", "1", "0", "7", "512", "1", "3", "skipped", "2", "0"};
            fmt = fmt.replaceFirst("%d", "1");
            StringBuilder sb = new StringBuilder(); int vi = 0; java.util.regex.Matcher pm = java.util.regex.Pattern.compile("%llu|%d|%s").matcher(fmt); int last = 0;
            while (pm.find()) { sb.append(fmt, last, pm.start()).append(vals[vi++]); last = pm.end(); }
            sb.append(fmt.substring(last));
            G7RunVerdict r = G7RunVerdict.parse(sb.toString());
            check(vi == vals.length && r != null && r.isRestart() && r.diskWrites == 17 && r.root != null && r.root.writeBytes == 4096 && r.aux != null && r.aux.failed == 3 && r.aux.flush.equals("skipped"), "the runner's real VERDICT format (" + vi + " fields) parses, root and AUX apart");
            check(sb.toString().contains("do not prove that the guest boot device"), "the runner's text says the counts do not prove persistence");
        } catch (java.io.IOException ex) { System.out.println("SKIP runner source not readable from the current directory: " + ex); }
        System.out.println(fail == 0 ? "G7RUNVERDICT OK" : "G7RUNVERDICT FAILED");
        System.exit(fail == 0 ? 0 : 1);
    }
}
