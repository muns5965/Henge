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
    int dfu_func_seen;       /* a DFU functional descriptor (0x21, length 7 or 9) followed it */
    int dfu_func_length;     /* 7 (DFU 1.0 form) or 9 */
    int dfu_attributes;      /* bmAttributes: bit0 canDnload, bit1 canUpload, bit2 manifestationTolerant, bit3 willDetach */
    int dfu_detach_timeout;
    int dfu_transfer_size;
    int dfu_bcd_version;     /* -1 for the 7-byte form, which has none */
    int dfu_istring;         /* iInterface of the DFU interface */
} HvUsbConfigInfo;

/* Describes `blob` (len bytes) on `out` (may be NULL) and fills `info`. Returns 0 if it is a well-formed configuration, -1 otherwise. */
int hv_usb_describe_config(const uint8_t *blob, int len, HvUsbConfigInfo *info, FILE *out);

/* Prints `len` bytes as hex, 16 per line, under `label`. */
void hv_usb_hexdump(FILE *out, const char *label, const uint8_t *b, int len);

/* Name of a DFU state (bState of GET_STATE / GET_STATUS), or "?" */
const char *hv_dfu_state_name(int state);
const char *hv_dfu_status_name(int status);

/* What the probe does with the DFU class requests. GET_STATUS can move some DFU states on (dfuDNLOAD-SYNC, dfuMANIFEST-SYNC, ...), so it is sent
   only when GET_STATE answered with exactly one byte and that byte is dfuIDLE; in every other case the state is only reported.
   state_rc is what the control transfer returned (bytes received, or a negative error). Returns 1 to send GET_STATUS, 0 to skip; *why says which. */
int hv_dfu_should_get_status(int state_rc, int state, const char **why);

/* Reads a GET_STATUS answer: requires exactly 6 bytes. Returns 0 and fills the fields, or -1 (not 6 bytes, or a negative transfer result). */
typedef struct { int status, poll_timeout_ms, state, istring; } HvDfuStatus;
int hv_dfu_parse_status(int rc, const uint8_t b[6], HvDfuStatus *out);

#endif
