#include "input_inject.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { J_KEY = 1, J_CLICK = 2 };
/* Linux input codes (the numbers hv_vm_key takes; the same table host/key_map_table.h maps Android keys to). */
static const struct { const char *name; uint16_t code; } KEYS[] = {
    {"enter", 28}, {"tab", 15}, {"space", 57}, {"esc", 1}, {"up", 103}, {"down", 108}, {"left", 105}, {"right", 106},
    {"shift", 42}, {"ctrl", 29}, {"alt", 56}, {"cmd", 125},
};

uint16_t hv_inject_key_code(const char *name) {
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) if (!strcasecmp(name, KEYS[i].name)) return KEYS[i].code;
    return 0;
}

static void say(HvInject *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void say(HvInject *j, const char *fmt, ...) {
    if (!j->log) return;
    char b[200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    j->log(j->log_ctx, b);
}

void hv_inject_init(HvInject *j, const HvInputSink *sink, void (*log)(void *, const char *), void *log_ctx) {
    memset(j, 0, sizeof *j);
    if (sink) j->sink = *sink;
    j->log = log; j->log_ctx = log_ctx; j->dw = 1920; j->dh = 1080;
    j->key_ok = j->mouse_ok = 1;
    hv_touch_init(&j->touch, sink);
    hv_touch_set_display(&j->touch, j->dw, j->dh);
    hv_touch_set_view(&j->touch, 0, 0, (float)(j->dw - 1), (float)(j->dh - 1));   /* view pixels = guest pixels: x maps to x */
}

void hv_inject_set_paths(HvInject *j, int key_ok, const char *key_why, int mouse_ok, const char *mouse_why) {
    j->key_ok = key_ok ? 1 : 0; j->mouse_ok = mouse_ok ? 1 : 0;
    snprintf(j->key_why, sizeof j->key_why, "%s", key_why ? key_why : "no keyboard device");
    snprintf(j->mouse_why, sizeof j->mouse_why, "%s", mouse_why ? mouse_why : "no mouse device");
}

static void set_display(HvInject *j, int w, int h) {
    if (w < 16 || h < 16 || w > 16384 || h > 16384) return;
    j->dw = w; j->dh = h;
    hv_touch_set_display(&j->touch, w, h);
    hv_touch_set_view(&j->touch, 0, 0, (float)(w - 1), (float)(h - 1));
}

static int push(HvInject *j, const HvInjectJob *job) {
    if (j->qtail - j->qhead >= HV_INJECT_QUEUE) return -1;
    j->q[j->qtail++ % HV_INJECT_QUEUE] = *job;
    return 0;
}

int hv_inject_command(HvInject *j, const char *line, int64_t now_ms, char *msg, size_t cap) {
    (void)now_ms;
#define REFUSE(...) do { if (msg && cap) snprintf(msg, cap, __VA_ARGS__); j->refused++; return -1; } while (0)
    char w[16] = "", a[48] = ""; int x = 0, y = 0;
    int n = sscanf(line, "%15s %47s", w, a);
    if (n < 1) REFUSE("empty command");
    HvInjectJob job; memset(&job, 0, sizeof job);
    if (!strcmp(w, "abort")) {
        if (j->held_btn && j->sink.mouse_button) j->sink.mouse_button(0, 0);
        for (int i = 0; i < HV_INJECT_STEPS; i++) if (j->held_key[i] && j->sink.key) j->sink.key((uint16_t)j->held_key[i], 0);
        memset(j->held_key, 0, sizeof j->held_key); j->held_btn = 0; j->active = 0; j->qhead = j->qtail = 0;
        say(j, "abort: jobs dropped, held button/keys released");
        if (msg && cap) snprintf(msg, cap, "aborted");
        return 0;
    }
    if (!strcmp(w, "display")) {
        int dw = 0, dh = 0;
        if (sscanf(line, "%*s %d %d", &dw, &dh) != 2 || dw < 16 || dh < 16 || dw > 16384 || dh > 16384) REFUSE("display W H out of range");
        set_display(j, dw, dh);
        if (msg && cap) snprintf(msg, cap, "display %dx%d", dw, dh);
        return 0;
    }
    if ((!strcmp(w, "key") && !j->key_ok) || ((!strcmp(w, "click") || !strcmp(w, "dclick")) && !j->mouse_ok)) {
        const char *why = !strcmp(w, "key") ? j->key_why : j->mouse_why;
        if (msg && cap) snprintf(msg, cap, "%s; input cannot reach the guest", why);
        j->rejected_no_path++;
        return -2;
    }
    if (!strcmp(w, "key")) {
        if (n < 2) REFUSE("key needs a name");
        char t[48]; snprintf(t, sizeof t, "%s", a);
        char *save = NULL;
        for (char *p = strtok_r(t, "+", &save); p; p = strtok_r(NULL, "+", &save)) {
            uint16_t c = hv_inject_key_code(p);
            if (!c) REFUSE("unknown key name '%s'", p);
            if (job.ncodes >= HV_INJECT_STEPS) REFUSE("too many keys");
            job.codes[job.ncodes++] = c;
        }
        if (!job.ncodes) REFUSE("key needs a name");
        job.kind = J_KEY; snprintf(job.text, sizeof job.text, "key %.34s", a);
    } else if (!strcmp(w, "click") || !strcmp(w, "dclick")) {
        job.kind = J_CLICK; job.count = w[0] == 'd' ? 2 : 1;
        if (n >= 2) {
            if (sscanf(line, "%*s %d %d", &x, &y) != 2) REFUSE("%s needs X Y or nothing", w);
            if (x < 0 || y < 0 || x >= j->dw || y >= j->dh) REFUSE("%d,%d is outside the %dx%d picture", x, y, j->dw, j->dh);
            job.x = x; job.y = y;
        } else job.x = job.y = -1;   /* here */
        snprintf(job.text, sizeof job.text, "%s %d,%d", w, job.x, job.y);
    } else REFUSE("unknown command '%s'", w);
    if (push(j, &job) != 0) REFUSE("queue full");
    say(j, "queued: %s (display %dx%d)", job.text, j->dw, j->dh);
    if (msg && cap) snprintf(msg, cap, "queued %s", job.text);
    return 0;
#undef REFUSE
}

int hv_inject_busy(const HvInject *j) { return j->active || j->qhead != j->qtail; }

static int arrived(HvInject *j, double *ex, double *ey) {
    HvTouchDiag d; hv_touch_diag(&j->touch, &d);
    *ex = d.pointer_x; *ey = d.pointer_y;
    j->arrive_confirmed = d.pointer_confirmed;   /* 1 = the guest's own pointer feedback; 0 = dead reckoning only */
    return d.has_target && d.pointer_known && fabs(d.target_x - d.pointer_x) <= 1.5 && fabs(d.target_y - d.pointer_y) <= 1.5;
}

static void finish(HvInject *j) { j->active = 0; j->jobs_done++; }

void hv_inject_tick(HvInject *j, int known, int px, int py, int64_t now) {
    hv_touch_pointer(&j->touch, known, px, py);
    hv_touch_tick(&j->touch, now);
    if (!j->active) {
        if (j->qhead == j->qtail) return;
        j->cur = j->q[j->qhead++ % HV_INJECT_QUEUE]; j->active = 1; j->phase = 0; j->clicks_done = 0; j->start_ms = j->phase_ms = now;
        say(j, "start: %s (guest pointer feedback %s)", j->cur.text, known ? "known" : "NOT available, dead reckoning");
    }
    HvInjectJob *c = &j->cur;
    if (c->kind == J_KEY) {
        if (j->phase == 0) {
            for (int i = 0; i < c->ncodes; i++) { if (j->sink.key) j->sink.key(c->codes[i], 1); j->held_key[i] = c->codes[i]; j->keys_down++; say(j, "key down code %u -> hv_vm_key (virtual USB HID keyboard report queued)", c->codes[i]); }
            j->phase = 1; j->phase_ms = now;
        } else if (now - j->phase_ms >= HV_INJECT_KEY_MS) {
            for (int i = c->ncodes - 1; i >= 0; i--) { if (j->sink.key) j->sink.key(c->codes[i], 0); j->held_key[i] = 0; j->keys_up++; say(j, "key up code %u", c->codes[i]); }
            finish(j);
        }
        return;
    }
    /* click */
    double ex, ey;
    switch (j->phase) {
    case 0:
        if (c->x >= 0) {   /* a DOWN and an UP that is too late to be a tap: sets the target and clicks nothing (a held finger would turn into a right click after 500 ms) */
            hv_touch_event(&j->touch, HV_TOUCH_DOWN, 7, (float)c->x, (float)c->y, now);
            hv_touch_event(&j->touch, HV_TOUCH_UP, 7, (float)c->x, (float)c->y, now + HV_TOUCH_TAP_MS + 1000);
            say(j, "target guest %d,%d set (closed loop on the pointer)", c->x, c->y);
            j->phase = 1;
        } else j->phase = 2;
        j->phase_ms = now;
        break;
    case 1: {
        int ok = arrived(j, &ex, &ey);
        if (ok || now - j->phase_ms >= HV_INJECT_ARRIVE_MS) {
            if (ok) say(j, "pointer arrived at %.0f,%.0f (%s)", ex, ey, j->arrive_confirmed ? "confirmed by the guest's pointer feedback" : "ESTIMATE ONLY: dead reckoning, no guest pointer feedback seen, so guest arrival is NOT proven");
            else say(j, "pointer did NOT arrive within %d ms (now %.0f,%.0f): clicking anyway", HV_INJECT_ARRIVE_MS, ex, ey);
            j->phase = 2;
        }
        break;
    }
    case 2:
        if (j->sink.mouse_button) j->sink.mouse_button(0, 1);
        j->held_btn = 1; j->btn_down++; say(j, "button left DOWN -> hv_vm_mouse_button (virtio-input event queued)");
        j->phase = 3; j->phase_ms = now;
        break;
    case 3:
        if (now - j->phase_ms >= HV_INJECT_HOLD_MS) {
            if (j->sink.mouse_button) j->sink.mouse_button(0, 0);
            j->held_btn = 0; j->btn_up++; j->clicks_done++; say(j, "button left UP (click %d of %d)", j->clicks_done, c->count);
            if (j->clicks_done < c->count) { j->phase = 4; j->phase_ms = now; } else finish(j);
        }
        break;
    case 4:
        if (now - j->phase_ms >= HV_INJECT_DOUBLE_GAP_MS) j->phase = 2;
        break;
    }
}
