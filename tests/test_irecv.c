/* host/irecv_probe.c + the real libirecovery + host/libusb_shim.c against a stand-in for the ROM's USB side (a thread that answers the virtio-usb requests the
   way the descriptors captured from AVPBooter on a device say it does). Linux CI only: it proves the plumbing (listing, open, SET_CONFIGURATION, serial string,
   close, error reporting), not what the real ROM does. Scenario in argv[1]:
     ok            - answers everything; GET_CONFIGURATION starts at 0
     configured    - the device already reports configuration 1 (no SET_CONFIGURATION expected)
     stall-setcfg  - STALLs SET_CONFIGURATION
     wrong-ecid    - the serial string carries another ECID
     no-device     - the guest never goes on the bus (wait is short)
     reattach      - after the library closed its handle the device leaves the bus and comes back (new session): events must pair up ADD, REMOVE, ADD -> all pass
     gone          - after the close the device leaves and does not come back: the events stage must FAIL (last event is a REMOVE), nothing else
     inject-dnload - a side thread sends a DFU DNLOAD through the libusb API during the run: the boundary stage must FAIL
     inject-bulk   - a side thread submits a bulk transfer through the libusb API during the run: the boundary stage must FAIL
   Exit status: what hv_irecv_probe_run returned (failed stages), 100 + n if the scenario's expectation on the control log is not met. */
#define _GNU_SOURCE 1
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "../host/irecv_probe.h"
#include "../devices/virtio_usb.h"

static HvVirtioUsb *g_u;
static volatile int g_stop;
static int g_cfg, g_stall_setcfg;
static uint64_t g_dev_ecid;
static int g_setcfg_seen, g_setcfg_value;
static const char *g_sc;
static int g_getstate_seen;   /* DFU GET_STATE requests the ROM has answered: the 1st is before the library, the 2nd after it closed its handle */

/* Runs on its own thread once the probe has closed the library's handle (the 2nd GET_STATE), so the probe's stages are over and only its final wait is left. */
static void *after_close(void *arg) {
    (void)arg;
    if (!strcmp(g_sc, "reattach") || !strcmp(g_sc, "gone")) {
        usleep(150000);
        hv_vusb_set_online(g_u, 0);
        if (!strcmp(g_sc, "reattach")) { usleep(300000); hv_vusb_set_online(g_u, 1); }
    } else if (!strcmp(g_sc, "inject-dnload") || !strcmp(g_sc, "inject-bulk")) {
        libusb_device **l = NULL;
        ssize_t n = libusb_get_device_list(NULL, &l);
        libusb_device_handle *h = NULL;
        if (n > 0 && libusb_open(l[0], &h) == 0) {
            unsigned char b[16] = {0};
            if (!strcmp(g_sc, "inject-dnload")) libusb_control_transfer(h, 0x21, 1, 0, 0, b, sizeof b, 500);   /* DFU_DNLOAD, block 0 */
            else { int got = 0; libusb_bulk_transfer(h, 0x04, b, sizeof b, &got, 500); }
            libusb_close(h);
        }
        if (l) libusb_free_device_list(l, 1);
    }
    return NULL;
}

static const uint8_t dev_desc[18] = {0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0xac, 0x05, 0x27, 0x12, 0x00, 0x00, 0x02, 0x03, 0x04, 0x01};
static const uint8_t cfg_desc[25] = {9, 2, 25, 0, 1, 1, 5, 0x80, 0xFA, 9, 4, 0, 0, 0, 0xFE, 1, 0, 0, 7, 0x21, 0x0D, 0x10, 0x27, 0x00, 0x10};

static int utf16(uint8_t *o, const char *s) {
    int n = 2;
    for (; *s; s++) o[n++] = (uint8_t)*s, o[n++] = 0;
    o[0] = (uint8_t)n, o[1] = 3;
    return n;
}

static void reply(uint8_t type, const void *d, uint32_t n) { hv_vusb_put_response(g_u, type, 0, d, n); }

static void handle_setup(const uint8_t *s, uint32_t len) {
    if (len < 8) { reply(HV_VUSB_RSP_STALL, NULL, 0); return; }
    uint8_t rt = s[0], rq = s[1];
    uint16_t val = (uint16_t)(s[2] | s[3] << 8), idx = (uint16_t)(s[4] | s[5] << 8), wlen = (uint16_t)(s[6] | s[7] << 8);
    (void)idx;
    uint8_t buf[256];
    int n = -1;
    if (rt == 0x80 && rq == 6) { /* GET_DESCRIPTOR */
        int t = val >> 8, i = val & 0xff;
        if (t == 1) memcpy(buf, dev_desc, 18), n = 18;
        else if (t == 2 && i == 0) memcpy(buf, cfg_desc, 25), n = 25;
        else if (t == 3) {
            char tmp[160];
            const char *str = NULL;
            if (i == 2) str = "Apple Inc.";
            else if (i == 3) str = "Apple Mobile Device (DFU Mode)";
            else if (i == 4) { snprintf(tmp, sizeof tmp, "SDOM:01 CPID:FE00 CPRV:00 CPFM:03 SCEP:01 BDID:20 ECID:%016" PRIX64 " IBFL:3C SRTG:[iBoot-10151.140.19]", g_dev_ecid); str = tmp; }
            else if (i == 5) str = "DFU";
            else if (i == 1) { snprintf(tmp, sizeof tmp, "CPID:FE00 NONC:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff SNON:00112233445566778899aabbccddeeff00112233"); str = tmp; }
            if (str) n = utf16(buf, str);
        }
        if (n > wlen) n = wlen;
    } else if (rt == 0x80 && rq == 0) { buf[0] = buf[1] = 0; n = 2; }
    else if (rt == 0x80 && rq == 8) { buf[0] = (uint8_t)g_cfg; n = 1; }
    else if (rt == 0x00 && rq == 9) {
        g_setcfg_seen++, g_setcfg_value = val & 0xff;
        if (g_stall_setcfg) { reply(HV_VUSB_RSP_STALL, NULL, 0); return; }
        g_cfg = val & 0xff;
        reply(HV_VUSB_RSP_DATA, NULL, 0);
        return;
    }
    else if (rt == 0xA1 && rq == 5) {
        buf[0] = 2; n = 1;
        if (++g_getstate_seen == 2) { pthread_t t; if (pthread_create(&t, NULL, after_close, NULL) == 0) pthread_detach(t); }
    }
    else if (rt == 0xA1 && rq == 3) { const uint8_t st[6] = {0, 0x32, 0, 0, 2, 0}; memcpy(buf, st, 6); n = 6; }
    if (n < 0) { reply(HV_VUSB_RSP_STALL, NULL, 0); return; }
    reply(HV_VUSB_RSP_DATA, buf, (uint32_t)n);
}

static void *rom(void *arg) {
    (void)arg;
    while (!g_stop) {
        HvVusbMsg m;
        if (!hv_vusb_take_request(g_u, &m)) { usleep(1000); continue; }
        if (m.type == HV_VUSB_REQ_RESET) { g_cfg = g_cfg == 1 && 0 ? 1 : g_cfg; reply(HV_VUSB_RSP_RESET, NULL, 0); }
        else if (m.type == HV_VUSB_REQ_DATA && m.ep == 0) handle_setup(m.data, m.len);
        else reply(HV_VUSB_RSP_STALL, NULL, 0);
        free(m.data);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "ok";
    g_sc = sc;
    uint64_t ecid = 0x2d2a8d94f32ba4d2ull;
    g_dev_ecid = !strcmp(sc, "wrong-ecid") ? 0x1111222233334444ull : ecid;
    g_cfg = !strcmp(sc, "configured") ? 1 : 0;
    g_stall_setcfg = !strcmp(sc, "stall-setcfg");
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    g_u->log = getenv("HV_VUSB_LOG") != NULL;
    pthread_t t;
    pthread_create(&t, NULL, rom, NULL);
    if (strcmp(sc, "no-device")) hv_vusb_set_online(g_u, 1);
    int r = hv_irecv_probe_run(g_u, ecid, stdout, !strcmp(sc, "no-device") ? 1500 : 5000);
    g_stop = 1;
    pthread_join(t, NULL);
    int bad = 0;
    if (!strcmp(sc, "ok")) bad = !(g_setcfg_seen == 1 && g_setcfg_value == 1 && r == 0);
    else if (!strcmp(sc, "configured")) bad = !(g_setcfg_seen == 0 && r == 0);
    else if (!strcmp(sc, "stall-setcfg")) bad = !(g_setcfg_seen == 1 && r >= 1);
    else if (!strcmp(sc, "wrong-ecid")) bad = !(g_setcfg_seen == 0 && r >= 1);
    else if (!strcmp(sc, "no-device")) bad = !(r == -1);
    else if (!strcmp(sc, "reattach")) bad = !(r == 0);
    else if (!strcmp(sc, "gone") || !strcmp(sc, "inject-dnload") || !strcmp(sc, "inject-bulk")) bad = !(r == 1);
    printf("scenario %s: probe returned %d, SET_CONFIGURATION seen %d times (value %d) -> %s\n", sc, r, g_setcfg_seen, g_setcfg_value, bad ? "EXPECTATION NOT MET" : "as expected");
    return bad ? 100 + (r > 0 ? r : 0) : 0;
}

/* virtio_usb.c's links to the guest-memory side of the machine. This test never builds a machine: the stand-in ROM talks to the queues' message lists directly
   (hv_vusb_take_request / hv_vusb_put_response), so these are never reached. */
#include "../devices/pcie.h"
int hv_virtq_pop(HvVirtio *v, int q, HvVirtqElem *e) { (void)v, (void)q, (void)e; return 0; }
void hv_virtq_push(HvVirtio *v, int q, const HvVirtqElem *e, uint32_t w) { (void)v, (void)q, (void)e, (void)w; }
void hv_virtio_interrupt(HvVirtio *v) { (void)v; }
void hv_virtio_init(HvVirtio *v, HvMachine *m, int id, uint32_t cc, int nq, uint64_t f) { (void)v, (void)m, (void)id, (void)cc, (void)nq, (void)f; }
uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t o, void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t o, const void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_writable(const HvVirtqElem *e) { (void)e; return 0; }
int hv_pcie_attach(HvPcie *p, HvPciDev *d) { (void)p, (void)d; return -1; }
void hv_machine_wake(HvMachine *m) { (void)m; }
