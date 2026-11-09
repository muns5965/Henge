/* Host services the virtual Mac can run without, for hosts that do not have them yet (Linux, Android):
   the user-mode network (libslirp), audio, and the share socket. Each does nothing and says so, which the
   machine's setup already copes with (hv_net_attach failing leaves the guest without a network; the callers
   check). Replace a stub with the real thing when it exists. Apple builds do not compile this file. */
#include <stdint.h>
#include <stdio.h>

#include "audio_mac.h"
#include "net_slirp.h"

uint64_t hv_net_tx_frames, hv_net_rx_frames;

void hv_net_set_share_socket(const char *path) { (void)path; }
int hv_net_attach(HvMachine *m, HvVirtioNet *n) {
    (void)m;
    (void)n;
    return -1; /* no user-mode network here */
}
void hv_net_poll(void) {}
void hv_audio_attach(HvVirtioSnd *d) { (void)d; }
