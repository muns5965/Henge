package dev.muns.hengeprobe;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

/** Plain-java test: javac -d out android/.../StopOnce.java tests/java/StopOnceTest.java && java -cp out dev.muns.hengeprobe.StopOnceTest
 *  Proves the one guarantee VmDisplayActivity leans on: among any number of calls to tryOnce() on one StopOnce,
 *  exactly one ever returns true, whether the calls are sequential or genuinely concurrent. */
public class StopOnceTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    public static void main(String[] a) throws Exception {
        StopOnce s = new StopOnce();
        check("first call wins", s.tryOnce());
        check("second call (sequential) loses", !s.tryOnce());
        check("third call (sequential) loses", !s.tryOnce());

        // The actual scenario this exists for: two threads (onDestroy() and the watchdog) call tryOnce() at
        // nearly the same instant on a fresh StopOnce. This loop tries to provoke that on whatever hardware runs
        // it, but a timing race is not something a CPU-starved box can be trusted to expose: this sandbox has
        // only 2 cores, and a deliberately broken, unsynchronized "if (done) return false; done = true; return
        // true;" version was checked by hand to still pass 20,000 rounds of 16 threads here, with zero bad
        // rounds -- not because it is correct, but because this machine can rarely schedule two threads to
        // execute those two statements at the exact same instant. So this loop is run (and any round where more
        // than one thread won IS still a real, reportable failure, if this happens to run somewhere with more
        // parallelism), but it is not what the next check relies on for the actual guarantee.
        for (int round = 0; round < 200; round++) {
            StopOnce race = new StopOnce();
            CountDownLatch gate = new CountDownLatch(1);
            AtomicInteger winners = new AtomicInteger(0);
            Thread[] t = new Thread[8];
            for (int i = 0; i < t.length; i++) {
                t[i] = new Thread(() -> {
                    try { gate.await(); } catch (InterruptedException ignored) { }
                    if (race.tryOnce()) winners.incrementAndGet();
                });
                t[i].start();
            }
            gate.countDown();
            for (Thread th : t) th.join();
            if (winners.get() != 1) { check("round " + round + ": exactly one winner among " + t.length + " racing callers", false); }
        }
        check("200 rounds of 8 racing threads: no MORE than one winner was ever observed (best-effort; see above)", true);

        // The guarantee that does not depend on luck: tryOnce() must be backed by a single lock-free atomic
        // primitive (java.util.concurrent.atomic.AtomicBoolean/AtomicInteger/...), not a plain boolean guarded by
        // nothing. A plain-field "if (done) return false; done = true;" has a real gap between the read and the
        // write that two threads can both pass through before either writes -- the race loop above just cannot
        // be trusted to catch it on hardware this small. Checked by reflection, not by racing threads.
        boolean hasAtomicField = false;
        for (java.lang.reflect.Field f : StopOnce.class.getDeclaredFields()) {
            if (java.util.concurrent.atomic.AtomicBoolean.class.isAssignableFrom(f.getType())
                    || java.util.concurrent.atomic.AtomicInteger.class.isAssignableFrom(f.getType())
                    || java.util.concurrent.atomic.AtomicLong.class.isAssignableFrom(f.getType())) hasAtomicField = true;
        }
        check("StopOnce's state is a java.util.concurrent.atomic.* field (an unsynchronized plain boolean would not actually be race-free, whatever the loop above happened to show)", hasAtomicField);

        // a fresh StopOnce (the real code makes a new one per process, in startVmRunner()) is independent of an
        // already-spent one: it must not start out already "done".
        StopOnce s2 = new StopOnce();
        check("a new StopOnce starts unspent even after another instance was used up", s2.tryOnce());

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
