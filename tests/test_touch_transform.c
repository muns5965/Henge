/* Android view coordinates -> guest pixels (host/touch_input.c), with a letterboxed view rectangle, a rotated screen, and the order of the events a tap and a drag produce. */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../host/touch_input.h"

static int bad;
#define EXPECT(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)
static int gx, gy, gw = 1920, gh = 1080; static char ev[512]; static int ne;
static void mv(int dx, int dy) { gx += dx; gy += dy; if (gx < 0) gx = 0; if (gy < 0) gy = 0; if (gx > gw - 1) gx = gw - 1; if (gy > gh - 1) gy = gh - 1; }
static void bt(int b, int d) { ne += snprintf(ev + ne, sizeof ev - (size_t)ne, "%c%d;", d ? 'D' : 'U', b); }
static HvInputSink SK = {NULL, mv, bt, NULL, NULL};
static void settle(HvTouch *t, int64_t *now) { for (int i = 0; i < 300; i++) { hv_touch_pointer(t, 1, gx, gy); hv_touch_tick(t, *now); *now += 15; } }
static void touch_at(HvTouch *t, float x, float y, int64_t *now) { hv_touch_event(t, HV_TOUCH_DOWN, 0, x, y, *now); settle(t, now); }
static void lift(HvTouch *t, float x, float y, int64_t *now) { hv_touch_event(t, HV_TOUCH_UP, 0, x, y, *now); settle(t, now); }

int main(void) {
    HvTouch t; int64_t now = 1000;
    /* portrait phone 1080x2400, the SurfaceView letterboxed to the 16:9 picture: 1080 x 607.5, centred vertically (the view rectangle the touch module is given is the SurfaceView's own: 0,0,w,h) */
    hv_touch_init(&t, &SK); hv_touch_set_display(&t, 1920, 1080); hv_touch_set_view(&t, 0, 0, 1080, 607.5f);
    touch_at(&t, 540, 303.75f, &now); EXPECT(abs(gx - 960) <= 3 && abs(gy - 540) <= 3, "centre of the letterboxed view -> centre of the guest (%d,%d)", gx, gy);
    lift(&t, 540, 303.75f, &now); ne = 0; ev[0] = 0;
    touch_at(&t, 1080, 607.5f, &now); EXPECT(abs(gx - 1919) <= 3 && abs(gy - 1079) <= 3, "bottom-right of the view -> guest corner (%d,%d)", gx, gy);
    lift(&t, 1080, 607.5f, &now);
    /* the same picture shown inside a full-screen view with margins (vx, vy != 0): the picture is at y 896.25..1503.75 of 2400 */
    hv_touch_init(&t, &SK); hv_touch_set_display(&t, 1920, 1080); hv_touch_set_view(&t, 0, 896.25f, 1080, 607.5f); gx = gy = 0; now = 5000;
    touch_at(&t, 270, 896.25f + 151.875f, &now); EXPECT(abs(gx - 480) <= 3 && abs(gy - 270) <= 3, "view with a top margin: a quarter point maps to 480,270 (%d,%d)", gx, gy);
    lift(&t, 270, 896.25f + 151.875f, &now);
    touch_at(&t, 100, 10, &now); EXPECT(gy <= 2, "a touch in the top margin is clamped to the first row (%d)", gy);
    lift(&t, 100, 10, &now);
    /* rotation to landscape 2400x1080 view of 1920x1080 (2400 x 1350 would be 16:9; here fit inside: 1920x1080 centred): view = 1920x1080 at x 240 */
    hv_touch_init(&t, &SK); hv_touch_set_display(&t, 1920, 1080); hv_touch_set_view(&t, 240, 0, 1920, 1080); gx = gy = 0; now = 9000;
    touch_at(&t, 240 + 1190, 550, &now); EXPECT(abs(gx - 1190) <= 3 && abs(gy - 550) <= 3, "landscape with side margins: Restart position (1190,550) is hit (%d,%d)", gx, gy);
    lift(&t, 240 + 1190, 550, &now);
    /* rotation again: the same activity gets a new size; the view is re-set and a standing target is NOT silently kept in the old frame */
    hv_touch_set_view(&t, 0, 0, 1080, 607.5f); gx = gy = 0; now += 1000;
    touch_at(&t, 540, 303.75f, &now); EXPECT(abs(gx - 960) <= 3 && abs(gy - 540) <= 3, "after rotating to portrait the centre is again the guest centre (%d,%d)", gx, gy);
    lift(&t, 540, 303.75f, &now);
    /* tap: move first, then button down, then up; a drag: down, then moves, then up */
    hv_touch_init(&t, &SK); hv_touch_set_display(&t, 1920, 1080); hv_touch_set_view(&t, 0, 0, 1920, 1080); gx = gy = 0; now = 20000; ne = 0; ev[0] = 0;
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 960, 397, now); now += 100; hv_touch_event(&t, HV_TOUCH_UP, 0, 960, 397, now); settle(&t, &now);
    EXPECT(!strcmp(ev, "D0;U0;") && abs(gx - 960) <= 3 && abs(gy - 397) <= 3, "tap: pointer got there, then one left down and one left up (%s) at %d,%d", ev, gx, gy);
    ne = 0; ev[0] = 0; now += 1000;
    hv_touch_event(&t, HV_TOUCH_DOWN, 0, 500, 500, now); now += 100; hv_touch_event(&t, HV_TOUCH_UP, 0, 500, 500, now); for (int i = 0; i < 40; i++) { hv_touch_pointer(&t, 1, gx, gy); hv_touch_tick(&t, now); now += 5; }
    now += 50; hv_touch_event(&t, HV_TOUCH_DOWN, 0, 500, 500, now); for (int i = 0; i < 4; i++) { hv_touch_pointer(&t, 1, gx, gy); hv_touch_tick(&t, now); now += 5; }
    hv_touch_event(&t, HV_TOUCH_MOVE, 0, 700, 600, now); settle(&t, &now); hv_touch_event(&t, HV_TOUCH_UP, 0, 700, 600, now); settle(&t, &now);
    EXPECT(!strcmp(ev, "D0;U0;D0;U0;") && abs(gx - 700) <= 3 && abs(gy - 600) <= 3, "tap then tap-and-drag: down/up, then down ... up with the pointer carried to 700,600 (%s) at %d,%d", ev, gx, gy);
    printf("%s test_touch_transform\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
