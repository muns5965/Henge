#ifndef HV_USB_DIAG_GATE_H
#define HV_USB_DIAG_GATE_H
/* Rate limit for the virtual USB device's per-transaction diagnostic lines ("bdif usb diag:"). The device printed about a dozen lines for every 2 KiB chunk on the CPU threads
   (160,000+ lines in one restore); each fprintf goes through a pipe the app reads line by line. The first HV_USB_DIAG_KEEP events are printed in full, after that only every
   HV_USB_DIAG_EVERY-th, each with one line saying how many events there were. One "event" is one transaction (all the lines it would print are printed or none are).
   Lines that are rare by themselves (queue enable, REG_BUSY "no change" every 20000 reads, "no host request yet" every 20000 checks) and every error line are not gated. */
#include <stdint.h>
#include <stdio.h>

#define HV_USB_DIAG_KEEP 100u
#define HV_USB_DIAG_EVERY 2000u

/* 1: print this event's lines. `events` is the counter of the one device; it is atomic, so the CPU threads may call this at the same time. */
static inline int hv_usb_diag_pass(uint64_t *events) {
    uint64_t n = __atomic_add_fetch(events, 1, __ATOMIC_RELAXED);
    if (n <= HV_USB_DIAG_KEEP) return 1;
    if (n % HV_USB_DIAG_EVERY) return 0;
    fprintf(stderr, "bdif usb diag: %llu events so far; after the first %u only every %uth is printed\n", (unsigned long long)n, HV_USB_DIAG_KEEP, HV_USB_DIAG_EVERY);
    return 1;
}
#endif
