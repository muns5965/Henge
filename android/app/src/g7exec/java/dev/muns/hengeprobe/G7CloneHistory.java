package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * [G7-EXEC] What is KNOWN about a clone, for the pick lists: when it was made, from what, how much real space it uses, and what the saved boot logs (files/g7_cboot_NNNN.txt) say about boots of THAT clone.
 * Read only, derived from files that already exist; nothing new is stored and the marker format is unchanged. A clone with no saved log is shown as "boot history unknown", never as good.
 * "Kernel" is the CPU-sample ESTIMATE of G7BootStage, said so in the text. Passing the structure check (validate()) is NOT a successful boot and is not shown as one. No Android classes.
 */
public final class G7CloneHistory {
    private G7CloneHistory() {}
    static final Pattern CLONE = Pattern.compile("clones/(clone-[0-9]{3}) verified");
    static final Pattern FRAMES = Pattern.compile("frames published ([0-9]+)");
    static final Pattern POPPED = Pattern.compile("cmds popped ([0-9]+)");
    static final Pattern RECOVERY = Pattern.compile("iBoot[A-Za-z0-9]* recovery mode");
    static final Pattern EXIT = Pattern.compile("g7-cboot-exit: (\\S+)");
    static final Pattern FLUSH = Pattern.compile("disk_flush=(\\S+)");
    static final Pattern STOP = Pattern.compile("stop_kind=(\\S+)");
    static final Pattern MACOS = Pattern.compile("macos_boot=(\\S+)");
    static final long MAX_LOG = 8L << 20;

    /** Facts from the saved boot logs of one clone. runs = 0 means there is no log at all. */
    public static final class Boots {
        public int runs; public long lastMs; public boolean stage2, kernelEstimate, recoveryEver, recoveryLast;
        public long maxFrames, maxGpuCmds; public String lastExit = "?", lastFlush = "?", lastStop = "?", macosBoot = "?";
    }

    public static Boots bootsOf(File filesDir, String cloneName) {
        Boots b = new Boots();
        File[] l = filesDir == null ? null : filesDir.listFiles((d, n) -> n.matches("g7_cboot_[0-9]{13}\\.txt"));
        if (l == null) return b;
        Arrays.sort(l);
        for (File f : l) {
            String t;
            try { if (f.length() > MAX_LOG) continue; t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException | RuntimeException e) { continue; }
            Matcher c = CLONE.matcher(t);
            if (!c.find() || !c.group(1).equals(cloneName)) continue;
            b.runs++;
            try { b.lastMs = Long.parseLong(f.getName().substring(9, 22)); } catch (NumberFormatException ignored) { }
            if (t.contains("iBootStage2")) b.stage2 = true;
            if (t.contains("g7-stage: kernel first seen")) b.kernelEstimate = true;
            boolean rec = RECOVERY.matcher(t).find();
            b.recoveryLast = rec; if (rec) b.recoveryEver = true;
            Matcher m = FRAMES.matcher(t); while (m.find()) b.maxFrames = Math.max(b.maxFrames, parse(m.group(1)));
            m = POPPED.matcher(t); while (m.find()) b.maxGpuCmds = Math.max(b.maxGpuCmds, parse(m.group(1)));
            b.lastExit = last(EXIT, t, b.lastExit); b.lastFlush = last(FLUSH, t, "?"); b.lastStop = last(STOP, t, "?"); b.macosBoot = last(MACOS, t, "?");
        }
        return b;
    }

    private static long parse(String s) { try { return Long.parseLong(s); } catch (NumberFormatException e) { return 0; } }
    private static String last(Pattern p, String t, String dflt) { Matcher m = p.matcher(t); String r = dflt; while (m.find()) r = m.group(1); return r; }

    /** The text for a pick list: line 1 = name, made when, from what, real size; line 2 = what is known of its boots. */
    public static String label(File filesDir, File priv, File cloneDir) {
        String name = cloneDir.getName();
        StringBuilder sb = new StringBuilder(name);
        File mk = new File(cloneDir, G7Clone.MARKER);
        long made = mk.lastModified();
        String from = G7Clone.originOf(priv, name);
        long alloc = allocatedOf(mk);
        sb.append("\n  ").append(made > 0 ? fmtDate(made) + " 생성" : "생성 시각 모름");
        sb.append(", 출처 ").append(from == null ? "기록 없음" : from.equals("install-pair") ? "설치 쌍" : from);
        if (alloc >= 0) sb.append(", 실제 ").append(G7Storage.fmt(alloc));
        sb.append("\n  ").append(summary(bootsOf(filesDir, name)));
        return sb.toString();
    }

    public static String summary(Boots b) {
        if (b.runs == 0) return "부팅 기록 없음(이 앱에 저장된 로그 기준) — 정상 여부 미확인";
        StringBuilder sb = new StringBuilder("저장된 부팅 로그 " + b.runs + "회(마지막 " + fmtDate(b.lastMs) + "): ");
        sb.append(b.recoveryLast ? "마지막에 iBoot 복구 모드" : "마지막은 복구 모드 아님");
        if (b.recoveryEver && !b.recoveryLast) sb.append("(이전에 복구 모드 있음)");
        sb.append(b.stage2 ? ", iBootStage2 도달" : ", iBootStage2 미관측");
        sb.append(b.kernelEstimate ? ", 커널 진입(CPU 샘플 추정)" : ", 커널 진입 미관측");
        sb.append(", 화면 프레임 최대 ").append(b.maxFrames).append(", GPU 명령 최대 ").append(b.maxGpuCmds);
        sb.append(", 마지막 종료 ").append(b.lastStop).append("/디스크 flush ").append(b.lastFlush);
        sb.append(" — macOS 정상 부팅: ").append("not-determined".equals(b.macosBoot) || "?".equals(b.macosBoot) ? "확인 안 됨" : b.macosBoot);
        return sb.toString();
    }

    static long allocatedOf(File marker) {
        try {
            for (String l : new String(Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8).split("\n")) if (l.startsWith("allocated_bytes=")) return Long.parseLong(l.substring(16));
        } catch (IOException | RuntimeException e) { /* unknown */ }
        return -1;
    }

    static String fmtDate(long ms) { return new SimpleDateFormat("MM-dd HH:mm", Locale.KOREA).format(new Date(ms)); }
}
