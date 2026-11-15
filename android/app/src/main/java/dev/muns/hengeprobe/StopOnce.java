package dev.muns.hengeprobe;

import java.util.concurrent.atomic.AtomicBoolean;

/** A lock that lets exactly one caller through, ever. VmDisplayActivity has two independent paths that can decide
 *  to stop the same VM runner process at roughly the same time -- onDestroy() (the user closed the screen) and
 *  the watchdog thread (host/ui_runner_vm.c's own --seconds limit did not stop it in time) -- and neither should
 *  assume anything about which one Android happens to schedule first. tryOnce() gives exactly one caller, among
 *  any number calling concurrently or repeatedly, the single "you go" answer; every other call, before or after,
 *  gets "stand down". No Android classes are used, so tests/java/StopOnceTest.java runs on a plain JVM. */
final class StopOnce {
    private final AtomicBoolean done = new AtomicBoolean(false);

    /** @return true for exactly one call across this object's whole lifetime; false for every other one. */
    boolean tryOnce() { return done.compareAndSet(false, true); }
}
