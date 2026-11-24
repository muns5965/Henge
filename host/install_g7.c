#include "install_g7.h"

#include "net_guard.h"

#include <stdio.h>

static const HvG7NetHost k_hosts[] = {{"gs.apple.com", 443}, {"gs.apple.com.akadns.net", 443}, {"gs.apple.com.v.aaplimg.com", 443},
    /* URLAsset host the device asked for (run g7_run_1795484190481, purpose=URL, refused at mount_filesystem); approved by the user on 2026-11-24, port 443 only. See docs/g7-new-destinations.md. */
    {"wkms-public.apple.com", 443}};
static int g_ok;

const HvG7NetHost *hv_g7_net_hosts(size_t *n) { if (n) *n = sizeof k_hosts / sizeof *k_hosts; return k_hosts; }

int hv_g7_extra_flags(void) {
    int ok = 1;
    for (size_t i = 0; i < sizeof k_hosts / sizeof *k_hosts; i++)
        if (hv_net_guard_allow_host(k_hosts[i].name, k_hosts[i].port) != 0) ok = 0;
    g_ok = ok;
    return 0;
}

int hv_g7_net_registered_ok(void) { return g_ok; }

int hv_g7_net_gate(void) {
    if (g_ok) return 0;
    fprintf(stderr, "install: the network allow-list is incomplete (the list is full or a host was invalid); the restore is not started\n");
    return -1;
}
