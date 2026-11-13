package dev.muns.hengeprobe;

/**
 * What the ROM-slice screen holds, and whether a result that arrives from the worker thread may still be shown. Plain Java (no Android
 * classes), so it has a JVM test.
 *
 * One request at a time is in flight (the app's "running" flag keeps a second one from starting), but the screen can be left behind
 * by it: the user edits the PC/length fields, or another run starts and takes the screen. Each begin() starts a new generation;
 * invalidate() ends the current one and wipes what was published. A worker's result is accepted only if its generation is still the
 * current one. The reason of an invalidation is kept for the request that was in flight, so the caller can tell
 *   - the user edited the fields: the request's own run is over, a one-line notice may replace the screen, and the run is the caller's to finish;
 *   - another run started: the screen and the run state belong to that run, so nothing of this request is shown or touched.
 * An "another run started" reason is never weakened by a later edit.
 *
 * Every method is synchronized, so the state change of invalidate()/begin() (generation, wipe, reason) is one step. The widgets are not
 * touched here: the caller changes them, on the UI thread only.
 */
public final class RomSliceResultSlot {
    public enum Reason { INPUT_CHANGED, OTHER_RUN_STARTED }

    public enum Publish {
        /** The result is current: show it, and the run is finished. */
        ACCEPTED,
        /** The fields were edited while it ran: drop the result; the run is this request's to finish, a notice may be shown. */
        DISCARDED_NOTICE,
        /** Another run took the screen, or a newer request exists, or this was already published: show nothing, change no run state. */
        DISCARDED_SILENT
    }

    private long gen;            // bumped by every begin() and invalidate()
    private long beganGen = -1;  // the generation begin() returned last
    private boolean active;      // a request began and has not been published or discarded yet
    private Reason reason;       // set only while that request is in flight and was invalidated
    private String screen, copy; // the published result, null when none

    /** Starts a request: wipes what was shown, forgets any earlier reason, and returns the generation the worker must publish with. */
    public synchronized long begin() {
        gen++;
        beganGen = gen;
        active = true;
        reason = null;
        screen = null;
        copy = null;
        return gen;
    }

    /**
     * Ends the current generation and wipes the published result.
     * @return true if a published result was on the screen (the caller may replace it with a notice); false otherwise
     */
    public synchronized boolean invalidate(Reason r) {
        if (r == null) throw new IllegalArgumentException("reason");
        boolean visible = screen != null;
        gen++;
        screen = null;
        copy = null;
        if (active) reason = (reason == Reason.OTHER_RUN_STARTED || r == Reason.OTHER_RUN_STARTED) ? Reason.OTHER_RUN_STARTED : Reason.INPUT_CHANGED;
        return visible;
    }

    /** The worker's result: screen text and copy text (copy may be null for a refused read). */
    public synchronized Publish publish(long g, String screenText, String copyText) {
        if (active && g == gen) {                    // while active, gen == beganGen until an invalidate() bumps it
            active = false;
            screen = screenText;
            copy = copyText;
            return Publish.ACCEPTED;
        }
        if (active && g == beganGen) {            // invalidated while it ran
            active = false;
            Reason r = reason;
            reason = null;
            return r == Reason.INPUT_CHANGED ? Publish.DISCARDED_NOTICE : Publish.DISCARDED_SILENT;
        }
        return Publish.DISCARDED_SILENT;            // superseded, never begun, or published twice
    }

    public synchronized String screen() { return screen; }

    public synchronized String copy() { return copy; }
}
