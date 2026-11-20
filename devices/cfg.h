#ifndef HENGEVM_CFG_H
#define HENGEVM_CFG_H

#include <stdint.h>

#include "machine.h"

/* The vmapple configuration region: 64 KB of RAM the boot ROM reads, laid
   out as QEMU's hw/vmapple/cfg.c describes it. */
typedef struct HvCfgParams {
    uint32_t nr_cpus;
    uint64_t ecid;
    uint64_t ram_size;
    uint32_t run_installer1, run_installer2;
    uint32_t rnd;
    uint8_t mac_en0[6], mac_en1[6], mac_wifi0[6], mac_bt0[6];
    const char *serial, *model, *soc_name; /* NULL: QEMU's defaults */
    uint64_t seed;  /* nonzero: `rnd` gives a reproducible sequence (for debugging) */
    /* Host-only test knob, added 2026-11-20 for the Android-port boot investigation: read by
       devices/vmapple.c's existing USB-device-port creation check alongside run_installer1/2 and
       HV_USB_ON_NORMAL, to attach the same virtual USB device (hv_virtio_usb_create -- the guest-
       facing emulated port already used for installer boots, not host USB) on a NON-installer boot
       too. Deliberately NOT part of the guest-visible config region: hv_cfg_create() below copies
       each HvCfgParams field into VMAppleCfg by name and does not reference this one, so the ROM
       never sees it and run_installer1/run_installer2 (what the ROM's cfg region actually reports)
       are unaffected. */
    int usb_on_normal_boot;
} HvCfgParams;

/* Returns the host copy of the region (64 KB). */
uint8_t *hv_cfg_create(HvMachine *m, uint64_t pa, const HvCfgParams *p);

#endif
