package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * [G7-EXEC] The app side of the network probe (host/net_probe.c as libnetprobe.so). It only runs that one program: name lookup, TCP connect and TLS handshake
 * against the three built-in TSS addresses, nothing written to the server, no VM, no disk, no restore tool. Independent of a restore run: it may be started while one is running
 * (that is the point of comparing the two). One NEW log per probe run (g7_net_&lt;13 digits&gt;.txt, CREATE_NEW: never appended to or overwritten). The program's own output goes
 * to the log as it is; a {@link Sampler} (the Android network state and the Java name lookups, G7NetState) writes its lines into the same log. Plain Java: no Android class here,
 * so a plain JVM test runs it with a fake process. The consent is a checked box passed as a boolean; no Intent input is read; nothing is typed.
 */
final class G7NetProbe {
    static final String LIB_NAME = "libnetprobe.so";
    static final String LOG_PREFIX = "g7_net_";
    static final String LOG_SUFFIX = ".txt";
    static final int MAX_LOG_BYTES = 256 * 1024;
    static final int TAIL_BYTES = 6000;
    static final int MAX_RUN_SECONDS = 330;   // the program ends itself after about 4.5 minutes; this is only the outer bound
    static volatile int GRACE_MS = 5000;      // SIGTERM, then this long, then a hard stop (the program reacts within a second)

    interface Spawner { Process spawn(List<String> command) throws IOException; }
    interface Sink { void line(String text); }
    /** Writes its own lines (through the sink) while the probe runs; both methods return promptly. */
    interface Sampler { void start(Sink sink); void stop(); }

    private static final Spawner REAL = command -> new ProcessBuilder(command).redirectErrorStream(true).start();
    private static volatile Run CURRENT;

    static Run current() { return CURRENT; }

    static final class Run {
        final File log;
        private final Process process;
        private final Sampler sampler;
        private final OutputStream out;
        private final StringBuilder tail = new StringBuilder();
        private long written;
        private volatile boolean finished;
        private volatile int exit = Integer.MIN_VALUE;
        private volatile boolean cancelled;

        private Run(File log, Process process, Sampler sampler, OutputStream out) { this.log = log; this.process = process; this.sampler = sampler; this.out = out; }

        synchronized void line(String text) {
            String l = text.endsWith("\n") ? text : text + "\n";
            byte[] b = l.getBytes(StandardCharsets.UTF_8);
            if (written + b.length <= MAX_LOG_BYTES) {
                try { out.write(b); out.flush(); written += b.length; } catch (IOException ignored) { }
            }
            tail.append(l);
            if (tail.length() > TAIL_BYTES) tail.delete(0, tail.length() - TAIL_BYTES);
        }
        synchronized String tail() { return tail.toString(); }
        boolean finished() { return finished; }
        int exitCode() { return exit; }
        boolean cancelled() { return cancelled; }

        /** SIGTERM now (the program ends its run at the next second and prints its summary); a hard stop only if it does not. */
        void cancel() {
            if (finished) return;
            cancelled = true;
            process.destroy();
            Thread t = new Thread(() -> {
                try { Thread.sleep(GRACE_MS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return; }
                if (process.isAlive()) process.destroyForcibly();
            });
            t.setDaemon(true);
            t.start();
        }
    }

    /** Result of start(): whether a probe run began, and what to tell the person. */
    static final class Start {
        final boolean started; final String message;
        Start(boolean started, String message) { this.started = started; this.message = message; }
    }

    static Start start(File nativeDir, File filesDir, Sampler sampler, boolean consent) { return startWith(REAL, nativeDir, filesDir, sampler, consent); }

    static synchronized Start startWith(Spawner spawner, File nativeDir, File filesDir, Sampler sampler, boolean consent) {
        if (!consent) return new Start(false, "시작하지 않았습니다: 동의 칸을 체크해야 합니다.");
        Run live = CURRENT;
        if (live != null && !live.finished()) return new Start(false, "이미 네트워크 시험이 실행 중입니다.");
        File lib = new File(nativeDir, LIB_NAME);
        if (!lib.isFile()) return new Start(false, "이 빌드에는 네트워크 시험 프로그램(" + LIB_NAME + ")이 없습니다.");
        File log = new File(filesDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + LOG_SUFFIX);
        OutputStream out;
        try { out = Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE); }
        catch (IOException e) { return new Start(false, "로그 파일을 만들 수 없어 시작하지 않았습니다."); }
        Process p;
        try { p = spawner.spawn(new ArrayList<>(Arrays.asList(lib.getAbsolutePath()))); }
        catch (IOException | RuntimeException e) { try { out.close(); } catch (IOException ignored) { } return new Start(false, "네트워크 시험 프로그램을 시작하지 못했습니다."); }
        final Run run = new Run(log, p, sampler, out);
        CURRENT = run;
        run.line("g7-net-start: " + System.currentTimeMillis());
        if (sampler != null) {
            try { sampler.start(run::line); } catch (RuntimeException e) { run.line("java-net: sampler did not start (" + e.getClass().getSimpleName() + ")"); }
        }
        final Thread reader = new Thread(() -> {
            try (InputStream in = p.getInputStream()) {
                byte[] buf = new byte[4096];
                StringBuilder cur = new StringBuilder();
                int n;
                while ((n = in.read(buf)) > 0) {
                    cur.append(new String(buf, 0, n, StandardCharsets.UTF_8));
                    int nl;
                    while ((nl = cur.indexOf("\n")) >= 0) { run.line(cur.substring(0, nl)); cur.delete(0, nl + 1); }
                }
                if (cur.length() > 0) run.line(cur.toString());
            } catch (IOException ignored) { }
        });
        reader.setDaemon(true);
        reader.start();
        Thread watcher = new Thread(() -> {
            int code = -1;
            try {
                if (!p.waitFor(MAX_RUN_SECONDS, java.util.concurrent.TimeUnit.SECONDS)) { run.line("g7-net: outer time bound reached; stopping"); run.cancel(); p.waitFor(GRACE_MS + 2000L, java.util.concurrent.TimeUnit.MILLISECONDS); }
                if (p.isAlive()) p.destroyForcibly();
                code = p.waitFor();
            } catch (InterruptedException e) { Thread.currentThread().interrupt(); p.destroyForcibly(); }
            try { reader.join(2000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            if (sampler != null) { try { sampler.stop(); } catch (RuntimeException ignored) { } }
            run.line("g7-net-exit: " + code + (run.cancelled() ? " cancelled=yes" : ""));
            run.exit = code;
            run.finished = true;
            try { run.out.close(); } catch (IOException ignored) { }
        });
        watcher.setDaemon(true);
        watcher.start();
        return new Start(true, "네트워크 시험을 시작했습니다(최대 약 5분). 요청 본문은 보내지 않습니다.");
    }

    static File newestLog(File filesDir) {
        File[] l = filesDir.listFiles((d, n) -> n.startsWith(LOG_PREFIX) && n.endsWith(LOG_SUFFIX));
        if (l == null || l.length == 0) return null;
        Arrays.sort(l, (a, b) -> a.getName().compareTo(b.getName()));
        return l[l.length - 1];
    }
}
