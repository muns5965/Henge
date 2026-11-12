#include "touch_input.h"

#include <math.h>
#include <string.h>

#define NO_FB_WAIT_MS 50   /* with feedback: do not send another step until the pointer moved or this long passed */

void hv_touch_init(HvTouch *t, const HvInputSink *sink) {
    memset(t, 0, sizeof *t);
    if (sink) t->sink = *sink;
    t->vw = t->vh = 1; t->dw = 1920; t->dh = 1080;
    t->ev_last_action = -1;
}

void hv_touch_set_view(HvTouch *t, float x, float y, float w, float h) { t->vx = x; t->vy = y; t->vw = w > 1 ? w : 1; t->vh = h > 1 ? h : 1; }
void hv_touch_set_display(HvTouch *t, int w, int h) { if (w > 0 && h > 0) { t->dw = w; t->dh = h; } }

void hv_touch_pointer(HvTouch *t, int known, int x, int y) {
    if (known) {
        if (!t->known || x != t->last_fb_x || y != t->last_fb_y) t->fb_changed = 1;
        t->last_fb_x = x; t->last_fb_y = y;
        t->px = x; t->py = y;
    }
    t->known = known;
}

static void map_to_target(HvTouch *t, float x, float y) {
    double fx = (x - t->vx) / t->vw, fy = (y - t->vy) / t->vh;
    fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
    fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
    t->tx = fx * (t->dw - 1);
    t->ty = fy * (t->dh - 1);
    t->has_target = 1;
}

static int step_clamp(int v) { return v > HV_TOUCH_STEP_MAX ? HV_TOUCH_STEP_MAX : v < -HV_TOUCH_STEP_MAX ? -HV_TOUCH_STEP_MAX : v; }

static double est_x(const HvTouch *t) { return t->known ? t->px : t->ex; }
static double est_y(const HvTouch *t) { return t->known ? t->py : t->ey; }

static int arrived(const HvTouch *t) {
    return t->has_target && fabs(t->tx - est_x(t)) <= 1.5 && fabs(t->ty - est_y(t)) <= 1.5 && (t->known || t->homed);
}

static void send_move(HvTouch *t, int dx, int dy) {
    if (!dx && !dy) return;
    if (t->sink.mouse_move) t->sink.mouse_move(dx, dy);
    if (!t->known) { t->ex += dx; t->ey += dy; }
}

static void click(HvTouch *t, int button) {
    if (t->sink.mouse_button) { t->sink.mouse_button(button, 1); t->sink.mouse_button(button, 0); }
}

static void begin_click(HvTouch *t, int button, int64_t now) {
    if (arrived(t)) { click(t, button); t->pending_btn = 0; }
    else { t->pending_btn = button + 1; t->pending_ms = now; }
}

static void reset_gesture(HvTouch *t) {
    t->moved = t->long_fired = t->multi = t->scrolled = t->ignore_rest = 0;
    t->scroll_acc_x = t->scroll_acc_y = 0; t->multi_travel = 0;
}

static void release_button(HvTouch *t) {
    if (t->button_held) { if (t->sink.mouse_button) t->sink.mouse_button(0, 0); t->button_held = 0; }
}

void hv_touch_release_all(HvTouch *t) {
    release_button(t);
    t->nptr = 0; t->pending_btn = 0; t->drag = 0; t->has_target = 0;
    reset_gesture(t);
}

static int find(HvTouch *t, int id) { for (int i = 0; i < t->nptr; i++) if (t->p[i].id == id) return i; return -1; }

static void avg(const HvTouch *t, float *x, float *y) { *x = (t->p[0].x + t->p[1].x) / 2; *y = (t->p[0].y + t->p[1].y) / 2; }

void hv_touch_event(HvTouch *t, int action, int id, float x, float y, int64_t now) {
    t->ev_count++; t->ev_last_action = action; t->ev_last_x = x; t->ev_last_y = y; t->ev_last_ms = now;
    if (action == HV_TOUCH_CANCEL) { hv_touch_release_all(t); return; }
    int i = find(t, id);
    if (action == HV_TOUCH_DOWN) {
        if (i >= 0 || t->nptr >= 2) return;         /* a third finger is ignored */
        t->p[t->nptr].id = id; t->p[t->nptr].x = x; t->p[t->nptr].y = y; t->nptr++;
        if (t->nptr == 1) {
            reset_gesture(t);
            t->down_ms = now; t->down_x = x; t->down_y = y;
            map_to_target(t, x, y);
            int again = t->last_tap_valid && now - t->last_tap_ms <= HV_TOUCH_DOUBLE_MS &&
                        hypotf(x - t->last_tap_x, y - t->last_tap_y) <= HV_TOUCH_DOUBLE_DIST_PX;
            t->last_tap_valid = 0;
            if (again) {                              /* tap, then touch and hold: a drag with the left button down */
                t->drag = 1;
                if (t->sink.mouse_button) { t->sink.mouse_button(0, 1); t->button_held = 1; }
            }
        } else {                                      /* the second finger */
            if (t->drag) { t->nptr = 1; return; }      /* ignored while a drag is held */
            t->multi = 1; t->multi_down_ms = now; t->pending_btn = 0;
            avg(t, &t->scroll_ax, &t->scroll_ay);
        }
        return;
    }
    if (i < 0) return;
    if (action == HV_TOUCH_MOVE) {
        float ox = t->p[i].x, oy = t->p[i].y;
        t->p[i].x = x; t->p[i].y = y;
        if (t->multi && t->nptr == 2) {
            float ax, ay; avg(t, &ax, &ay);
            float dx = ax - t->scroll_ax, dy = ay - t->scroll_ay;
            t->scroll_ax = ax; t->scroll_ay = ay;
            t->multi_travel += fabsf(dx) + fabsf(dy);
            t->scroll_acc_y += dy; t->scroll_acc_x += dx;
            /* natural: fingers down = content down = wheel away from the user (positive) */
            int ny = (int)(t->scroll_acc_y / HV_TOUCH_SCROLL_PX), nx = (int)(t->scroll_acc_x / HV_TOUCH_SCROLL_PX);
            if (ny) { t->scroll_acc_y -= ny * HV_TOUCH_SCROLL_PX; if (t->sink.mouse_wheel) t->sink.mouse_wheel(ny); t->scrolled = 1; }
            if (nx) { t->scroll_acc_x -= nx * HV_TOUCH_SCROLL_PX; if (t->sink.mouse_hwheel) t->sink.mouse_hwheel(-nx); t->scrolled = 1; }
            (void)ox; (void)oy;
            return;
        }
        if (t->nptr == 1 && !t->ignore_rest) {
            if (hypotf(x - t->down_x, y - t->down_y) > HV_TOUCH_SLOP_PX) t->moved = 1;
            map_to_target(t, x, y);
        }
        return;
    }
    if (action == HV_TOUCH_UP) {
        if (t->multi && t->nptr == 2) {
            /* the first of two fingers lifting: a short, still pair is a right click; the rest of the gesture is ignored */
            if (!t->scrolled && t->multi_travel < HV_TOUCH_SLOP_PX && now - t->multi_down_ms <= HV_TOUCH_TAP_MS && t->has_target) begin_click(t, 1, now);
            t->ignore_rest = 1;
        } else if (t->nptr == 1) {
            if (t->drag) { release_button(t); t->drag = 0; }
            else if (!t->multi && !t->ignore_rest && !t->moved && !t->long_fired && now - t->down_ms <= HV_TOUCH_TAP_MS) {
                begin_click(t, 0, now);
                t->last_tap_valid = 1; t->last_tap_ms = now; t->last_tap_x = x; t->last_tap_y = y;
            }
        }
        for (int k = i; k + 1 < t->nptr; k++) t->p[k] = t->p[k + 1];
        t->nptr--;
        if (t->nptr == 0) { t->multi = 0; t->ignore_rest = 0; }
        return;
    }
}

void hv_touch_tick(HvTouch *t, int64_t now) {
    /* long press: still, single finger, long enough */
    if (t->nptr == 1 && !t->moved && !t->multi && !t->drag && !t->long_fired && !t->ignore_rest && now - t->down_ms >= HV_TOUCH_LONG_MS) {
        t->long_fired = 1;
        begin_click(t, 1, now);
    }
    if (t->has_target) {
        if (!t->known && !t->homed) {                 /* no feedback: home the pointer to the top-left corner once */
            send_move(t, -t->dw, -t->dh);
            t->ex = 0; t->ey = 0; t->homed = 1;
        }
        if (t->known) {
            /* one damped step; wait until the guest has shown the effect of the last one (or a short time) */
            double ex = t->tx - t->px, ey = t->ty - t->py;
            int moving = fabs(ex) > 1.5 || fabs(ey) > 1.5;
            int ready = !t->sent_since_fb || t->fb_changed || now - t->last_send_ms >= NO_FB_WAIT_MS;
            if (moving && ready) {
                int dx = (int)lround(ex * 0.6), dy = (int)lround(ey * 0.6);
                if (!dx && fabs(ex) > 1.5) dx = ex > 0 ? 1 : -1;
                if (!dy && fabs(ey) > 1.5) dy = ey > 0 ? 1 : -1;
                dx = step_clamp(dx); dy = step_clamp(dy);
                send_move(t, dx, dy);
                t->fb_changed = 0; t->sent_since_fb = 1; t->last_send_ms = now;
            }
        } else if (t->homed) {                         /* dead reckoning: all of it at once, in steps */
            for (int n = 0; n < 40; n++) {
                double ex = t->tx - t->ex, ey = t->ty - t->ey;
                if (fabs(ex) < 1.0 && fabs(ey) < 1.0) break;   /* moves are whole pixels: a half pixel left over is not worth another move (and would flip-flop) */
                int dx = (int)lround(ex), dy = (int)lround(ey);
                dx = step_clamp(dx); dy = step_clamp(dy);
                send_move(t, dx, dy);
            }
        }
    }
    /* a click that waited for the pointer */
    if (t->pending_btn) {
        if (arrived(t) || now - t->pending_ms >= HV_TOUCH_CLICK_WAIT_MS) { click(t, t->pending_btn - 1); t->pending_btn = 0; }
    }
}

void hv_touch_diag(const HvTouch *t, HvTouchDiag *d) {
    memset(d, 0, sizeof *d);
    d->events = t->ev_count; d->last_action = t->ev_last_action;
    d->last_x = t->ev_last_x; d->last_y = t->ev_last_y; d->last_ms = t->ev_last_ms;
    d->fingers = t->nptr;
    d->has_target = t->has_target; d->target_x = t->tx; d->target_y = t->ty;
    d->pointer_known = t->known || t->homed;
    d->pointer_x = est_x(t); d->pointer_y = est_y(t);
    d->drag = t->drag; d->button_held = t->button_held;
}
