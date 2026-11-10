package dev.muns.hengeprobe;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;

/**
 * Runs libipswdl.so (host/ipsw_dl.c) as a separate process. All the download logic (Range, ETag, resume, SHA-256, free space, locking) is in that program; this class
 * only starts it, forwards its "dl: " lines and sends it SIGTERM on cancel. It holds no reference to an Activity, so a foreground service can run the same class later.
 */
final class IpswDownload {
    interface Listener {
        void onProgress(long done, long total);
        void onLine(String line);
    }

    /** Result of a run: the program's exit code (0 done, 2 refused, 3 state, 4 server, 5 interrupted, 6 hash mismatch, 7 paused, 8 stopped on request) and its last RESULT text. */
    static final class Result {
        int code = -1;
        String text = "";
        String detail = "";   // the "dl: STATUS ..." line of status()
    }

    private final File exe;
    private final File dir;
    private volatile Process proc;

    IpswDownload(File exe, File dir) {
        this.exe = exe;
        this.dir = dir;
    }

    /** Blocking; call from a worker thread. stopAfter = 0: download to the end (--full); otherwise pause at that many bytes. */
    Result run(long stopAfter, Listener l) {
        List<String> cmd = new ArrayList<>();
        cmd.add(exe.getAbsolutePath());
        cmd.add("--dir"); cmd.add(dir.getAbsolutePath());
        if (stopAfter > 0) { cmd.add("--stop-after"); cmd.add(Long.toString(stopAfter)); }
        else cmd.add("--full");   // to the end; the program then checks the SHA-256 and installs the file only if it matches
        return exec(cmd, l);
    }

    Result status() {
        List<String> cmd = new ArrayList<>();
        cmd.add(exe.getAbsolutePath()); cmd.add("--status"); cmd.add("--dir"); cmd.add(dir.getAbsolutePath());
        return exec(cmd, null);
    }

    Result deletePartial() {
        List<String> cmd = new ArrayList<>();
        cmd.add(exe.getAbsolutePath()); cmd.add("--delete-partial"); cmd.add("--dir"); cmd.add(dir.getAbsolutePath());
        return exec(cmd, null);
    }

    /** SIGTERM: the program flushes its state and exits with 5; a kill would also leave a consistent state, but this is the clean way. */
    void cancel() {
        Process p = proc;
        if (p == null) return;
        try {
            // Process.pid() exists from API 33 (looked up by name: the build's language level does not see it)
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                long pid = (Long) Process.class.getMethod("pid").invoke(p);
                android.system.Os.kill((int) pid, android.system.OsConstants.SIGTERM);
            } else p.destroy();
        } catch (Throwable t) {
            p.destroy();
        }
    }

    private Result exec(List<String> cmd, Listener l) {
        Result r = new Result();
        try {
            dir.mkdirs();
            Process p = new ProcessBuilder(cmd).directory(dir).redirectErrorStream(true).start();
            proc = p;
            try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String line;
                while ((line = br.readLine()) != null) {
                    if (line.startsWith("dl: PROGRESS ")) {
                        String[] f = line.substring(13).trim().split(" ");
                        if (f.length == 2 && l != null) {
                            try { l.onProgress(Long.parseLong(f[0]), Long.parseLong(f[1])); } catch (NumberFormatException ignored) { }
                        }
                    } else if (line.startsWith("dl: RESULT ")) {
                        String rest = line.substring(11);
                        int sp = rest.indexOf(' ');
                        try { r.code = Integer.parseInt(sp < 0 ? rest : rest.substring(0, sp)); } catch (NumberFormatException ignored) { }
                        r.text = sp < 0 ? "" : rest.substring(sp + 1);
                    }
                    if (l != null) l.onLine(line);
                    if (line.startsWith("dl: STATUS")) r.detail = line.substring(4);
                }
            }
            int rc = p.waitFor();
            if (r.code < 0) { r.code = rc; if (r.text.isEmpty()) r.text = "프로그램이 결과 줄 없이 끝났습니다 (종료 코드 " + rc + ")"; }
        } catch (Exception e) {
            r.code = -1;
            r.text = "실행 오류: " + e;
        } finally {
            proc = null;
        }
        return r;
    }
}
