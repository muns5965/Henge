#ifndef HENGEVM_USB_DESC_H
#define HENGEVM_USB_DESC_H

#include <stdint.h>
#include <stdio.h>

/* Reads a USB configuration descriptor blob (the configuration, its interfaces, endpoints and class descriptors) and says what is in it.
   Pure parsing, no device: used by the USB probe in usb_host.c and tested on synthetic data in tests/test_usbdesc.c. */
typedef struct {
    int valid;               /* the blob parsed without running off its end */
    int total_length;        /* wTotalLength from the header */
    int num_interfaces;      /* bNumInterfaces from the header */
    int configuration_value;
    int interfaces;          /* interface descriptors seen (alternate settings count) */
    int endpoints;
    int dfu_interface;       /* bInterfaceNumber of the first DFU interface (class 0xFE, subclass 1), or -1 */
    int dfu_protocol;        /* 1 = runtime, 2 = DFU mode, as in that interface; -1 if none */
    int dfu_alt;
    int dfu_func_seen;       /* a DFU functional descriptor (0x21, length 9) followed it */
    int dfu_attributes;      /* bmAttributes: bit0 canDnload, bit1 canUpload, bit2 manifestationTolerant, bit3 willDetach */
    int dfu_detach_timeout;
    int dfu_transfer_size;
    int dfu_bcd_version;
    int dfu_istring;         /* iInterface of the DFU interface */
} HvUsbConfigInfo;

/* Describes `blob` (len bytes) on `out` (may be NULL) and fills `info`. Returns 0 if it is a well-formed configuration, -1 otherwise. */
int hv_usb_describe_config(const uint8_t *blob, int len, HvUsbConfigInfo *info, FILE *out);

/* Name of a DFU state (bState of GET_STATE / GET_STATUS), or "?" */
const char *hv_dfu_state_name(int state);
const char *hv_dfu_status_name(int status);

#endif
