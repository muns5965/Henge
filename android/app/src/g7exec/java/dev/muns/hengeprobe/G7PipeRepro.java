package dev.muns.hengeprobe;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * [G7-EXEC] GPU pipeline reproduction WITHOUT a virtual machine. A failed vkCreateGraphicsPipelines leaves a bundle folder (pipefail_<hash>, written by libapplegfx-vulkan inside the GPU-dump run
 * folder when "셰이더·파이프라인 저장" is on). libg7pipelinerepro.so (examples/pipeline-repro.c of the library, linked statically with it) rebuilds exactly that pipeline on this phone's Vulkan device
 * and prints a report; with matrix=true it also runs the library's DIAGNOSTIC variant matrix (each variant starts from the original; nothing here is ever applied to a real run).
 * Starts one child process, reads its output (bounded), writes it to g7_pipelinerepro_<ms>.txt in the app's files folder. Touches no clone, no ROM, no disk image.
 */
final class G7PipeRepro {
    static final String LIB_NAME = "libg7pipelinerepro.so";
    static final String LOG_PREFIX = "g7_pipelinerepro_";
    static final int MAX_OUT = 512 * 1024;
    static final long TIMEOUT_S = 180;

    private G7PipeRepro() {}

    static boolean isPackaged(File nativeLibDir) { return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile(); }

    /** All pipefail_* bundle folders under dumpRoot/run-*, newest first (by the run folder name, then the bundle name). */
    static List<File> listBundles(File dumpRoot) {
        List<File> out = new ArrayList<>();
        File[] runs = dumpRoot == null ? null : dumpRoot.listFiles();
        if (runs == null) return out;
        Arrays.sort(runs, Comparator.comparing(File::getName).reversed());
        for (File r : runs) {
            if (!r.isDirectory() || !r.getName().startsWith("run-")) continue;
            File[] b = r.listFiles();
            if (b == null) continue;
            Arrays.sort(b, Comparator.comparing(File::getName));
            for (File d : b) if (d.isDirectory() && d.getName().startsWith("pipefail_") && new File(d, "state").isFile()) out.add(d);
        }
        return out;
    }

    static final class Result { final boolean started; final int exitCode; final String text; final File file; Result(boolean s, int c, String t, File f) { started = s; exitCode = c; text = t; file = f; } }

    /** Runs the repro on one bundle. Blocking: call from a worker thread. */
    static Result run(File nativeLibDir, File bundle, boolean matrix, File logDir) {
        if (!isPackaged(nativeLibDir)) return new Result(false, -1, "재현 실행 파일(" + LIB_NAME + ")이 이 APK에 없습니다.", null);
        if (bundle == null || !bundle.isDirectory()) return new Result(false, -1, "번들 폴더가 없습니다.", null);
        List<String> cmd = new ArrayList<>();
        cmd.add(new File(nativeLibDir, LIB_NAME).getPath());
        cmd.add(bundle.getPath());
        if (matrix) cmd.add("--matrix");
        StringBuilder head = new StringBuilder();
        head.append("g7-pipelinerepro: bundle=").append(bundle.getName()).append(" run=").append(bundle.getParentFile() == null ? "?" : bundle.getParentFile().getName())
            .append(" matrix=").append(matrix).append(" henge=").append(G7ExecLauncher.HENGE_SHA).append(" lagfx=").append(G7ExecLauncher.LAGFX_SHA).append("\n");
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        int code = -1; String note = "";
        try {
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            try (InputStream in = p.getInputStream()) {
                byte[] tmp = new byte[8192]; int n; boolean cut = false;
                while ((n = in.read(tmp)) > 0) { if (buf.size() < MAX_OUT) buf.write(tmp, 0, Math.min(n, MAX_OUT - buf.size())); else cut = true; }
                if (cut) note = "\n[출력이 " + MAX_OUT + " 바이트에서 잘렸습니다]\n";
            }
            if (!p.waitFor(TIMEOUT_S, TimeUnit.SECONDS)) { p.destroyForcibly(); note += "\n[시간 제한 " + TIMEOUT_S + "초: 강제 종료]\n"; code = -2; }
            else code = p.exitValue();
        } catch (IOException | InterruptedException e) {
            return new Result(false, -1, head + "실행하지 못했습니다: " + e, null);
        }
        String verdict = code == 0 ? "재현됨(기록된 실패가 같은 결과로 다시 발생)" : code == 1 ? "재현되지 않음(생성 성공 또는 다른 결과)" : code == 2 ? "재현 도구 오류(사용법·번들·Vulkan)" : code == -2 ? "시간 초과" : "비정상 종료(코드 " + code + ", 신호 종료는 128 이상일 수 있음)";
        String text = head + "g7-pipelinerepro: exit=" + code + " -> " + verdict + "\n" + new String(buf.toByteArray(), StandardCharsets.UTF_8) + note;
        File f = null;
        try {
            f = new File(logDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + ".txt");
            java.nio.file.Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8), java.nio.file.StandardOpenOption.CREATE_NEW);
        } catch (IOException e) { f = null; text += "\n[결과 파일을 저장하지 못했습니다: " + e + "]\n"; }
        return new Result(true, code, text, f);
    }
}
