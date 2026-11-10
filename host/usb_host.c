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

int hv_usb_reset(HvVirtioUsb *d, int timeout_ms) {
    if (hv_vusb_send(d, HV_VUSB_REQ_RESET, 0, NULL, 0) != 0) return HV_USB_EGONE;
    HvVusbMsg m;
    int r = hv_vusb_recv(d, 0xff, &m, timeout_ms);
    if (r == 0) return HV_USB_ETIMEOUT;
    if (r < 0) return HV_USB_EGONE;
    int ok = m.type == HV_VUSB_RSP_RESET;
    free(m.data);
    return ok ? 0 : HV_USB_EIO;
}

int hv_usb_control(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                   void *data, uint16_t wLength, int timeout_ms) {
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
    HvUsbConfigInfo ci;
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
