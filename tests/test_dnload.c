/* host/dnload_probe.c (stage A0-A2 of the DFU download test) against a stand-in for the ROM's USB side. Linux CI only: it proves the probe sends exactly the
   planned requests in the planned order, with the planned bytes, and stops when it should. It says nothing about what the real ROM does with them.
   Scenarios (argv[1]):
     sync       - GET_STATE answers 2, DNLOAD is accepted, the next GET_STATE answers 3        -> returns 0
     idle-after - ... answers 5 after the DNLOAD                                                  -> returns 0
     error-after- ... answers 10 after the DNLOAD                                                 -> returns 0 (recorded, reported as dfuERROR)
     ignored    - ... answers 2 after the DNLOAD                                                  -> returns 0
     not-idle   - the first GET_STATE answers 8: nothing may be downloaded                        -> returns 2
     stall-dnload - the DNLOAD is STALLed: no second GET_STATE                                    -> returns 3
     silent-dnload - the DNLOAD gets no answer (timeout): no second GET_STATE                     -> returns 3
     stall-state - the first GET_STATE is STALLed                                                 -> returns 2
     stall-setcfg - SET_CONFIGURATION is STALLed                                                  -> returns 1
     configured - the device already reports configuration 1: no SET_CONFIGURATION                -> returns 0
     no-device  - the guest never goes on the bus                                                 -> returns -1
   Exit status 0 when the returned value and the requests the stand-in saw both match the scenario. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../host/dnload_probe.h"
#include "../devices/virtio_usb.h"

static HvVirtioUsb *g_u;
static volatile int g_stop;
static int g_cfg, g_state;
static const char *g_sc;
#define MAXLOG 64
static struct { uint8_t rt, rq; uint16_t val, idx, len; uint8_t data[64]; int dlen; } g_log[MAXLOG];
static int g_n, g_resets;

static void reply(uint8_t type, const void *d, uint32_t n) { hv_vusb_put_response(g_u, type, 0, d, n); }

static void handle_setup(const uint8_t *s, uint32_t len) {
    if (len < 8) { reply(HV_VUSB_RSP_STALL, NULL, 0); return; }
    uint8_t rt = s[0], rq = s[1];
    uint16_t val = (uint16_t)(s[2] | s[3] << 8), idx = (uint16_t)(s[4] | s[5] << 8), wlen = (uint16_t)(s[6] | s[7] << 8);
    if (g_n < MAXLOG) {
        g_log[g_n].rt = rt, g_log[g_n].rq = rq, g_log[g_n].val = val, g_log[g_n].idx = idx, g_log[g_n].len = wlen;
        g_log[g_n].dlen = len > 8 ? (int)(len - 8) : 0;
        if (g_log[g_n].dlen > 64) g_log[g_n].dlen = 64;
        memcpy(g_log[g_n].data, s + 8, (size_t)g_log[g_n].dlen);
        g_n++;
    }
    uint8_t b[8];
    if (rt == 0x80 && rq == 8) { b[0] = (uint8_t)g_cfg; reply(HV_VUSB_RSP_DATA, b, 1); return; }
    if (rt == 0x00 && rq == 9) {
        if (!strcmp(g_sc, "stall-setcfg")) { reply(HV_VUSB_RSP_STALL, NULL, 0); return; }
        g_cfg = val & 0xff;
        reply(HV_VUSB_RSP_DATA, NULL, 0);
        return;
    }
    if (rt == 0xA1 && rq == 5) { /* GET_STATE */
        if (!strcmp(g_sc, "stall-state") && g_state == 2) { reply(HV_VUSB_RSP_STALL, NULL, 0); return; }
        b[0] = (uint8_t)g_state;
        reply(HV_VUSB_RSP_DATA, b, 1);
        return;
    }
    if (rt == 0x21 && rq == 1) { /* DNLOAD */
        if (!strcmp(g_sc, "stall-dnload")) { reply(HV_VUSB_RSP_STALL, NULL, 0); return; }
        if (!strcmp(g_sc, "silent-dnload")) return; /* no answer at all */
        g_state = !strcmp(g_sc, "idle-after") ? 5 : !strcmp(g_sc, "error-after") ? 10 : !strcmp(g_sc, "ignored") ? 2 : 3;
        reply(HV_VUSB_RSP_DATA, NULL, 0);
        return;
    }
    reply(HV_VUSB_RSP_STALL, NULL, 0);
}

static void *rom(void *arg) {
    (void)arg;
    while (!g_stop) {
        HvVusbMsg m;
        if (!hv_vusb_take_request(g_u, &m)) { usleep(1000); continue; }
        if (m.type == HV_VUSB_REQ_RESET) { g_resets++; reply(HV_VUSB_RSP_RESET, NULL, 0); }
        else if (m.type == HV_VUSB_REQ_DATA && m.ep == 0) handle_setup(m.data, m.len);
        else reply(HV_VUSB_RSP_STALL, NULL, 0);
        free(m.data);
    }
    return NULL;
}

static int count(uint8_t rt, uint8_t rq) {
    int n = 0;
    for (int i = 0; i < g_n; i++) n += g_log[i].rt == rt && g_log[i].rq == rq;
    return n;
}

int main(int argc, char **argv) {
    g_sc = argc > 1 ? argv[1] : "sync";
    g_cfg = !strcmp(g_sc, "configured") ? 1 : 0;
    g_state = !strcmp(g_sc, "not-idle") ? 8 : 2;
    g_u = calloc(1, sizeof *g_u);
    pthread_mutex_init(&g_u->mu, NULL);
    pthread_cond_init(&g_u->cv, NULL);
    g_u->log = getenv("HV_VUSB_LOG") != NULL;
    pthread_t t;
    pthread_create(&t, NULL, rom, NULL);
    int nodev = !strcmp(g_sc, "no-device");
    if (!nodev) hv_vusb_set_online(g_u, 1);
    HvDnloadResult res;
    int r = hv_dnload_probe_run(g_u, stdout, nodev ? 1500 : 5000, &res);
    usleep(300000); /* anything the probe sent late would show up now */
    g_stop = 1;
    pthread_join(t, NULL);

    int want = !strcmp(g_sc, "not-idle") || !strcmp(g_sc, "stall-state") ? 2 : !strcmp(g_sc, "stall-dnload") || !strcmp(g_sc, "silent-dnload") ? 3 : !strcmp(g_sc, "stall-setcfg") ? 1 : nodev ? -1 : 0;
    int bad = r != want;
    /* things that must never be sent, in any scenario */
    if (count(0xA1, 3) || count(0x21, 6) || count(0x21, 4) || count(0xA1, 2) || count(0x21, 0) || count(0xA1, 0xff) || g_resets > 1) { printf("FORBIDDEN request seen (GET_STATUS/ABORT/CLR_STATUS/UPLOAD/DETACH or a second reset)\n"); bad = 1; }
    if (g_n > HV_DNLOAD_MAX_REQUESTS) { printf("more than %d requests\n", HV_DNLOAD_MAX_REQUESTS); bad = 1; }
    int dn = count(0x21, 1);
    if (dn > 1) { printf("more than one DNLOAD\n"); bad = 1; }
    for (int i = 0; i < g_n; i++) if (g_log[i].rt == 0x21 && g_log[i].rq == 1) {
        static const uint8_t z[16];
        if (g_log[i].val != 0 || g_log[i].idx != 0 || g_log[i].len != 16 || g_log[i].dlen != 16 || memcmp(g_log[i].data, z, 16)) { printf("DNLOAD is not block 0 / 16 zero bytes\n"); bad = 1; }
    }
    if (!strcmp(g_sc, "sync") || !strcmp(g_sc, "idle-after") || !strcmp(g_sc, "error-after") || !strcmp(g_sc, "ignored") || !strcmp(g_sc, "configured")) {
        int states = count(0xA1, 5);
        int setcfg = count(0x00, 9);
        if (dn != 1 || states != 2 || setcfg != (g_cfg == 1 && !strcmp(g_sc, "configured") ? 0 : 1) || g_resets != 1) { printf("unexpected request counts: dnload %d, get_state %d, set_cfg %d, resets %d\n", dn, states, setcfg, g_resets); bad = 1; }
        /* order: ... GET_STATE, DNLOAD, GET_STATE (last two entries) */
        if (g_n < 3 || !(g_log[g_n - 3].rt == 0xA1 && g_log[g_n - 2].rt == 0x21 && g_log[g_n - 1].rt == 0xA1)) { printf("wrong order at the end\n"); bad = 1; }
        int a2 = res.a2_state;
        int wantstate = !strcmp(g_sc, "idle-after") ? 5 : !strcmp(g_sc, "error-after") ? 10 : !strcmp(g_sc, "ignored") ? 2 : 3;
        if (a2 != wantstate || res.a0_state != 2) { printf("states a0 %d a2 %d\n", res.a0_state, a2); bad = 1; }
    }
    if (!strcmp(g_sc, "not-idle") && (dn != 0 || count(0xA1, 5) != 1)) { printf("data sent although the device was not in dfuIDLE\n"); bad = 1; }
    if ((!strcmp(g_sc, "stall-dnload") || !strcmp(g_sc, "silent-dnload")) && (dn != 1 || count(0xA1, 5) != 1)) { printf("after the failed DNLOAD another request was sent\n"); bad = 1; }
    if (!strcmp(g_sc, "stall-state") && (dn != 0 || count(0xA1, 5) != 1)) { printf("requests continued after the GET_STATE STALL\n"); bad = 1; }
    if (nodev && g_n != 0) { printf("requests sent although the guest never came online\n"); bad = 1; }
    printf("scenario %s: probe returned %d (expected %d), %d requests seen -> %s\n", g_sc, r, want, g_n, bad ? "EXPECTATION NOT MET" : "as expected");
    return bad ? 100 : 0;
}

/* virtio_usb.c's links to the guest-memory side of the machine: never reached here (see tests/test_irecv.c). */
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
