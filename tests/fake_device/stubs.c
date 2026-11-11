/* S1 test harness: the guest-memory side of virtio_usb.c, never reached here (same stand-ins as tests/test_restore_stack.c). */
#include <stdint.h>
#include "devices/pcie.h"
#include "devices/virtio_usb.h"

/* virtio_usb.c's links to the guest-memory side of the machine: never reached here */
int hv_virtq_pop(HvVirtio *v, int q, HvVirtqElem *e) { (void)v, (void)q, (void)e; return 0; }
void hv_virtq_push(HvVirtio *v, int q, const HvVirtqElem *e, uint32_t w) { (void)v, (void)q, (void)e, (void)w; }
void hv_virtio_interrupt(HvVirtio *v) { (void)v; }
void hv_virtio_init(HvVirtio *v, HvMachine *m, int id, uint32_t cc, int nq, uint64_t f) { (void)v, (void)m, (void)id, (void)cc, (void)nq, (void)f; }
uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t o, void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t o, const void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_writable(const HvVirtqElem *e) { (void)e; return 0; }
int hv_pcie_attach(HvPcie *p, HvPciDev *d) { (void)p, (void)d; return -1; }
void hv_machine_wake(HvMachine *m) { (void)m; }

