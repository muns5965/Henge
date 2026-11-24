#include "usb_host.h"
#include "usb_desc.c" /* included, not compiled separately: the Apple Makefiles list usb_host.c only */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int wait_ep(HvVirtioUsb *d, uint8_t ep, void *buf, uint32_t max, int timeout_ms) {
    HvVusbMsg m;
    int r = hv_vusb_recv(d, ep, &m, timeout_ms);
    if (r == 0) return HV_USB_ETIMEOUT;
    if (r < 0) return HV_USB_EGONE;
    int rc;
    switch (m.type) {
    case HV_VUSB_RSP_DATA: {
        uint32_t n = m.len < max ? m.len : max;
        if (n && buf) memcpy(buf, m.data, n);
        rc = (int)n;
        break;
    }
    case HV_VUSB_RSP_STALL: rc = HV_USB_ESTALL; break;
    case HV_VUSB_RSP_RESET: rc = HV_USB_EIO; break; /* a reset answer where data was expected */
    default: rc = HV_USB_EIO; break;
    }
    free(m.data);
    return rc;
}

int64_t hv_usb_wait_online(HvVirtioUsb *d, int timeout_ms) {
    for (int t = 0; t <= timeout_ms; t += 10) {
        uint32_t s;
        if (hv_vusb_online(d, &s)) return s;
        usleep(10000);
    }
    return -1;
}

/* Strict mode. The lock order is g_cmu, then the device's mu (taken inside hv_vusb_send_session / hv_vusb_recv_ex); hv_vusb_set_online takes mu only. g_cmu guards
   state check, reservation, queueing and the settling transition, and is NOT held while waiting for an answer. Each transition is made by the request's own caller. */
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER;

int hv_usb_set_strict(HvVirtioUsb *d, int on) {
    pthread_mutex_lock(&g_cmu);
    int rc = 0;
    if (d->cbusy > 0) rc = HV_USB_EBUSY;
    else if (d->cstate == HV_CTRL_UNCERTAIN) rc = HV_USB_EUNCERTAIN;
    else if (d->cstate != HV_CTRL_CLEAR) rc = HV_USB_EBUSY;
    else d->cstrict = on != 0;
    pthread_mutex_unlock(&g_cmu);
    return rc;
}
int hv_usb_ctrl_state(HvVirtioUsb *d) {
    pthread_mutex_lock(&g_cmu);
    int s = d->cstate;
    pthread_mutex_unlock(&g_cmu);
    return s;
}
int hv_usb_ctrl_reacquire(HvVirtioUsb *d) {
    pthread_mutex_lock(&g_cmu);
    int rc = 0;
    if (d->cbusy > 0 || d->cstate == HV_CTRL_PENDING || d->cstate == HV_CTRL_RESETTING) rc = HV_USB_EBUSY;
    else if (d->cstate != HV_CTRL_UNCERTAIN) rc = HV_USB_ESTATE;
    else d->cstate = HV_CTRL_CLEAR, d->cgen++;
    pthread_mutex_unlock(&g_cmu);
    return rc;
}

/* Every control / RESET call is counted while it is inside, and reads the mode once, at entry: set_strict cannot change the mode under a call. */
static int call_enter(HvVirtioUsb *d) {
    pthread_mutex_lock(&g_cmu);
    int strict = d->cstrict;
    d->cbusy++;
    pthread_mutex_unlock(&g_cmu);
    return strict;
}
static void call_leave(HvVirtioUsb *d) {
    pthread_mutex_lock(&g_cmu);
    d->cbusy--;
    pthread_mutex_unlock(&g_cmu);
}

/* Reserve the endpoint and queue the request in one step. Returns 0 (state is now `kind`), or the error to hand back (nothing was queued). */
static int ctrl_begin(HvVirtioUsb *d, int kind, uint8_t type, const void *req, uint32_t len, uint32_t *session, uint32_t *gen) {
    pthread_mutex_lock(&g_cmu);
    int rc = 0;
    if (d->cstate == HV_CTRL_UNCERTAIN) rc = HV_USB_EUNCERTAIN;
    else if (d->cstate != HV_CTRL_CLEAR) rc = HV_USB_EBUSY;
    else {
        d->cstate = kind;
        *gen = ++d->cgen;
        if (hv_vusb_send_session(d, type, 0, req, len, session) != 0) {   /* every failure of the send is before the queue: nothing was delivered */
            d->cstate = HV_CTRL_CLEAR;
            rc = HV_USB_EGONE;
        }
    }
    pthread_mutex_unlock(&g_cmu);
    return rc;
}

static int ctrl_strict(HvVirtioUsb *d, uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, void *data, uint16_t wlen, int timeout_ms, volatile int *cancel) {
    int in = (rt & 0x80) != 0;
    uint32_t len = 8 + (in ? 0 : wlen);
    if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE)) return HV_USB_ECANCELLED;   /* before anything is sent: no state */
    uint8_t *req = malloc(len);
    if (!req) return HV_USB_EIO;
    req[0] = rt, req[1] = rq;
    memcpy(req + 2, &val, 2);
    memcpy(req + 4, &idx, 2);
    memcpy(req + 6, &wlen, 2);
    if (!in && wlen) memcpy(req + 8, data, wlen);
    uint32_t session = 0, gen = 0;
    int rc = ctrl_begin(d, HV_CTRL_PENDING, HV_VUSB_REQ_DATA, req, len, &session, &gen);
    free(req);
    if (rc) return rc;
    HvVusbMsg m = {0};
    int rr = hv_vusb_recv_ex(d, 0, 0, &m, timeout_ms, session, cancel);
    pthread_mutex_lock(&g_cmu);
    int out;
    if (d->cstate != HV_CTRL_PENDING || d->cgen != gen) {   /* reacquired from under us: the answer, if any, belongs to nobody */
        out = HV_USB_EIO;
    } else if (rr == 1 && m.type == HV_VUSB_RSP_DATA) {
        uint32_t n = m.len < (in ? wlen : 0u) ? m.len : (in ? wlen : 0u);
        if (n && data) memcpy(data, m.data, n);
        out = in ? (int)n : wlen;
        d->cstate = HV_CTRL_CLEAR;
    } else if (rr == 1 && m.type == HV_VUSB_RSP_STALL) {
        out = HV_USB_ESTALL;
        d->cstate = HV_CTRL_CLEAR;
    } else {   /* anything that is not a settled answer */
        out = rr == 0 ? HV_USB_ETIMEOUT : rr == -1 ? HV_USB_EGONE : rr == -2 ? HV_USB_ECANCELLED : HV_USB_EIO;
        d->cstate = HV_CTRL_UNCERTAIN;
    }
    pthread_mutex_unlock(&g_cmu);
    if (rr == 1) free(m.data);
    return out;
}

static int reset_strict(HvVirtioUsb *d, int timeout_ms) {
    uint32_t session = 0, gen = 0;
    int rc = ctrl_begin(d, HV_CTRL_RESETTING, HV_VUSB_REQ_RESET, NULL, 0, &session, &gen);
    if (rc) return rc;
    HvVusbMsg m = {0};
    int rr = hv_vusb_recv_ex(d, 0, 1, &m, timeout_ms, session, NULL);
    pthread_mutex_lock(&g_cmu);
    int out;
    if (d->cstate != HV_CTRL_RESETTING || d->cgen != gen) out = HV_USB_EIO;
    else if (rr == 1) out = 0, d->cstate = HV_CTRL_CLEAR;
    else out = rr == 0 ? HV_USB_ETIMEOUT : HV_USB_EGONE, d->cstate = HV_CTRL_UNCERTAIN;
    pthread_mutex_unlock(&g_cmu);
    if (rr == 1) free(m.data);
    return out;
}

static int reset_legacy(HvVirtioUsb *d, int timeout_ms) {
    if (hv_vusb_send(d, HV_VUSB_REQ_RESET, 0, NULL, 0) != 0) return HV_USB_EGONE;
    HvVusbMsg m;
    int r = hv_vusb_recv(d, 0xff, &m, timeout_ms);
    if (r == 0) return HV_USB_ETIMEOUT;
    if (r < 0) return HV_USB_EGONE;
    int ok = m.type == HV_VUSB_RSP_RESET;
    free(m.data);
    return ok ? 0 : HV_USB_EIO;
}

int hv_usb_reset(HvVirtioUsb *d, int timeout_ms) {
    int strict = call_enter(d);
    int r = strict ? reset_strict(d, timeout_ms) : reset_legacy(d, timeout_ms);
    call_leave(d);
    return r;
}

int hv_usb_control(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                   void *data, uint16_t wLength, int timeout_ms) {
    return hv_usb_control_c(d, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout_ms, NULL);
}

static int ctrl_legacy(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength, int timeout_ms) {
    int in = (bmRequestType & 0x80) != 0;
    uint32_t len = 8 + (in ? 0 : wLength);
    uint8_t *req = malloc(len);
    if (!req) return HV_USB_EIO;
    req[0] = bmRequestType, req[1] = bRequest;
    memcpy(req + 2, &wValue, 2);
    memcpy(req + 4, &wIndex, 2);
    memcpy(req + 6, &wLength, 2);
    if (!in && wLength) memcpy(req + 8, data, wLength);
    int r = hv_vusb_send(d, HV_VUSB_REQ_DATA, 0, req, len);
    free(req);
    if (r != 0) return HV_USB_EGONE;
    r = wait_ep(d, 0, in ? data : NULL, in ? wLength : 0, timeout_ms);
    if (r < 0) return r;
    return in ? r : wLength;
}

int hv_usb_control_c(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                     void *data, uint16_t wLength, int timeout_ms, volatile int *cancel) {
    int strict = call_enter(d);
    int r = strict ? ctrl_strict(d, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout_ms, cancel)
                   : ctrl_legacy(d, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout_ms);
    call_leave(d);
    return r;
}

int hv_usb_bulk(HvVirtioUsb *d, uint8_t ep, void *data, uint32_t len, int timeout_ms) {
    int in = (ep & 0x80) != 0;
    int r = in ? hv_vusb_send_in(d, ep, len) : hv_vusb_send(d, HV_VUSB_REQ_DATA, ep, data, len);
    if (r != 0) return HV_USB_EGONE;
    r = wait_ep(d, ep, in ? data : NULL, in ? len : 0, timeout_ms);
    if (r < 0) return r;
    return in ? r : (int)len;
}

static void print_string(HvVirtioUsb *d, uint8_t idx) {
    if (!idx) return;
    uint8_t buf[256];
    int n = hv_usb_control(d, 0x80, 6, (uint16_t)(0x300 | idx), 0x409, buf, 255, 2000);
    if (n < 2) {
        fprintf(stderr, "usb probe: string %u -> %d\n", idx, n);
        return;
    }
    char s[128];
    int k = 0;
    for (int i = 2; i + 1 < n && k < 127; i += 2) s[k++] = buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '?';
    s[k] = 0;
    fprintf(stderr, "usb probe: string %u = \"%s\"\n", idx, s);
}

/* [2026-11-25, explicitly approved] read-only recovery-environment query. It repeats exactly what libirecovery's irecv_getenv() puts on the wire (third_party/libirecovery:
   a vendor control OUT, bRequest 0, "getenv <name>" plus its NUL; then a vendor control IN, bRequest 0, 255 bytes) but through hv_usb_control directly, so no libirecovery or
   libusb object is linked into the clone runner. Only these five names are asked, only when the device reports a recovery-mode PID (0x1280-0x1283), and nothing here sends
   setenv/saveenv/reboot/DFU/bulk. The answer cannot separate "unset" from "no answer" on the wire (an empty reply is both), so those are reported as one case; a failed transfer is
   reported as a failure. */
static int g_getenv_enabled;
void hv_usb_probe_enable_getenv(int on) { g_getenv_enabled = on; }

/* One "getenv <name>": 0 = answered (out holds the text, possibly empty), -1 = a transfer failed. */
static int getenv_one(HvVirtioUsb *d, const char *name, char *out, size_t outsz, int *sr_out, int *rr_out) {
    char cmd[64];
    int cl = snprintf(cmd, sizeof cmd, "getenv %s", name);
    out[0] = 0;
    int sr = hv_usb_control(d, 0x40, 0, 0, 0, cmd, (uint16_t)(cl + 1), 2000);
    *sr_out = sr; *rr_out = 0;
    if (sr < 0) return -1;
    char resp[256];
    memset(resp, 0, sizeof resp);
    int rr = hv_usb_control(d, 0xC0, 0, 0, 0, resp, 255, 2000);
    *rr_out = rr;
    if (rr < 0) return -1;
    resp[255] = 0;
    size_t k = 0;
    for (int j = 0; j < rr && j < 255 && resp[j] && k + 1 < outsz && k < 250; j++) out[k++] = (resp[j] >= 32 && resp[j] < 127) ? resp[j] : '?';
    out[k] = 0;
    return 0;
}

static void getenv_query(HvVirtioUsb *d, uint16_t pid) {
    if (pid < 0x1280 || pid > 0x1283) {
        fprintf(stderr, "usb getenv: skipped: PID 0x%04x is not a recovery-mode PID\n", pid);
        return;
    }
    static const char *const names[] = {"auto-boot", "boot-command", "boot-args", "bootdelay", "build-version"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        char out[256];
        int sr, rr;
        if (getenv_one(d, names[i], out, sizeof out, &sr, &rr) != 0) fprintf(stderr, "usb getenv: %s: QUERY FAILED (send %d, read %d)\n", names[i], sr, rr);
        else if (!out[0]) fprintf(stderr, "usb getenv: %s: EMPTY (unset, or no answer; cannot be told apart) (send %d, read %d)\n", names[i], sr, rr);
        else fprintf(stderr, "usb getenv: %s = \"%s\" (send %d, read %d)\n", names[i], out, sr, rr);
    }
}

/* [2026-11-25, explicitly approved, ONE clone only] mode E: the single fixed sequence getenv auto-boot -> setenv auto-boot true -> getenv -> saveenv -> getenv. Each step
   must succeed before the next is sent; it stops at the first failure; nothing else is ever sent (no reboot, no other variable, no free-form command), and it runs once per
   process, at the first recovery prompt. */
static int g_setenv_enabled;
void hv_usb_probe_enable_autoboot_fix(int on) { g_setenv_enabled = on; }

static int send_fixed(HvVirtioUsb *d, const char *cmd, const char *what) {
    int r = hv_usb_control(d, 0x40, 0, 0, 0, (void *)cmd, (uint16_t)(strlen(cmd) + 1), 2000);
    fprintf(stderr, "usb autoboot-fix: %s sent -> %d\n", what, r);
    return r < 0 ? -1 : 0;
}

static void autoboot_fix(HvVirtioUsb *d, uint16_t pid) {
    char v[256];
    int sr, rr;
    if (pid < 0x1280 || pid > 0x1283) { fprintf(stderr, "usb autoboot-fix: STOP: PID 0x%04x is not a recovery-mode PID, nothing sent\n", pid); return; }
    if (getenv_one(d, "auto-boot", v, sizeof v, &sr, &rr) != 0) { fprintf(stderr, "usb autoboot-fix: STOP: first getenv auto-boot failed (send %d, read %d), nothing changed\n", sr, rr); return; }
    fprintf(stderr, "usb autoboot-fix: step 1 getenv auto-boot = \"%s\"\n", v);
    if (!strcmp(v, "true")) { fprintf(stderr, "usb autoboot-fix: already true, nothing sent\n"); return; }
    if (strcmp(v, "false")) { fprintf(stderr, "usb autoboot-fix: STOP: unexpected current value, nothing changed\n"); return; }
    if (send_fixed(d, "setenv auto-boot true", "step 2 setenv auto-boot true") != 0) { fprintf(stderr, "usb autoboot-fix: STOP at step 2\n"); return; }
    usleep(300000);
    if (getenv_one(d, "auto-boot", v, sizeof v, &sr, &rr) != 0) { fprintf(stderr, "usb autoboot-fix: STOP at step 3: getenv failed (send %d, read %d); saveenv NOT sent\n", sr, rr); return; }
    fprintf(stderr, "usb autoboot-fix: step 3 getenv auto-boot = \"%s\"\n", v);
    if (strcmp(v, "true")) { fprintf(stderr, "usb autoboot-fix: STOP at step 3: value did not change; saveenv NOT sent\n"); return; }
    if (send_fixed(d, "saveenv", "step 4 saveenv") != 0) { fprintf(stderr, "usb autoboot-fix: STOP at step 4\n"); return; }
    usleep(500000);
    if (getenv_one(d, "auto-boot", v, sizeof v, &sr, &rr) != 0) fprintf(stderr, "usb autoboot-fix: step 5 getenv after saveenv FAILED (send %d, read %d); persistence is shown only by the next normal boot\n", sr, rr);
    else fprintf(stderr, "usb autoboot-fix: step 5 getenv auto-boot after saveenv = \"%s\" (memory value only; persistence is shown only by the next normal boot)\n", v);
    fprintf(stderr, "usb autoboot-fix: DONE (no reboot sent)\n");
}

static void *probe(void *arg) {
    HvVirtioUsb *d = arg;
    int64_t s = hv_usb_wait_online(d, 120000);
    fprintf(stderr, "usb probe: online (session %lld)\n", (long long)s);
    if (s < 0) return NULL;
    usleep(200000);
    fprintf(stderr, "usb probe: reset -> %d\n", hv_usb_reset(d, 2000));
    uint8_t dd[18] = {0};
    int n = hv_usb_control(d, 0x80, 6, 0x100, 0, dd, 18, 2000);
    fprintf(stderr, "usb probe: device descriptor -> %d:", n);
    for (int i = 0; i < (n > 0 ? n : 0); i++) fprintf(stderr, " %02x", dd[i]);
    fprintf(stderr, "\n");
    if (n == 18) {
        print_string(d, dd[14]);
        print_string(d, dd[15]);
        print_string(d, dd[16]);
    }
    if (n != 18) return NULL;
    if (g_setenv_enabled) {
        getenv_query(d, (uint16_t)(dd[10] | (dd[11] << 8)));
        autoboot_fix(d, (uint16_t)(dd[10] | (dd[11] << 8)));
    } else if (g_getenv_enabled) {
        /* Tried a few times: the guest may still be on its way into the recovery prompt when the bus first comes up. */
        for (int t = 0; t < 3; t++) {
            getenv_query(d, (uint16_t)(dd[10] | (dd[11] << 8)));
            if (t < 2) usleep(1000000);
        }
    }
    /* The rest asks questions: configuration descriptor, standard status, DFU GET_STATE and, only from dfuIDLE, DFU GET_STATUS (which can advance some other
       DFU states, so it is not a harmless read in general). Nothing here sends SET_CONFIGURATION, DNLOAD, ABORT or CLRSTATUS. */
    uint8_t cfg[512] = {0};
    int cn = hv_usb_control(d, 0x80, 6, 0x200, 0, cfg, 9, 2000);
    if (cn >= 9) {
        int total = cfg[2] | (cfg[3] << 8);
        if (total > 9 && total <= (int)sizeof cfg) cn = hv_usb_control(d, 0x80, 6, 0x200, 0, cfg, (uint16_t)total, 2000);
        fprintf(stderr, "usb probe: configuration descriptor -> %d bytes (wTotalLength %d)\n", cn, total);
    } else {
        fprintf(stderr, "usb probe: configuration descriptor -> %d\n", cn);
    }
    if (cn > 0) hv_usb_hexdump(stderr, "usb probe: configuration descriptor raw", cfg, cn);
    HvUsbConfigInfo ci;
    if (cn >= 9 && cfg[6]) print_string(d, cfg[6]); /* iConfiguration; a failed lookup only prints a line */
    if (cn >= 9 && hv_usb_describe_config(cfg, cn, &ci, stderr) == 0) {
        if (ci.dfu_istring) print_string(d, (uint8_t)ci.dfu_istring);
        uint8_t st[2] = {0xff, 0xff};
        int r = hv_usb_control(d, 0x80, 0, 0, 0, st, 2, 2000);
        fprintf(stderr, "usb probe: standard GET_STATUS(device) -> %d: %02x %02x\n", r, st[0], st[1]);
        uint8_t cv = 0xff;
        r = hv_usb_control(d, 0x80, 8, 0, 0, &cv, 1, 2000);
        fprintf(stderr, "usb probe: GET_CONFIGURATION -> %d: %u\n", r, cv);
        if (ci.dfu_interface >= 0) {
            uint8_t state = 0xff;
            r = hv_usb_control(d, 0xA1, 5, 0, (uint16_t)ci.dfu_interface, &state, 1, 2000);
            fprintf(stderr, "usb probe: DFU GET_STATE(interface %d) -> %d: %u (%s)\n", ci.dfu_interface, r, state, r == 1 ? hv_dfu_state_name(state) : "?");
            const char *why = "";
            if (hv_dfu_should_get_status(r, state, &why)) {
                uint8_t ds[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
                r = hv_usb_control(d, 0xA1, 3, 0, (uint16_t)ci.dfu_interface, ds, 6, 2000);
                fprintf(stderr, "usb probe: DFU GET_STATUS(interface %d) -> %d:", ci.dfu_interface, r);
                for (int i = 0; i < (r > 0 ? r : 0); i++) fprintf(stderr, " %02x", ds[i]);
                HvDfuStatus st;
                if (hv_dfu_parse_status(r, ds, &st) == 0)
                    fprintf(stderr, "  status=%d (%s) pollTimeout=%d ms state=%d (%s) iString=%d\n", st.status, hv_dfu_status_name(st.status), st.poll_timeout_ms, st.state,
                            hv_dfu_state_name(st.state), st.istring);
                else
                    fprintf(stderr, "  (not a 6-byte answer; ignored)\n");
            } else {
                fprintf(stderr, "usb probe: DFU GET_STATUS skipped: %s\n", why);
            }
        } else {
            fprintf(stderr, "usb probe: no DFU interface (class 0xFE subclass 1) in the configuration\n");
        }
    }
    return NULL;
}

void hv_usb_probe_start(HvVirtioUsb *d) {
    pthread_t t;
    pthread_create(&t, NULL, probe, d);
    pthread_detach(t);
}
