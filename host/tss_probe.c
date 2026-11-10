#define _GNU_SOURCE 1
#include "tss_probe.h"
#include "tss_probe_core.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libirecovery.h>
#include <libusb.h>

#define IPSW_URL "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw"
static volatile int g_done;
int hv_tss_probe_finished(void) { return g_done; }

static void *thread_main(void *arg) {
    struct { HvVirtioUsb *u; uint64_t ecid; } *a = arg;
    FILE *out = stdout;
    fprintf(out, "tss probe: waiting for the guest on the virtual USB bus\n");
    int64_t sess = hv_usb_wait_online(a->u, 120000);
    if (sess < 0) { fprintf(out, "tss probe: the guest never appeared on the bus: nothing sent\n"); goto done; }
    usleep(200000);
    hv_libusb_shim_attach(a->u);
    irecv_client_t cl = NULL;
    if (irecv_open_with_ecid(&cl, a->ecid) != IRECV_E_SUCCESS || !cl) { fprintf(out, "tss probe: libirecovery could not open the device: nothing sent\n"); goto done; }
    const struct irecv_device_info *inf = irecv_get_device_info(cl);
    uint8_t ap[64], sep[64];
    size_t apl = 0, sepl = 0;
    int ecid_ok = 0;
    if (inf) {
        ecid_ok = inf->have_ecid && inf->ecid == a->ecid;
        if (inf->ap_nonce && inf->ap_nonce_size && inf->ap_nonce_size <= sizeof ap) memcpy(ap, inf->ap_nonce, apl = inf->ap_nonce_size);
        if (inf->sep_nonce && inf->sep_nonce_size && inf->sep_nonce_size <= sizeof sep) memcpy(sep, inf->sep_nonce, sepl = inf->sep_nonce_size);
    }
    irecv_close(cl);
    fprintf(out, "tss probe: device read: ECID %s, AP nonce %zu bytes, SEP nonce %zu bytes (values not printed)\n", ecid_ok ? "matches the VM's" : "DOES NOT MATCH or is absent", apl, sepl);
    if (!ecid_ok || !apl || !sepl) { fprintf(out, "tss probe: without the ROM's own ECID and both nonces nothing is sent\n"); goto done; }
    HvTssProbeCfg c; memset(&c, 0, sizeof c);
    c.ipsw_url = IPSW_URL; c.ecid = a->ecid; c.ap_nonce = ap; c.ap_len = apl; c.sep_nonce = sep; c.sep_len = sepl; c.send = 1;
    int rc = hv_tss_probe_core(&c, out);
    fprintf(out, "tss probe: finished, result code %d (0 = ticket issued, 1 = sent and not issued, 2 = nothing sent)\n", rc);
    memset(ap, 0, sizeof ap); memset(sep, 0, sizeof sep);
done:
    g_done = 1;
    fflush(out);
    free(a);
    return NULL;
}

void hv_tss_probe_start(HvVirtioUsb *usb, uint64_t ecid) {
    struct { HvVirtioUsb *u; uint64_t ecid; } *a = malloc(sizeof *a);
    if (!a) return;
    a->u = usb, a->ecid = ecid;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, a) != 0) { free(a); return; }
    pthread_detach(t);
}
