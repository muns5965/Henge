#ifndef HENGEVM_TSS_PROBE_H
#define HENGEVM_TSS_PROBE_H
#include <stdint.h>
#include "usb_host.h"
/* vmrun --tss-probe: waits for the guest's ROM to show up on the virtual USB bus in DFU mode, reads the ECID and the AP/SEP nonces the ROM reports (through
   libirecovery, as the restore tools do), reads the build identity for this VM from the macOS 15.6.1 IPSW with Range requests, builds the main TSS request
   and sends it to Apple's TSS server exactly once (host/tss_probe_core.c). Sends no firmware, downloads no IPSW. On a detached thread. */
void hv_tss_probe_start(HvVirtioUsb *usb, uint64_t ecid);
int hv_tss_probe_finished(void);

/* Same, but after the AP ticket is issued it continues on, unretried, to the LocalPolicy request and then the RecoveryOS-root-ticket request, stopping
   at the first one that is not accepted (host/tss_probe_core.c's hv_tss_probe_core_chain). Shares hv_tss_probe_finished() with the function above: the
   two are never both run in the same process (each is its own vmrun build target), so sharing is safe. */
void hv_tss_probe_lpol_start(HvVirtioUsb *usb, uint64_t ecid);
#endif
