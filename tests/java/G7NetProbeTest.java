package dev.muns.hengeprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** G7NetProbe on a plain JVM with fake processes: nothing is started, nothing is connected. */
public class G7NetProbeTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    /** A process whose output is `text` and which ends when told to (or at once when `endsAt` is 0). */
    static class Fake extends Process {
        final ByteArrayInputStream in; volatile boolean alive; volatile int code = 0; volatile boolean destroyed, forced;
        Fake(String text, boolean stayAlive) { in = new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)); alive = stayAlive; }
        public OutputStream getOutputStream() { return new ByteArrayOutputStream(); }
        public InputStream getInputStream() { return in; }
        public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
        public synchronized int waitFor() throws InterruptedException { while (alive) wait(50); return code; }
        public synchronized boolean waitFor(long t, TimeUnit u) throws InterruptedException { long end = System.currentTimeMillis() + u.toMillis(t); while (alive && System.currentTimeMillis() < end) wait(20); return !alive; }
        public int exitValue() { if (alive) throw new IllegalThreadStateException(); return code; }
        public synchronized void destroy() { destroyed = true; alive = false; code = 143; notifyAll(); }
        public synchronized Process destroyForcibly() { forced = true; alive = false; code = 137; notifyAll(); return this; }
        public boolean isAlive() { return alive; }
    }

    static void waitDone(G7NetProbe.Run r) throws Exception { for (int i = 0; i < 200 && !r.finished(); i++) Thread.sleep(20); }

    public static void main(String[] a) throws Exception {
        File files = Files.createTempDirectory("netprobe").toFile();
        File nat = Files.createTempDirectory("nat").toFile();
        G7NetProbe.Spawner never = c -> { throw new AssertionError("must not start"); };
        check("without consent nothing starts", !G7NetProbe.startWith(never, nat, files, null, false).started);
        check("without the program in the app's library directory nothing starts", !G7NetProbe.startWith(never, nat, files, null, true).started);
        File lib = new File(nat, G7NetProbe.LIB_NAME); Files.write(lib.toPath(), new byte[] {1});
        final List<List<String>> cmds = new ArrayList<>();
        G7NetProbe.Spawner ok = c -> { cmds.add(c); return new Fake("net-probe-start: x\nnet-probe: t=0s round=1 url=1 curl=0\nnet-probe-end: connections=1", false); };
        List<String> sampled = Collections.synchronizedList(new ArrayList<>());
        final AtomicInteger starts = new AtomicInteger(), stops = new AtomicInteger();
        G7NetProbe.Sampler sampler = new G7NetProbe.Sampler() {
            public void start(G7NetProbe.Sink s) { starts.incrementAndGet(); s.line("java-net: round=1 active=yes"); }
            public void stop() { stops.incrementAndGet(); }
        };
        G7NetProbe.Start s = G7NetProbe.startWith(ok, nat, files, sampler, true);
        check("with consent and the program present a run starts", s.started);
        G7NetProbe.Run r = G7NetProbe.current();
        waitDone(r);
        check("the run finishes by itself with exit code 0", r.finished() && r.exitCode() == 0 && !r.cancelled());
        check("the command is the program alone: no arguments, no options", cmds.size() == 1 && cmds.get(0).size() == 1 && cmds.get(0).get(0).endsWith(G7NetProbe.LIB_NAME));
        String text = new String(Files.readAllBytes(r.log.toPath()), StandardCharsets.UTF_8);
        check("the log holds the program's lines, the sampler's line, and a closing exit line", text.contains("net-probe: t=0s round=1 url=1 curl=0") && text.contains("java-net: round=1 active=yes") && text.contains("g7-net-exit: 0"));
        check("the sampler was started once and stopped once", starts.get() == 1 && stops.get() == 1);
        check("the log name is g7_net_<13 digits>.txt and the provider's name check accepts it", r.log.getName().matches("g7_net_[0-9]{13}\\.txt") && G7ExecLauncher.logFile(files, r.log.getName()) != null);
        check("newestLog finds it", G7NetProbe.newestLog(files) != null && G7NetProbe.newestLog(files).equals(r.log));
        check("another start after the first one finished is possible", G7NetProbe.startWith(ok, nat, files, null, true).started);
        waitDone(G7NetProbe.current());
        // cancel
        final Fake stay = new Fake("net-probe-start: y\n", true);
        G7NetProbe.GRACE_MS = 100;
        Thread.sleep(5);
        check("a run that is still going refuses a second start", G7NetProbe.startWith(c -> stay, nat, files, null, true).started && !G7NetProbe.startWith(never, nat, files, null, true).started);
        G7NetProbe.Run r2 = G7NetProbe.current();
        r2.cancel();
        waitDone(r2);
        check("cancel sends the stop (SIGTERM) and the run ends marked cancelled", stay.destroyed && r2.finished() && r2.cancelled());
        String t2 = new String(Files.readAllBytes(r2.log.toPath()), StandardCharsets.UTF_8);
        check("the closing line says cancelled", t2.contains("g7-net-exit: 143 cancelled=yes"));
        // log is never overwritten: a name collision is refused
        File[] before = files.listFiles((d, n) -> n.startsWith("g7_net_"));
        check("logs of earlier runs are all still there (nothing overwritten)", before != null && before.length >= 3);
        // the cap
        final StringBuilder big = new StringBuilder(); for (int i = 0; i < 20000; i++) big.append("net-probe: line ").append(i).append(" xxxxxxxxxxxxxxxxxxxx\n");
        Thread.sleep(3);
        G7NetProbe.startWith(c -> new Fake(big.toString(), false), nat, files, null, true);
        G7NetProbe.Run r3 = G7NetProbe.current(); waitDone(r3);
        check("the log is capped", r3.log.length() <= G7NetProbe.MAX_LOG_BYTES + 200 && r3.tail().length() <= G7NetProbe.TAIL_BYTES + 100);
        System.out.println(fails == 0 ? "G7NETPROBE OK (0 failed)" : "G7NETPROBE FAILED (" + fails + ")");
        System.exit(fails == 0 ? 0 : 1);
    }
}
