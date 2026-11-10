#include "dnload_probe.h"
#include "usb_desc.h"

#include <pthread.h>
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
} Ctx;

static double now_ms(void) {
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

/* One control request, logged with everything it carried. Refuses (-100) past the request limit or the total time limit. */
static int ctrl(Ctx *c, const char *tag, uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, void *data, uint16_t len) {
    if (c->sent >= HV_DNLOAD_MAX_REQUESTS || now_ms() - c->t0 > TOTAL_MS) {
        fprintf(c->out, "dnload probe: %s refused (request or time limit)\n", tag);
        return -100;
    }
    c->sent++;
    int in = (rt & 0x80) != 0;
    fprintf(c->out, "dnload probe: [%s] request #%d: bmRequestType=0x%02x bRequest=%u wValue=0x%04x wIndex=%u wLength=%u", tag, c->sent, rt, rq, val, idx, len);
    if (!in && len) { fprintf(c->out, " data="); hex(c->out, data, len); }
    fprintf(c->out, "\n");
    double t = now_ms();
    int r = hv_usb_control(c->u, rt, rq, val, idx, data, len, TIMEOUT_MS);
    fprintf(c->out, "dnload probe: [%s]   -> %d", tag, r);
    if (r < 0) fprintf(c->out, " (%s)", err_name(r));
    else if (in && r > 0) { fprintf(c->out, ": "); hex(c->out, data, r); }
    fprintf(c->out, "  (%.0f ms)\n", now_ms() - t);
    return r;
}

static int get_state(Ctx *c, const char *tag, int *state) {
    uint8_t st = 0xff;
    int r = ctrl(c, tag, 0xA1, 5, 0, 0, &st, 1);
    *state = (r == 1) ? st : -1;
    if (r == 1) fprintf(c->out, "dnload probe: [%s]   DFU state %u (%s)\n", tag, st, hv_dfu_state_name(st));
    return r;
}

int hv_dnload_probe_run(HvVirtioUsb *usb, FILE *out, int wait_ms, HvDnloadResult *resp) {
    HvDnloadResult res = {0, 0, -1, 0, -1};
    Ctx c = {usb, out, 0, now_ms()};
    int rv = 0;
    int64_t s = hv_usb_wait_online(usb, wait_ms);
    if (s < 0) {
        fprintf(out, "dnload probe: the guest never went on the bus within %d ms; nothing sent\n", wait_ms);
        if (resp) *resp = res;
        return -1;
    }
    fprintf(out, "dnload probe: guest online (session %lld). Stage A0-A2 only; the sequence stops at the first anything-unexpected.\n", (long long)s);

    /* setup */
    int r = hv_usb_reset(usb, TIMEOUT_MS);
    fprintf(out, "dnload probe: [setup] bus reset -> %d%s\n", r, r < 0 ? " (stop)" : "");
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

done:
    res.requests = c.sent;
    fprintf(out, "dnload probe: ---- result ----\n");
    fprintf(out, "dnload probe: control requests sent: %d (limit %d); stopped at: %s\n", res.requests, HV_DNLOAD_MAX_REQUESTS,
            res.stopped_at == 0 ? "no stop, A0-A2 all answered" : res.stopped_at == 1 ? "setup" : res.stopped_at == 2 ? "A0" : res.stopped_at == 3 ? "A1" : "A2");
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
    fprintf(out, "dnload probe: nothing further is sent. (CPU state: see the vmrun lines; it is context only.)\n");
    rv = res.stopped_at;
    if (resp) *resp = res;
    return rv;
}

static void *thread_main(void *arg) {
    hv_dnload_probe_run((HvVirtioUsb *)arg, stdout, 60000, NULL);
    fflush(stdout);
    return NULL;
}

void hv_dnload_probe_start(HvVirtioUsb *usb) {
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, usb) == 0) pthread_detach(t);
}
