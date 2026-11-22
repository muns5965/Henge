package dev.muns.hengeprobe;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/** Drains a G4 UI runner Process's combined stdout/stderr on its own thread and hands the result to
 *  VmRunnerG4Verdict.evaluate() -- plain Java, no Android classes, so tests/java/VmRunnerG4RunTest.java can
 *  drive it against real child processes (and tests/java/VmRunnerG4VerdictTest.java drives the verdict logic
 *  alone) on a plain JVM. This class creates NO process of its own: it only ever reads from and waits on a
 *  Process the caller already has (VmDisplayActivity.java's own "henge-vm-runner-out" thread is the proven,
 *  already-shipping precedent for exactly this read-to-EOF-then-waitFor() ordering never deadlocking even
 *  under full-pipe output, because the reader thread keeps consuming while the process is still writing --
 *  this class is the same shape, extracted so it is testable without Android and without the launcher's own
 *  execution switch anywhere in it: nothing here can start real execution, because nothing here starts anything).
 *
 *  Collects only the lines VmRunnerG4Verdict.isKept() wants (there is exactly one "ui_runner_vm_g4 RESULT: "
 *  line per real run; MAX_KEPT guards a runaway/duplicated stream the same way G0Verdict.MAX_KEPT does for
 *  G0), waits up to timeoutMillis for the process to end, and reports whether the reader thread itself
 *  finished (readerUnfinished) before computing the verdict -- the three inputs VmRunnerG4Verdict.evaluate()
 *  needs besides the exit code. */
final class VmRunnerG4Run {
    static final class Outcome {
        final VmRunnerG4Verdict.Result verdict;
        final boolean timedOut;
        final int exitCode;
        Outcome(VmRunnerG4Verdict.Result verdict, boolean timedOut, int exitCode) { this.verdict = verdict; this.timedOut = timedOut; this.exitCode = exitCode; }
    }

    private VmRunnerG4Run() {}

    static Outcome run(Process p, long timeoutMillis) throws InterruptedException {
        StringBuilder kept = new StringBuilder();
        int[] keptCount = {0};
        boolean[] overflow = {false};
        Thread reader = new Thread(() -> {
            try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                String ln;
                while ((ln = br.readLine()) != null) {
                    if (VmRunnerG4Verdict.isKept(ln)) {
                        if (keptCount[0] < VmRunnerG4Verdict.MAX_KEPT) { kept.append(ln).append('\n'); keptCount[0]++; }
                        else overflow[0] = true;
                    }
                }
            } catch (IOException ignored) { }
        }, "henge-vm-runner-g4-out");
        reader.start();   // started BEFORE waitFor(): a child writing more than the pipe's buffer would
                           // otherwise block forever waiting for a reader that never comes.

        boolean exited;
        try {
            exited = p.waitFor(timeoutMillis, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            reader.interrupt();
            throw e;
        }
        boolean timedOut = !exited;
        if (!exited) {
            p.destroyForcibly();
            try { p.waitFor(5, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
        }

        reader.join(5000);
        boolean readerUnfinished = reader.isAlive();

        int exitCode = safeExitValue(p);
        VmRunnerG4Verdict.Result verdict = VmRunnerG4Verdict.evaluate(kept.toString(), timedOut, exitCode, overflow[0], readerUnfinished);
        return new Outcome(verdict, timedOut, exitCode);
    }

    private static int safeExitValue(Process p) {
        try { return p.exitValue(); } catch (IllegalThreadStateException stillRunning) { return -1; }
    }
}
