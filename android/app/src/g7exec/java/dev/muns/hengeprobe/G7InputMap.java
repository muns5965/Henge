package dev.muns.hengeprobe;

/**
 * TEMPORARY guest-input test helpers (research/g7-input). Pure Java, no Android classes: the transform from a touch on the preview picture to guest pixels, the text commands the runner
 * understands (host/input_inject.h), and the two screen-specific presets.
 *
 * THE PRESETS ARE FOR ONE SCREEN ONLY: the "Choose Startup Disk" window of macOS 15.6.1 as the PVG surface showed it at 1920x1080 (g7_frame_pvg_surface_last_t143s_1920x1080.png, one Macintosh HD
 * entry, Restart... greyed until the entry is selected). The icon's pixels measured 919..1000 x 356..438 (centre 960,397); the Restart button's text 1158..1224 x 540..554 inside a button about 1154..1228 x 538..562
 * (centre 1190,550). Another window, another macOS version or another resolution moves them.
 */
public final class G7InputMap {
    private G7InputMap() { }
    public static final int GUEST_W = 1920, GUEST_H = 1080;
    public static final int HD_X = 960, HD_Y = 397;
    public static final int RESTART_X = 1190, RESTART_Y = 550;

    /**
     * A touch at (tx, ty) in a view of viewW x viewH that shows a picture of srcW x srcH scaled to fit and centred (ImageView FIT_CENTER: the aspect ratio is kept and the rest is margin),
     * to guest pixels of a guestW x guestH picture. Returns {x, y}, or null when the touch is in the margin (or the numbers cannot be). The picture's own pixel size is only its aspect
     * ratio here: the guest picture may be bigger than what is shown.
     */
    public static int[] previewToGuest(float tx, float ty, int viewW, int viewH, int srcW, int srcH, int guestW, int guestH) {
        if (viewW <= 0 || viewH <= 0 || srcW <= 0 || srcH <= 0 || guestW <= 0 || guestH <= 0) return null;
        double scale = Math.min((double) viewW / srcW, (double) viewH / srcH);
        double cw = srcW * scale, ch = srcH * scale, left = (viewW - cw) / 2.0, top = (viewH - ch) / 2.0;
        if (tx < left || ty < top || tx >= left + cw || ty >= top + ch) return null;
        int gx = (int) ((tx - left) / cw * guestW), gy = (int) ((ty - top) / ch * guestH);
        return new int[] { Math.min(gx, guestW - 1), Math.min(gy, guestH - 1) };
    }

    public static String key(String name) { return "inject key " + name; }
    public static String click(int x, int y) { return "inject click " + x + " " + y; }
    public static String dclick(int x, int y) { return "inject dclick " + x + " " + y; }
    public static String display(int w, int h) { return "inject display " + w + " " + h; }

    /** The only lines the runner is sent: anything else is refused here, before it reaches the process. */
    private static final java.util.regex.Pattern OK = java.util.regex.Pattern.compile(
        "^inject (key [a-z]{1,8}(\\+[a-z]{1,8}){0,3}|click( [0-9]{1,5} [0-9]{1,5})?|dclick( [0-9]{1,5} [0-9]{1,5})?|display [0-9]{2,5} [0-9]{2,5}|abort)$");
    public static boolean allowed(String line) { return line != null && OK.matcher(line).matches(); }
}
