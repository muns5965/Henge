package dev.muns.hengeprobe;

public class G7InputMapTest {
    static int fail;
    static void check(boolean ok, String what) { if (!ok) { fail++; System.out.println("FAIL: " + what); } else System.out.println("PASS: " + what); }
    static boolean at(int[] p, int x, int y, int tol) { return p != null && Math.abs(p[0] - x) <= tol && Math.abs(p[1] - y) <= tol; }

    public static void main(String[] a) {
        // a 1080-wide portrait view that is 900 high, showing the 96x54 preview of a 1920x1080 guest: the picture is 1080 x 607.5, margin 146.25 above and below
        int vw = 1080, vh = 900, sw = 96, sh = 54;
        check(at(G7InputMap.previewToGuest(540, 450, vw, vh, sw, sh, 1920, 1080), 960, 540, 1), "centre -> guest centre");
        check(G7InputMap.previewToGuest(540, 100, vw, vh, sw, sh, 1920, 1080) == null, "a touch in the top margin is not a guest position");
        check(G7InputMap.previewToGuest(540, 800, vw, vh, sw, sh, 1920, 1080) == null, "a touch in the bottom margin is not a guest position");
        check(at(G7InputMap.previewToGuest(0, 146.25f, vw, vh, sw, sh, 1920, 1080), 0, 0, 0), "the picture's top-left corner -> 0,0");
        check(at(G7InputMap.previewToGuest(1079.9f, 753.7f, vw, vh, sw, sh, 1920, 1080), 1919, 1079, 1), "the picture's bottom-right corner -> 1919,1079");
        // a wide, short view (landscape): pillarboxed left and right
        int[] p = G7InputMap.previewToGuest(1200 + 0.5f * 0, 300, 2400, 600, sw, sh, 1920, 1080);
        check(at(p, 960, 540, 1), "wide view: its middle is the guest middle");
        check(G7InputMap.previewToGuest(100, 300, 2400, 600, sw, sh, 1920, 1080) == null, "wide view: the side margin is outside");
        // the presets map back through a preview the same size as the guest: touching the Restart position gives the Restart position
        check(at(G7InputMap.previewToGuest(G7InputMap.RESTART_X + 0.5f, G7InputMap.RESTART_Y + 0.5f, 1920, 1080, 1920, 1080, 1920, 1080), 1190, 550, 0), "Restart preset round trip");
        // rotation: the same touch fraction after the view changes shape gives the same guest point
        int[] p1 = G7InputMap.previewToGuest(270, 146.25f + 151.875f, 1080, 900, sw, sh, 1920, 1080);
        int[] p2 = G7InputMap.previewToGuest(480, 270, 1920, 1080, sw, sh, 1920, 1080);
        check(p1 != null && p2 != null && Math.abs(p1[0] - p2[0]) <= 1 && Math.abs(p1[1] - p2[1]) <= 1, "a quarter point is the same guest point in both view shapes");
        check(G7InputMap.previewToGuest(1, 1, 0, 10, sw, sh, 1920, 1080) == null && G7InputMap.previewToGuest(1, 1, 10, 10, 0, sh, 1920, 1080) == null, "bad sizes give null");
        // commands
        check(G7InputMap.allowed(G7InputMap.key("shift+tab")) && G7InputMap.allowed(G7InputMap.key("enter")) && G7InputMap.allowed(G7InputMap.click(960, 397)) && G7InputMap.allowed(G7InputMap.dclick(1190, 550))
            && G7InputMap.allowed("inject click") && G7InputMap.allowed("inject dclick") && G7InputMap.allowed("inject abort") && G7InputMap.allowed(G7InputMap.display(1920, 1080)), "the command builders make lines the runner accepts");
        check(!G7InputMap.allowed("quit") && !G7InputMap.allowed("inject key enter\nquit") && !G7InputMap.allowed("inject key ../x") && !G7InputMap.allowed("inject click 1") && !G7InputMap.allowed(null) && !G7InputMap.allowed("inject key a+b+c+d+e"), "anything else is refused (no quit, no newline, no path)");
        // the run object: what it writes to the runner and what it keeps as the trail
        final java.io.ByteArrayOutputStream sink = new java.io.ByteArrayOutputStream();
        Process fake = new Process() {
            public java.io.OutputStream getOutputStream() { return sink; }
            public java.io.InputStream getInputStream() { return new java.io.ByteArrayInputStream(new byte[0]); }
            public java.io.InputStream getErrorStream() { return new java.io.ByteArrayInputStream(new byte[0]); }
            public int waitFor() { return 0; }
            public int exitValue() { return 0; }
            public void destroy() { }
        };
        G7CloneBoot.Run run = new G7CloneBoot.Run(fake, "x.txt");
        check(run.sendInject("inject key enter") == null && sink.toString().equals("inject key enter\n"), "an allowed line is written once, with a newline");
        check(run.sendInject("quit") != null && run.sendInject("inject key enter\nquit") != null && sink.toString().equals("inject key enter\n"), "a refused line writes nothing");
        check(run.injectTail().contains("app: sent to the runner's input: 'inject key enter'") && run.injectTail().contains("refused"), "the trail records the app's steps");
        run.injectNote("INJECT: start: key enter (guest pointer feedback known) | kbd reports delivered-to-guest-buffer 0 | guest consumption: unknown");
        check(run.injectTail().endsWith("guest consumption: unknown\n"), "the runner's lines are kept after the app's");
        System.out.println(fail == 0 ? "PASS G7InputMapTest" : "FAIL G7InputMapTest");
        System.exit(fail == 0 ? 0 : 1);
    }
}
