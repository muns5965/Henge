#include "irecv_probe.h"
#include "usb_desc.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libirecovery.h>
#include <libusb.h>

#define APPLE_VID 0x05ac

typedef enum { ST_NOT_REACHED, ST_OK, ST_FAILED } StageResult;
typedef struct { const char *name; StageResult r; char note[300]; } Stage;

static void stage(Stage *s, const char *name, StageResult r, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void stage(Stage *s, const char *name, StageResult r, const char *fmt, ...) {
    s->name = name, s->r = r;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->note, sizeof s->note, fmt, ap);
    va_end(ap);
}

static const char *irecv_err(int e) {
    switch (e) {
    case IRECV_E_SUCCESS: return "success";
    case IRECV_E_NO_DEVICE: return "NO_DEVICE";
    case IRECV_E_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case IRECV_E_UNABLE_TO_CONNECT: return "UNABLE_TO_CONNECT";
    case IRECV_E_INVALID_INPUT: return "INVALID_INPUT";
    case IRECV_E_USB_INTERFACE: return "USB_INTERFACE";
    case IRECV_E_USB_CONFIGURATION: return "USB_CONFIGURATION";
    case IRECV_E_PIPE: return "PIPE";
    case IRECV_E_TIMEOUT: return "TIMEOUT";
    case IRECV_E_UNSUPPORTED: return "UNSUPPORTED";
    default: return "other";
    }
}

/* GET_CONFIGURATION and the DFU class GET_STATE (interface 0), read directly, outside the library. */
static void read_state(HvVirtioUsb *u, FILE *out, const char *when, int *cfg, int *state) {
    uint8_t c = 0xff, st = 0xff;
    int rc = hv_usb_control(u, 0x80, 8, 0, 0, &c, 1, 2000);
    int rs = hv_usb_control(u, 0xA1, 5, 0, 0, &st, 1, 2000);
    *cfg = rc == 1 ? c : -1;
    *state = rs == 1 ? st : -1;
    fprintf(out, "irecv probe: %s:\n", when);
    if (rc == 1) fprintf(out, "irecv probe:   GET_CONFIGURATION: configuration value %u\n", c);
    else fprintf(out, "irecv probe:   GET_CONFIGURATION: no usable answer (transfer returned %d)\n", rc);
    if (rs == 1) fprintf(out, "irecv probe:   DFU GET_STATE(interface 0): state %u (%s)\n", st, hv_dfu_state_name(st));
    else fprintf(out, "irecv probe:   DFU GET_STATE(interface 0): no usable answer (transfer returned %d)\n", rs);
}

static const char *req_name(const HvCtrlRecord *r) {
    uint8_t type = (r->bmRequestType >> 5) & 3;
    if (type == 0) {
        switch (r->bRequest) {
        case 0: return "GET_STATUS";
        case 6: return "GET_DESCRIPTOR";
        case 8: return "GET_CONFIGURATION";
        case 9: return "SET_CONFIGURATION";
        case 10: return "GET_INTERFACE";
        case 11: return "SET_INTERFACE";
        case 1: return "CLEAR_FEATURE";
        case 3: return "SET_FEATURE";
        default: return "standard request";
        }
    }
    if (type == 1) {
        switch (r->bRequest) {
        case 0: return "DFU_DETACH";
        case 1: return "DFU_DNLOAD";
        case 2: return "DFU_UPLOAD";
        case 3: return "DFU_GETSTATUS";
        case 4: return "DFU_CLRSTATUS";
        case 5: return "DFU_GETSTATE";
        case 6: return "DFU_ABORT";
        default: return "class request";
        }
    }
    return "vendor request";
}

/* ---- device events: what idevicerestore listens to (irecv_device_event_subscribe) to learn the mode and to wait for the device to come back ---- */
typedef HvIrecvEv Ev;
#define EV_MAX 32
static Ev g_ev[EV_MAX];
static int g_nev, g_ev_dropped;
static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static struct timespec g_ev_t0;

static int ms_since(const struct timespec *t0) {
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    return (int)((n.tv_sec - t0->tv_sec) * 1000 + (n.tv_nsec - t0->tv_nsec) / 1000000);
}

static void ev_cb(const irecv_device_event_t *e, void *user) {
    (void)user;
    pthread_mutex_lock(&g_ev_mu);
    if (g_nev < EV_MAX) {
        Ev *v = &g_ev[g_nev++];
        v->type = (int)e->type, v->mode = (int)e->mode, v->ms = ms_since(&g_ev_t0);
        v->have_ecid = e->device_info && e->device_info->have_ecid;
        v->ecid = e->device_info ? e->device_info->ecid : 0;
    } else {
        g_ev_dropped++;
    }
    pthread_mutex_unlock(&g_ev_mu);
}

static int ev_count(void) {
    pthread_mutex_lock(&g_ev_mu);
    int n = g_nev;
    pthread_mutex_unlock(&g_ev_mu);
    return n;
}

static int dfu_family_mode(int m) {
    return m == IRECV_K_DFU_MODE || m == IRECV_K_WTF_MODE || m == IRECV_K_PORT_DFU_MODE || (m >= 0x1280 && m <= 0x1283);
}

int hv_irecv_events_verdict(const HvIrecvEv *ev, int n, int dropped, uint64_t ecid, char *note, int cap) {
    if (dropped) { snprintf(note, cap, "%d event(s) were not recorded", dropped); return 0; }
    if (n == 0) { snprintf(note, cap, "no event arrived"); return 0; }
    for (int i = 0; i < n; i++) {
        int want = (i % 2 == 0) ? HV_IREC_EV_ADD : HV_IREC_EV_REMOVE;
        if (ev[i].type != want) {
            snprintf(note, cap, "event #%d is a %s where the bus model expects %s (ADD and REMOVE must alternate, starting with ADD)", i,
                     ev[i].type == HV_IREC_EV_ADD ? "ADD" : "REMOVE", want == HV_IREC_EV_ADD ? "an ADD" : "a REMOVE");
            return 0;
        }
        if (ev[i].type == HV_IREC_EV_ADD) {
            if (!ev[i].have_ecid || ev[i].ecid != ecid) { snprintf(note, cap, "event #%d: ADD carries another ECID than requested", i); return 0; }
            if (!dfu_family_mode(ev[i].mode)) { snprintf(note, cap, "event #%d: ADD with mode 0x%04x, which libirecovery does not call DFU/WTF", i, (unsigned)ev[i].mode); return 0; }
        }
    }
    if (ev[n - 1].type != HV_IREC_EV_ADD) { snprintf(note, cap, "the last event is a REMOVE: the device left and did not come back"); return 0; }
    snprintf(note, cap, "%d event(s): ADD%s; every ADD carries the requested ECID and a DFU-family mode; the device is present at the end", n, n > 1 ? " and REMOVE alternate" : "");
    return 1;
}

int hv_irecv_probe_run(HvVirtioUsb *usb, uint64_t ecid, FILE *out, int wait_ms) {
    enum { S_EVENTS, S_DISCOVER, S_OPEN, S_INFO_STR, S_INFO_TABLE, S_CLOSE, S_BOUNDARY, S_COUNT };
    Stage st[S_COUNT];
    for (int i = 0; i < S_COUNT; i++) st[i].name = "", st[i].r = ST_NOT_REACHED, st[i].note[0] = 0;
    int64_t sess = hv_usb_wait_online(usb, wait_ms);
    fprintf(out, "irecv probe: guest on the bus (session %lld); requested ECID 0x%" PRIx64 "\n", (long long)sess, ecid);
    if (sess < 0) return -1;
    usleep(200000);
    hv_libusb_shim_attach(usb);

    int cfg_before, state_before, cfg_after = -2, state_after = -2;
    read_state(usb, out, "before the library touches the device", &cfg_before, &state_before);

    /* Stage 0: the device-event interface idevicerestore relies on. Subscribed first and kept until the end, so the bus reset that the open below causes is seen too. */
    int ctrl0 = hv_libusb_ctrl_count(), data0 = hv_libusb_data_transfer_count();
    pthread_mutex_lock(&g_ev_mu);
    g_nev = g_ev_dropped = 0;
    pthread_mutex_unlock(&g_ev_mu);
    clock_gettime(CLOCK_MONOTONIC, &g_ev_t0);
    irecv_device_event_context_t ectx = NULL;
    irecv_error_t sub = irecv_device_event_subscribe(&ectx, ev_cb, NULL);
    if (sub == IRECV_E_SUCCESS) {
        for (int w = 0; w < 60 && ev_count() == 0; w++) usleep(50000);   /* up to 3 s for the first ADD */
    }

    /* Stage 1: discovery through the same libusb API libirecovery uses (this is where the stand-in resets the bus and reads the descriptors). */
    int first_req = hv_libusb_ctrl_count();
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(NULL, &list);
    int found = 0;
    uint16_t pid = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) == 0) {
            fprintf(out, "irecv probe: libusb lists %04x:%04x\n", d.idVendor, d.idProduct);
            if (d.idVendor == APPLE_VID && (d.idProduct == IRECV_K_DFU_MODE || d.idProduct == IRECV_K_WTF_MODE || d.idProduct == IRECV_K_PORT_DFU_MODE ||
                                           (d.idProduct >= 0x1280 && d.idProduct <= 0x1283)))
                found = 1, pid = d.idProduct;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    if (found) stage(&st[S_DISCOVER], "discover", ST_OK, "an Apple device with PID 0x%04x (a mode libirecovery accepts) is on the stand-in bus", pid);
    else stage(&st[S_DISCOVER], "discover", ST_FAILED, "%zd device(s) listed, none a libirecovery-supported Apple mode", n);

    /* Stage 2: the library's own open. */
    irecv_client_t cl = NULL;
    int open_rc = IRECV_E_UNABLE_TO_CONNECT;
    int open_first = hv_libusb_ctrl_count();
    if (found) {
        irecv_set_debug_level(1);
        open_rc = irecv_open_with_ecid(&cl, ecid);
    }
    int open_last = hv_libusb_ctrl_count();
    int set_cfg = -1, set_cfg_res = 0, get_cfg_seen = 0, set_if_seen = 0;
    for (int k = open_first; k < open_last; k++) {
        HvCtrlRecord r;
        if (!hv_libusb_ctrl_get(k, &r)) continue;
        if (r.bmRequestType == 0x00 && r.bRequest == 9) set_cfg = r.wValue, set_cfg_res = r.result;
        if (r.bmRequestType == 0x80 && r.bRequest == 8) get_cfg_seen = 1;
        if (r.bmRequestType == 0x01 && r.bRequest == 11) set_if_seen = 1;
    }
    if (!found) stage(&st[S_OPEN], "open", ST_NOT_REACHED, "no device to open");
    else if (open_rc == IRECV_E_SUCCESS && cl) stage(&st[S_OPEN], "open", ST_OK, "irecv_open_with_ecid -> success; SET_CONFIGURATION %s%s", set_cfg >= 0 ? "sent" : "not sent (already configured)", set_cfg >= 0 ? (set_cfg_res >= 0 ? ", accepted" : ", failed") : "");
    else stage(&st[S_OPEN], "open", ST_FAILED, "irecv_open_with_ecid -> %d (%s); SET_CONFIGURATION %s", open_rc, irecv_err(open_rc),
               set_cfg >= 0 ? (set_cfg_res >= 0 ? "was accepted" : "was sent and failed") : "was not sent");

    /* Stages 3 and 4: what the library knows, and where each part came from. */
    if (cl) {
        int mode = -1;
        irecv_error_t me = irecv_get_mode(cl, &mode);
        const struct irecv_device_info *inf = irecv_get_device_info(cl);
        fprintf(out, "irecv probe: irecv_get_mode -> %d (%s), mode value 0x%04x (this is the USB product id)\n", me, irecv_err(me), (unsigned)mode);
        if (inf) {
            fprintf(out, "irecv probe: from the USB serial string, parsed by libirecovery: CPID 0x%04x%s CPRV 0x%02x%s CPFM 0x%02x%s SCEP 0x%02x%s BDID 0x%02x%s ECID 0x%016" PRIx64 "%s IBFL 0x%02x%s SRTG %s\n",
                    inf->cpid, inf->have_cpid ? "" : "(absent)", inf->cprv, inf->have_cprv ? "" : "(absent)", inf->cpfm, inf->have_cpfm ? "" : "(absent)", inf->scep,
                    inf->have_scep ? "" : "(absent)", inf->bdid, inf->have_bdid ? "" : "(absent)", inf->ecid, inf->have_ecid ? "" : "(absent)", inf->ibfl,
                    inf->have_ibfl ? "" : "(absent)", inf->srtg ? inf->srtg : "(none)");
            fprintf(out, "irecv probe: nonces libirecovery read from the device (USB string descriptor 1; informational, a device may not offer them): AP nonce %u bytes, SEP nonce %u bytes\n",
                    inf->ap_nonce_size, inf->sep_nonce_size);
            fprintf(out, "irecv probe: serial string: %s\n", inf->serial_string ? inf->serial_string : "(none)");
            if (inf->have_ecid && inf->ecid == ecid) stage(&st[S_INFO_STR], "info (serial string)", ST_OK, "ECID 0x%016" PRIx64 " read back from the string equals the one requested", inf->ecid);
            else stage(&st[S_INFO_STR], "info (serial string)", ST_FAILED, "ECID %s, 0x%016" PRIx64 " vs requested 0x%016" PRIx64, inf->have_ecid ? "differs" : "missing", inf->ecid, ecid);
        } else {
            stage(&st[S_INFO_STR], "info (serial string)", ST_FAILED, "irecv_get_device_info returned NULL");
        }
        irecv_device_t dev = NULL;
        irecv_error_t de = irecv_devices_get_device_by_client(cl, &dev);
        if (de == IRECV_E_SUCCESS && dev) {
            fprintf(out, "irecv probe: from libirecovery's own device table (looked up by CPID/BDID, not read from the device): product type %s, hardware model %s, display name %s\n",
                    dev->product_type, dev->hardware_model, dev->display_name);
            stage(&st[S_INFO_TABLE], "info (device table)", ST_OK, "table entry %s / %s", dev->product_type, dev->hardware_model);
        } else {
            stage(&st[S_INFO_TABLE], "info (device table)", ST_FAILED, "no table entry for this CPID/BDID (irecv_devices_get_device_by_client -> %d)", de);
        }
        int cr = irecv_close(cl);
        stage(&st[S_CLOSE], "close", cr == IRECV_E_SUCCESS ? ST_OK : ST_FAILED, "irecv_close -> %d (%s)", cr, irecv_err(cr));
    } else {
        stage(&st[S_INFO_STR], "info (serial string)", ST_NOT_REACHED, "no open handle");
        stage(&st[S_INFO_TABLE], "info (device table)", ST_NOT_REACHED, "no open handle");
        stage(&st[S_CLOSE], "close", ST_NOT_REACHED, "no open handle");
    }
    usleep(100000);
    read_state(usb, out, "after the library closed its handle", &cfg_after, &state_after);

    /* Stage 0 verdict: let late events (the open's bus reset) arrive: observation ends when no event has come for QUIET_MS, but never later than the overall cap.
       (With the device staying attached that is all this decides; a reconnect after a transfer must instead wait for its new ADD.) */
    {
        const int QUIET_MS = 1000, CAP_MS = 8000;
        for (;;) {
            int now = ms_since(&g_ev_t0), last = 0;
            pthread_mutex_lock(&g_ev_mu);
            if (g_nev > 0) last = g_ev[g_nev - 1].ms;
            pthread_mutex_unlock(&g_ev_mu);
            if (now >= CAP_MS || now - last >= QUIET_MS) break;
            usleep(50000);
        }
    }
    if (sub == IRECV_E_SUCCESS && ectx) irecv_device_event_unsubscribe(ectx);
    Ev evs[EV_MAX];
    int nev, dropped;
    pthread_mutex_lock(&g_ev_mu);
    nev = g_nev, dropped = g_ev_dropped;
    memcpy(evs, g_ev, sizeof evs);
    pthread_mutex_unlock(&g_ev_mu);
    for (int i = 0; i < nev; i++)
        fprintf(out, "irecv probe: event #%d at +%d ms: %s, mode value 0x%04x, ECID %s0x%016" PRIx64 "\n", i, evs[i].ms, evs[i].type == IRECV_DEVICE_ADD ? "ADD" : "REMOVE",
                (unsigned)evs[i].mode, evs[i].have_ecid ? "" : "(absent) ", evs[i].ecid);
    if (sub != IRECV_E_SUCCESS) {
        stage(&st[S_EVENTS], "events", ST_FAILED, "irecv_device_event_subscribe -> %d (%s)", (int)sub, irecv_err(sub));
    } else {
        char vnote[200];
        int vok = hv_irecv_events_verdict(evs, nev, dropped, ecid, vnote, sizeof vnote);
        stage(&st[S_EVENTS], "events", vok ? ST_OK : ST_FAILED, "%s", vnote);
    }

    /* Boundary: everything that went through the libusb API since before the subscription. Nothing may be a DFU download/upload/detach/abort/clear-status, a vendor
       request, or a bulk/interrupt transfer (firmware goes out as data). Reads (descriptors, configuration, GET_STATE, GET_STATUS) and SET_CONFIGURATION are allowed. */
    {
        int c1 = hv_libusb_ctrl_count(), d1 = hv_libusb_data_transfer_count();
        int forbidden = 0, lost = 0, getstatus = 0, total = c1 - ctrl0;
        for (int k = ctrl0; k < c1; k++) {
            HvCtrlRecord r;
            if (!hv_libusb_ctrl_get(k, &r)) { lost++; continue; }
            uint8_t type = (r.bmRequestType >> 5) & 3;
            if (type == 2) forbidden++;
            else if (type == 1 && (r.bRequest == 0 || r.bRequest == 1 || r.bRequest == 2 || r.bRequest == 4 || r.bRequest == 6)) forbidden++;
            else if (type == 1 && r.bRequest == 3) getstatus++;
        }
        if (forbidden || (d1 - data0) || lost)
            stage(&st[S_BOUNDARY], "boundary", ST_FAILED, "%d control request(s) through the API: %d forbidden (download/upload/detach/abort/clear-status/vendor), %d no longer in the log; %d data transfer(s) (must be 0)", total, forbidden, lost, d1 - data0);
        else
            stage(&st[S_BOUNDARY], "boundary", ST_OK, "%d control request(s) through the API (%d DFU GET_STATUS), no download/upload/detach/abort/clear-status/vendor request, %d data transfer(s); no TSS code is linked into this program", total, getstatus, d1 - data0);
    }

    fprintf(out, "irecv probe: control requests that went through the stand-in libusb API during the run (the bus reset and descriptor reads of its device enumeration are done below that API and are not listed), in order (seq, bmRequestType, bRequest, wValue, wIndex, wLength -> result):\n");
    int last = hv_libusb_ctrl_count();
    for (int k = first_req; k < last; k++) {
        HvCtrlRecord r;
        if (!hv_libusb_ctrl_get(k, &r)) { fprintf(out, "  #%d (no longer held)\n", k); continue; }
        fprintf(out, "  #%d %02x %02x %04x %04x %u -> %d  %s%s\n", r.seq, r.bmRequestType, r.bRequest, r.wValue, r.wIndex, r.wLength, r.result, req_name(&r), r.done ? "" : " (still in flight)");
    }
    fprintf(out, "irecv probe: GET_CONFIGURATION during open: %s; SET_INTERFACE during open: %s (for interface 0 the library only claims the interface, which the stand-in does without a request)\n",
            get_cfg_seen ? "yes" : "no", set_if_seen ? "yes" : "no");

    int failed = 0;
    fprintf(out, "irecv probe: ===== result =====\n");
    for (int i = 0; i < S_COUNT; i++) {
        const char *tag = st[i].r == ST_OK ? "ok" : st[i].r == ST_FAILED ? "FAILED" : "not reached";
        if (st[i].r == ST_FAILED) failed++;
        fprintf(out, "irecv probe: %-22s %-12s %s\n", st[i].name, tag, st[i].note);
    }
    fprintf(out, "irecv probe: before/after (expected, not a pass condition): GET_CONFIGURATION %d -> %d; DFU state %d -> %d\n", cfg_before, cfg_after, state_before, state_after);
    fprintf(out, "irecv probe: opened and closed only; no firmware was sent, no TSS request made, nothing downloaded. A pass says libirecovery can open this virtual device, not that a restore will work.\n");
    fprintf(out, "irecv probe: done, %d stage(s) failed\n", failed);
    fflush(out);
    return failed;
}

static void *thread_main(void *arg) {
    struct { HvVirtioUsb *u; uint64_t ecid; } *a = arg;
    hv_irecv_probe_run(a->u, a->ecid, stderr, 120000);
    free(a);
    return NULL;
}

void hv_irecv_probe_start(HvVirtioUsb *usb, uint64_t ecid) {
    struct { HvVirtioUsb *u; uint64_t ecid; } *a = malloc(sizeof *a);
    if (!a) return;
    a->u = usb, a->ecid = ecid;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, a) != 0) { free(a); return; }
    pthread_detach(t);
}
