/* The touch target is in the guest's own pixels (the pointer feedback from the display page is). android/app/src/main/cpp/ui_jni.c used to set 1280x720 once and never
   change it, whatever the picture the guest showed; hv_touch_follow_display is what it now calls with the real size. A guest pointer is simulated (1:1, clamped to the
   real display) and a finger is put on the bottom-right of the view: where does the pointer end up? */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../host/touch_input.h"

static int gw = 1920, gh = 1080, gx, gy;
static void mv(int dx, int dy) { gx += dx; gy += dy; if (gx < 0) gx = 0; if (gy < 0) gy = 0; if (gx > gw - 1) gx = gw - 1; if (gy > gh - 1) gy = gh - 1; }
static int bad;
#define EXPECT(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

static void settle(HvTouch *t, int64_t *now) {
    for (int i = 0; i < 400; i++) { hv_touch_pointer(t, 1, gx, gy); hv_touch_tick(t, *now); *now += 15; }
}
/* a finger pressed at the view's (fx, fy) fraction; returns with the pointer settled */
static void press(HvTouch *t, double fx, double fy, int64_t *now) {
    hv_touch_event(t, HV_TOUCH_DOWN, 0, (float)(fx * 2400.0), (float)(fy * 1080.0), *now);
    settle(t, now);
}

int main(void) {
    HvInputSink sk = {0};
    sk.mouse_move = mv;
    HvTouch t; int64_t now = 1000;
    /* what ui_jni.c did: 1280x720 for a guest that shows 1920x1080 */
    hv_touch_init(&t, &sk); hv_touch_set_view(&t, 0, 0, 2400, 1080); hv_touch_set_display(&t, 1280, 720);
    gx = gy = 960; gw = 1920; gh = 1080;
    press(&t, 1.0, 1.0, &now);
    EXPECT(abs(gx - 1279) <= 2 && abs(gy - 719) <= 2, "old behaviour (display left at 1280x720): a finger on the bottom-right corner leaves the pointer at %d,%d, not at the corner 1919,1079", gx, gy);
    /* the same run, but the size of the shown picture is followed */
    hv_touch_init(&t, &sk); hv_touch_set_view(&t, 0, 0, 2400, 1080); hv_touch_set_display(&t, 1280, 720);
    EXPECT(hv_touch_follow_display(&t, 1920, 1080) == 1, "follow: a different size is taken");
    EXPECT(hv_touch_follow_display(&t, 1920, 1080) == 0, "follow: the same size again changes nothing");
    EXPECT(hv_touch_follow_display(&t, 0, 1080) == 0 && hv_touch_follow_display(&t, 1920, -1) == 0, "follow: a size that cannot be is refused");
    gx = gy = 0; now = 5000;
    press(&t, 1.0, 1.0, &now);
    EXPECT(abs(gx - 1919) <= 2 && abs(gy - 1079) <= 2, "followed: the bottom-right corner of the view reaches the corner of the guest display (%d,%d)", gx, gy);
    hv_touch_event(&t, HV_TOUCH_UP, 0, 2400.0f, 1080.0f, now); now += 600;
    press(&t, 0.5, 0.5, &now);
    EXPECT(abs(gx - 960) <= 1 && abs(gy - 540) <= 1, "followed: the middle of the view reaches the middle of the guest display (%d,%d)", gx, gy);
    /* a target that is already set is converted again when the size changes */
    hv_touch_init(&t, &sk); hv_touch_set_view(&t, 0, 0, 2400, 1080); hv_touch_set_display(&t, 1280, 720);
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 2400.0f, 1080.0f, 9000);
    HvTouchDiag d; hv_touch_diag(&t, &d); int seq = d.tgt_seq;
    EXPECT(d.has_target && d.target_x > 1278.9 && d.target_x < 1279.1 && d.target_y > 718.9, "target before the size is known: %.1f,%.1f", d.target_x, d.target_y);
    hv_touch_follow_display(&t, 1920, 1080); hv_touch_diag(&t, &d);
    EXPECT(d.target_x > 1918.9 && d.target_x < 1919.1 && d.target_y > 1078.9 && d.target_y < 1079.1 && d.tgt_seq == seq, "the standing target is re-converted (%.1f,%.1f) and is not counted as a new one", d.target_x, d.target_y);
    printf("%s test_touch_display\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
