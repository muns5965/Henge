#include "dnload_probe.h"
#include "usb_desc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 2000
#define TOTAL_MS 20000

typedef struct {
    HvVirtioUsb *u;
    FILE *out;
    int sent;
    double t0;
    int limit;
    uint32_t session;        /* the session the sequence runs in; any other session ends it */
    HvDnloadResult *res;
    char last_tag[8];        /* stage name of the last answered request */
} Ctx;

static double now_ms(void) { /* monotonic, with sub-millisecond resolution */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static const char *err_name(int r) {
    switch (r) {
    case HV_USB_ETIMEOUT: return "timeout";
    case HV_USB_ESTALL: return "STALL";
    case HV_USB_EGONE: return "guest left the bus";
    case HV_USB_EIO: return "I/O error";
    default: return "error";
    }
}

static void hex(FILE *o, const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) fprintf(o, "%s%02x", i ? " " : "", d[i]);
}

/* The guest's session changed (or it left the bus). Records the bounds and says plainly what is and is not known; nothing is sent to the new session. */
static void note_gone(Ctx *c, const char *tag, int sent) {
    HvDnloadResult *res = c->res;
    uint32_t now_s = 0;
    int on = hv_vusb_online(c->u, &now_s);
    res->gone = 1;
    res->gone_sent = sent;
    res->gone_request = c->sent + (sent ? 0 : 1);
    res->session_after = on ? (int64_t)now_s : -1;
    if (sent) fprintf(c->out, "dnload probe: [%s] SESSION CHANGE noticed while request #%d was in flight (its answer was lost)\n", tag, res->gone_request);
    else fprintf(c->out, "dnload probe: [%s] SESSION CHANGE noticed before request #%d; it was NOT sent\n", tag, res->gone_request);
    if (res->last_answered) fprintf(c->out, "dnload probe: [%s]   last answered request: #%d (%s)\n", tag, res->last_answered, c->last_tag);
    else fprintf(c->out, "dnload probe: [%s]   no request had been answered yet\n", tag);
    if (on) fprintf(c->out, "dnload probe: [%s]   session %u -> %u (guest on the bus)\n", tag, c->session, now_s);
    else fprintf(c->out, "dnload probe: [%s]   session %u -> guest not on the bus\n", tag, c->session);
    fprintf(c->out, "dnload probe: [%s]   the host cannot tell when between those two points the guest left or which request, if any, caused it; "
                    "the new session is not queried (no descriptor read, no GET_STATE, no reset, nothing more is sent)\n", tag);
}

/* One control request, logged with everything it carried. Refuses (-100) past the request limit or the total time limit.
   A request is not sent into a session other than the one the sequence started in (-3 = HV_USB_EGONE, recorded as a session change). */
static int ctrl(Ctx *c, const char *tag, uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, void *data, uint16_t len) {
    if (c->sent >= c->limit || now_ms() - c->t0 > TOTAL_MS) {
        fprintf(c->out, "dnload probe: %s refused (request or time limit)\n", tag);
        return -100;
    }
    uint32_t cur = 0;
    if (!hv_vusb_online(c->u, &cur) || cur != c->session) {
        note_gone(c, tag, 0);
        return HV_USB_EGONE;
    }
    c->sent++;
    int in = (rt & 0x80) != 0;
    fprintf(c->out, "dnload probe: [%s] request #%d: bmRequestType=0x%02x bRequest=%u wValue=0x%04x wIndex=%u wLength=%u", tag, c->sent, rt, rq, val, idx, len);
    if (!in && len) { fprintf(c->out, " data="); hex(c->out, data, len); }
    fprintf(c->out, "\n");
    double t = now_ms();
    int r = hv_usb_control(c->u, rt, rq, val, idx, data, len, TIMEOUT_MS);
    if (r == HV_USB_ETIMEOUT || r == HV_USB_EIO) {
        /* hv_usb_control reads the session only when it starts to wait, so a guest that re-attached just after the request went out shows as a timeout
           instead of "left the bus": look at the session itself. */
        uint32_t cur2 = 0;
        if (!hv_vusb_online(c->u, &cur2) || cur2 != c->session) r = HV_USB_EGONE;
    }
    fprintf(c->out, "dnload probe: [%s]   -> %d", tag, r);
    if (r < 0) fprintf(c->out, " (%s)", err_name(r));
    else if (in && r > 0) { fprintf(c->out, ": "); hex(c->out, data, r); }
    fprintf(c->out, "  (%.3f ms)\n", now_ms() - t);
    if (r >= 0 || r == HV_USB_ESTALL) { /* the device answered (a STALL is an answer) */
        c->res->last_answered = c->sent;
        snprintf(c->last_tag, sizeof c->last_tag, "%s", tag);
    } else if (r == HV_USB_EGONE) {
        note_gone(c, tag, 1);
    }
    return r;
}

static int get_state(Ctx *c, const char *tag, int *state) {
    uint8_t st = 0xff;
    int r = ctrl(c, tag, 0xA1, 5, 0, 0, &st, 1);
    *state = (r == 1) ? st : -1;
    if (r == 1) fprintf(c->out, "dnload probe: [%s]   DFU state %u (%s)\n", tag, st, hv_dfu_state_name(st));
    return r;
}

int hv_dnload_probe_run(HvVirtioUsb *usb, FILE *out, int wait_ms, int with_status, HvDnloadResult *resp) {
    HvDnloadResult res;
    memset(&res, 0, sizeof res);
    res.a0_state = res.a2_state = res.a5_state = -1;
    Ctx c = {.u = usb, .out = out, .t0 = now_ms(), .limit = with_status >= HV_DNLOAD_A4 ? HV_DNLOAD_MAX_REQUESTS_A4 : HV_DNLOAD_MAX_REQUESTS};
    int rv = 0;
    int64_t s = hv_usb_wait_online(usb, wait_ms);
    if (s < 0) {
        fprintf(out, "dnload probe: the guest never went on the bus within %d ms; nothing sent\n", wait_ms);
        if (resp) *resp = res;
        return -1;
    }
    fprintf(out, "dnload probe: guest online (session %lld). Stage %s only; the sequence stops at the first anything-unexpected.\n", (long long)s, with_status >= HV_DNLOAD_A4 ? "A0-A5 (A4 = one ABORT, A5 = one GET_STATE)" : with_status ? "A0-A3" : "A0-A2");
    c.session = (uint32_t)s;
    c.res = &res;
    res.session_before = s;
    res.session_after = -1;

    /* setup */
    int r = hv_usb_reset(usb, TIMEOUT_MS);
    fprintf(out, "dnload probe: [setup] bus reset -> %d%s\n", r, r < 0 ? " (stop)" : "");
    if (r == HV_USB_EGONE) note_gone(&c, "setup", 1);
    if (r < 0) { res.stopped_at = 1; goto done; }
    uint8_t cfg = 0xff;
    r = ctrl(&c, "setup", 0x80, 8, 0, 0, &cfg, 1);
    if (r != 1) { res.stopped_at = 1; goto done; }
    if (cfg == 0) {
        r = ctrl(&c, "setup", 0x00, 9, 1, 0, NULL, 0);
        if (r < 0) { res.stopped_at = 1; goto done; }
        r = ctrl(&c, "setup", 0x80, 8, 0, 0, &cfg, 1);
        if (r != 1 || cfg != 1) { fprintf(out, "dnload probe: [setup] configuration is not 1 afterwards (stop)\n"); res.stopped_at = 1; goto done; }
    }

    /* A0 */
    r = get_state(&c, "A0", &res.a0_state);
    if (r != 1) { res.stopped_at = 2; goto done; }
    if (res.a0_state != 2) { fprintf(out, "dnload probe: [A0] the device is not in dfuIDLE; no data is sent (stop)\n"); res.stopped_at = 2; goto done; }

    /* A1 */
    uint8_t blk[16];
    memset(blk, 0, sizeof blk);
    r = ctrl(&c, "A1", 0x21, 1, 0, 0, blk, (uint16_t)sizeof blk);
    res.a1_result = r;
    if (r < 0) { fprintf(out, "dnload probe: [A1] the DNLOAD failed; nothing more is sent (stop)\n"); res.stopped_at = 3; goto done; }

    /* A2 */
    r = get_state(&c, "A2", &res.a2_state);
    if (r != 1) { res.stopped_at = 4; goto done; }

    /* A3: one GET_STATUS, only in the run that reproduces A0-A2 exactly (state 5 after the DNLOAD) */
    if (with_status) {
        if (res.a2_state != 5) {
            fprintf(out, "dnload probe: [A3] not sent: A2 was not state 5, so this run does not reproduce the earlier one (stop)\n");
            res.stopped_at = 5;
            goto done;
        }
        uint8_t ds[6];
        memset(ds, 0xEE, sizeof ds);
        res.a3_sent = 1;
        r = ctrl(&c, "A3", 0xA1, 3, 0, 0, ds, 6);
        res.a3_len = r;
        if (r > 0) memcpy(res.a3_raw, ds, (size_t)(r > 6 ? 6 : r));
        HvDfuStatus st;
        if (hv_dfu_parse_status(r, ds, &st) == 0) {
            res.a3_parsed = 1;
            res.a3_status = st.status, res.a3_poll_ms = st.poll_timeout_ms, res.a3_state = st.state, res.a3_istring = st.istring;
        } else {
            res.stopped_at = 5;
        }
    }

    /* A4/A5: one ABORT and one GET_STATE, only after a clean A3 (bStatus 0 and bState 5), nothing after them */
    if (with_status >= HV_DNLOAD_A4 && res.stopped_at == 0) {
        if (!(res.a3_parsed && res.a3_status == 0 && res.a3_state == 5)) {
            fprintf(out, "dnload probe: [A4] ABORT not sent: A3 did not report bStatus 0 with bState 5 (stop)\n");
            res.stopped_at = 6;
            goto done;
        }
        res.a4_sent = 1;
        r = ctrl(&c, "A4", 0x21, 6, 0, 0, NULL, 0);
        res.a4_result = r;
        if (r < 0) { fprintf(out, "dnload probe: [A4] the ABORT failed; no GET_STATE is sent after it (stop)\n"); res.stopped_at = 6; goto done; }
        r = get_state(&c, "A5", &res.a5_state);
        if (r != 1) { res.stopped_at = 6; goto done; }
    }

done:
    res.requests = c.sent;
    fprintf(out, "dnload probe: ---- result ----\n");
    fprintf(out, "dnload probe: control requests sent: %d (limit %d); stopped at: %s\n", res.requests, c.limit,
            res.stopped_at == 0 ? (with_status >= HV_DNLOAD_A4 ? "no stop, A0-A5 all answered" : with_status ? "no stop, A0-A3 all answered" : "no stop, A0-A2 all answered") : res.stopped_at == 1 ? "setup" : res.stopped_at == 2 ? "A0" : res.stopped_at == 3 ? "A1" : res.stopped_at == 4 ? "A2" : res.stopped_at == 5 ? "A3" : "A4/A5");
    if (res.a0_state >= 0) fprintf(out, "dnload probe: A0 state %d (%s)\n", res.a0_state, hv_dfu_state_name(res.a0_state));
    if (res.a1_result > 0) fprintf(out, "dnload probe: A1 DNLOAD: the device answered the request (%d bytes were sent; the count is the request's wLength, not one the device reported)\n", res.a1_result);
    if (res.a2_state >= 0) {
        fprintf(out, "dnload probe: A2 state %d (%s)\n", res.a2_state, hv_dfu_state_name(res.a2_state));
        switch (res.a2_state) {
        case 3: fprintf(out, "dnload probe: reading: dfuDNLOAD-SYNC is the DFU state after a download block that the host has not yet polled with GET_STATUS. It is the ordinary DFU reaction. It does not show that the ROM accepted the bytes as firmware.\n"); break;
        case 5: fprintf(out, "dnload probe: reading: dfuDNLOAD-IDLE. The device is waiting for the next block or the end of the download.\n"); break;
        case 2: fprintf(out, "dnload probe: reading: still dfuIDLE. The ROM did not enter a download state (the request may have been ignored).\n"); break;
        case 10: fprintf(out, "dnload probe: reading: dfuERROR. The ROM rejected something; GET_STATUS (not sent) would say what.\n"); break;
        default: fprintf(out, "dnload probe: reading: unexpected state; recorded, not interpreted.\n"); break;
        }
    }
    if (res.a3_sent) {
        fprintf(out, "dnload probe: A3 GET_STATUS: transfer returned %d%s", res.a3_len, res.a3_len == 6 ? "" : " (6 bytes were expected: not parsed)");
        if (res.a3_len > 0) { fprintf(out, "; raw: "); hex(out, res.a3_raw, res.a3_len > 6 ? 6 : res.a3_len); }
        fprintf(out, "\n");
        if (res.a3_parsed) {
            fprintf(out, "dnload probe: A3 bStatus=%d (%s) bwPollTimeout=%d ms bState=%d (%s) iString=%d\n", res.a3_status, hv_dfu_status_name(res.a3_status), res.a3_poll_ms, res.a3_state, hv_dfu_state_name(res.a3_state), res.a3_istring);
            fprintf(out, "dnload probe: reading: these are the values GET_STATUS returned, nothing more. bState after GET_STATUS can differ from what GET_STATE showed because GET_STATUS can advance the state; no second GET_STATUS is sent, whatever it says. Whether state 5 is normal for this ROM, or the ROM accepted the 16 bytes, is not answered by this.\n");
        }
        fprintf(out, "dnload probe: A3 is the last request: no second GET_STATUS, no ABORT, no CLR_STATUS, no zero-length DNLOAD, no second block was sent.\n");
    }
    if (res.a4_sent) {
        fprintf(out, "dnload probe: A4 DFU ABORT: transfer returned %d%s\n", res.a4_result, res.a4_result < 0 ? " (failed; no GET_STATE was sent after it)" : " (the device answered the request)");
        if (res.a5_state >= 0) {
            fprintf(out, "dnload probe: A5 GET_STATE after ABORT: %d (%s)\n", res.a5_state, hv_dfu_state_name(res.a5_state));
            switch (res.a5_state) {
            case 2: fprintf(out, "dnload probe: reading: back in dfuIDLE after ABORT: the device left the download state when asked to.\n"); break;
            case 5: fprintf(out, "dnload probe: reading: still dfuDNLOAD-IDLE after ABORT: the ABORT did not change the state (ignored, or not processed yet).\n"); break;
            case 10: fprintf(out, "dnload probe: reading: dfuERROR after ABORT. GET_STATUS and CLR_STATUS (neither is sent) would be the next questions.\n"); break;
            default: fprintf(out, "dnload probe: reading: unexpected state; recorded, not interpreted.\n"); break;
            }
        }
        else if (res.gone && res.gone_request > 0)
            fprintf(out, "dnload probe: A5: no state was read, so whether the ABORT took the device back to dfuIDLE is NOT known; the session changed first (see SESSION CHANGE).\n");
        fprintf(out, "dnload probe: A4/A5 are the last requests: no CLR_STATUS, no second ABORT, no retry, no further GET_STATUS, no DNLOAD was sent.\n");
    }
    if (res.gone) {
        char last[32];
        if (res.last_answered) snprintf(last, sizeof last, "#%d (%s)", res.last_answered, c.last_tag);
        else snprintf(last, sizeof last, "none");
        fprintf(out, "dnload probe: SESSION CHANGE summary: session %lld -> %lld (-1 = not on the bus); the request in flight or next was #%d (%s); the last answered request was %s.\n",
                (long long)res.session_before, (long long)res.session_after, res.gone_request, res.gone_sent ? "it had been sent" : "it was not sent", last);
        fprintf(out, "dnload probe: SESSION CHANGE reading: the guest re-attached (a new session) at some point between those two requests. This run cannot say whether the request before it, "
                     "the one in flight, or something the guest's own ROM did caused it. Nothing was sent to the new session.\n");
    }
    fprintf(out, "dnload probe: nothing further is sent. (CPU state: see the vmrun lines; it is context only.)\n");
    rv = res.stopped_at;
    if (resp) *resp = res;
    return rv;
}

static void *thread_main(void *arg) {
    hv_dnload_probe_run(((struct { HvVirtioUsb *u; int ws; } *)arg)->u, stdout, 60000, ((struct { HvVirtioUsb *u; int ws; } *)arg)->ws, NULL);
    fflush(stdout);
    return NULL;
}

void hv_dnload_probe_start(HvVirtioUsb *usb, int with_status) {
    static struct { HvVirtioUsb *u; int ws; } arg; /* the probe runs once per process */
    arg.u = usb, arg.ws = with_status;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, &arg) == 0) pthread_detach(t);
}
