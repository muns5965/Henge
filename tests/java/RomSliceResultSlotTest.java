package dev.muns.hengeprobe;

import dev.muns.hengeprobe.RomSliceResultSlot.Publish;
import dev.muns.hengeprobe.RomSliceResultSlot.Reason;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Plain-java test of RomSliceResultSlot (no framework):
 *   javac -encoding UTF-8 -d out android/app/src/main/java/dev/muns/hengeprobe/RomSliceResultSlot.java tests/java/RomSliceResultSlotTest.java && java -cp out dev.muns.hengeprobe.RomSliceResultSlotTest
 * It checks the state rules only. That the widgets are touched on the UI thread, and the screen/buttons/clipboard themselves, are not covered here.
 */
public class RomSliceResultSlotTest {
    static int fails;
    static void check(String name, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + name); if (!ok) fails++; }

    public static void main(String[] a) throws Exception {
        // --- the plain path ---
        RomSliceResultSlot s = new RomSliceResultSlot();
        long g = s.begin();
        check("nothing is shown before a result", s.screen() == null && s.copy() == null);
        check("begin -> publish: accepted, screen and copy kept", s.publish(g, "SCREEN", "COPY") == Publish.ACCEPTED && "SCREEN".equals(s.screen()) && "COPY".equals(s.copy()));
        check("publishing the same generation again changes nothing", s.publish(g, "OTHER", "OTHER") == Publish.DISCARDED_SILENT && "SCREEN".equals(s.screen()) && "COPY".equals(s.copy()));
        s = new RomSliceResultSlot();
        g = s.begin();
        check("a refused read has no copy text", s.publish(g, "REFUSED", null) == Publish.ACCEPTED && "REFUSED".equals(s.screen()) && s.copy() == null);
        check("publish with a generation that never began is silent", new RomSliceResultSlot().publish(0, "x", "y") == Publish.DISCARDED_SILENT
                && new RomSliceResultSlot().publish(5, "x", "y") == Publish.DISCARDED_SILENT);

        // --- the fields were edited while it ran ---
        s = new RomSliceResultSlot();
        g = s.begin();
        check("invalidate with nothing shown reports false", !s.invalidate(Reason.INPUT_CHANGED));
        check("edited in flight: the result is dropped, with a notice (the run is this request's to finish)", s.publish(g, "S", "C") == Publish.DISCARDED_NOTICE);
        check("a dropped result is neither shown nor copyable", s.screen() == null && s.copy() == null);
        check("a second publish of the dropped request is silent", s.publish(g, "S", "C") == Publish.DISCARDED_SILENT && s.screen() == null);

        // --- another run took the screen ---
        s = new RomSliceResultSlot();
        g = s.begin();
        s.invalidate(Reason.OTHER_RUN_STARTED);
        check("another run started in flight: silent, nothing shown, run state not this request's", s.publish(g, "S", "C") == Publish.DISCARDED_SILENT && s.screen() == null && s.copy() == null);

        // --- the reasons: "another run" is never weakened ---
        s = new RomSliceResultSlot();
        g = s.begin();
        s.invalidate(Reason.INPUT_CHANGED);
        s.invalidate(Reason.OTHER_RUN_STARTED);
        check("edit, then another run: silent", s.publish(g, "S", "C") == Publish.DISCARDED_SILENT);
        s = new RomSliceResultSlot();
        g = s.begin();
        s.invalidate(Reason.OTHER_RUN_STARTED);
        s.invalidate(Reason.INPUT_CHANGED);
        check("another run, then an edit: still silent (the other run still owns the screen)", s.publish(g, "S", "C") == Publish.DISCARDED_SILENT);
        s = new RomSliceResultSlot();
        g = s.begin();
        s.invalidate(Reason.INPUT_CHANGED);
        s.invalidate(Reason.INPUT_CHANGED);
        check("two edits: still a notice", s.publish(g, "S", "C") == Publish.DISCARDED_NOTICE);

        // --- a new request does not inherit the last one's reason ---
        s = new RomSliceResultSlot();
        long g1 = s.begin();
        s.invalidate(Reason.OTHER_RUN_STARTED);
        s.publish(g1, "old", "old");
        long g2 = s.begin();
        check("a new request after 'another run' starts clean and is accepted", s.publish(g2, "NEW", "NEWC") == Publish.ACCEPTED && "NEW".equals(s.screen()));
        s = new RomSliceResultSlot();
        g1 = s.begin();
        s.invalidate(Reason.OTHER_RUN_STARTED);
        g2 = s.begin();                                 // begins before the old one finished
        s.invalidate(Reason.INPUT_CHANGED);
        check("the reason of a superseded request does not leak into the new one (edit only: a notice)", s.publish(g2, "x", "y") == Publish.DISCARDED_NOTICE);

        // --- completion order reversed: a late old request cannot overwrite the new one ---
        s = new RomSliceResultSlot();
        g1 = s.begin();
        g2 = s.begin();
        check("the newer request is accepted", s.publish(g2, "NEW", "NEWC") == Publish.ACCEPTED);
        check("the older request, finishing later, is silent and changes nothing", s.publish(g1, "OLD", "OLDC") == Publish.DISCARDED_SILENT && "NEW".equals(s.screen()) && "NEWC".equals(s.copy()));
        s = new RomSliceResultSlot();
        g1 = s.begin();
        g2 = s.begin();
        check("the older request finishing first is silent, and does not stop the newer one", s.publish(g1, "OLD", "OLDC") == Publish.DISCARDED_SILENT && s.screen() == null
                && s.publish(g2, "NEW", "NEWC") == Publish.ACCEPTED && "NEW".equals(s.screen()));

        // --- a result that is on the screen, and then the fields are edited ---
        s = new RomSliceResultSlot();
        g = s.begin();
        s.publish(g, "S", "C");
        check("an edit after the result: reports that a result was visible", s.invalidate(Reason.INPUT_CHANGED));
        check("... and the shown result and its copy text are gone", s.screen() == null && s.copy() == null);
        check("a further edit has no result to replace (no second notice)", !s.invalidate(Reason.INPUT_CHANGED));
        s = new RomSliceResultSlot();
        g = s.begin();
        s.publish(g, "S", "C");
        s.invalidate(Reason.OTHER_RUN_STARTED);
        check("another run took the screen: the result is gone", s.screen() == null && s.copy() == null);
        check("... so a later edit does not claim the screen (no notice)", !s.invalidate(Reason.INPUT_CHANGED));
        check("invalidate refuses a null reason", throwsIae(new RomSliceResultSlot()));

        // --- the order the app uses when it starts a run: invalidate (setButtons(false)), then begin ---
        s = new RomSliceResultSlot();
        s.invalidate(Reason.OTHER_RUN_STARTED);
        g = s.begin();
        check("invalidate, then begin: the request is accepted", s.publish(g, "S", "C") == Publish.ACCEPTED);
        s = new RomSliceResultSlot();
        g = s.begin();
        s.invalidate(Reason.OTHER_RUN_STARTED);
        check("begin, then invalidate (the wrong order): the request is lost", s.publish(g, "S", "C") == Publish.DISCARDED_SILENT);

        // --- a previous result is wiped when a request begins ---
        s = new RomSliceResultSlot();
        g = s.begin();
        s.publish(g, "S1", "C1");
        s.begin();
        check("begin wipes the earlier result", s.screen() == null && s.copy() == null);

        // --- hand-offs between threads (auxiliary: the ordering is forced, so this is not a count of luck) ---
        final int N = 3000;
        final AtomicInteger leaked = new AtomicInteger(), notice = new AtomicInteger();
        for (int i = 0; i < N; i++) {
            final RomSliceResultSlot slot = new RomSliceResultSlot();
            final long gen = slot.begin();
            final CountDownLatch invalidated = new CountDownLatch(1);
            Thread worker = new Thread(() -> {
                try { invalidated.await(); } catch (InterruptedException e) { return; }
                Publish p = slot.publish(gen, "S", "C");
                if (p == Publish.ACCEPTED) leaked.incrementAndGet();
                if (p == Publish.DISCARDED_NOTICE) notice.incrementAndGet();
            });
            worker.start();
            slot.invalidate(Reason.INPUT_CHANGED);           // finishes before the latch opens
            invalidated.countDown();
            worker.join();
            if (slot.screen() != null || slot.copy() != null) leaked.incrementAndGet();
        }
        check("an invalidation that completed before a publish started is never overridden (" + N + " hand-offs)", leaked.get() == 0 && notice.get() == N);

        // a publish and an invalidate racing: whichever order wins, nothing stays visible after the invalidate
        final AtomicInteger visibleAfter = new AtomicInteger(), acc = new AtomicInteger(), dis = new AtomicInteger();
        for (int i = 0; i < N; i++) {
            final RomSliceResultSlot slot = new RomSliceResultSlot();
            final long gen = slot.begin();
            final CountDownLatch go = new CountDownLatch(1);
            Thread worker = new Thread(() -> {
                try { go.await(); } catch (InterruptedException e) { return; }
                if (slot.publish(gen, "S", "C") == Publish.ACCEPTED) acc.incrementAndGet(); else dis.incrementAndGet();
            });
            worker.start();
            go.countDown();
            slot.invalidate(Reason.INPUT_CHANGED);
            worker.join();
            if (slot.screen() != null || slot.copy() != null) visibleAfter.incrementAndGet();
        }
        check("racing publish and invalidate: nothing is visible after the invalidate, either order", visibleAfter.get() == 0 && acc.get() + dis.get() == N);
        System.out.println("   (racing run: accepted-then-wiped " + acc.get() + ", discarded " + dis.get() + ")");

        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }

    static boolean throwsIae(RomSliceResultSlot s) {
        try { s.invalidate(null); return false; } catch (IllegalArgumentException e) { return true; }
    }
}
