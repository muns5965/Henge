#ifndef HENGEVM_USB_HOST_H
#define HENGEVM_USB_HOST_H

#include <stdint.h>

#include "virtio_usb.h"

/* The USB host side of the virtual Mac's device port: synchronous transfers
   over the virtio-usb messages. Returns bytes transferred, or a negative
   HV_USB_E* code. */
#define HV_USB_ETIMEOUT -1
#define HV_USB_ESTALL -2
#define HV_USB_EGONE -3   /* the guest left the bus (rebooting into the next stage) */
#define HV_USB_EIO -4
#define HV_USB_ECANCELLED -5   /* strict mode: the caller's cancel flag ended the wait */
#define HV_USB_EUNCERTAIN -6   /* strict mode: an earlier request on the control endpoint has no settled answer; nothing was sent */
#define HV_USB_ESTATE -8       /* strict mode: the call does not apply in the control endpoint's present state; nothing changed */
#define HV_USB_EBUSY -7        /* strict mode: another control request (or a RESET) is in flight; nothing was sent */

/* STRICT control mode (off by default, and nothing in the product turns it on): the virtio-usb messages carry no request number, so after a request that was
   cancelled or timed out, its answer may still arrive and would be taken for the NEXT request's. In strict mode the control endpoint is CLEAR, PENDING (a request is
   in flight, or a RESET), or UNCERTAIN (a request was sent and its answer did not settle: timeout, cancel, the connection changed, an answer of an unexpected kind).
   A new control request or RESET is refused BEFORE anything is sent unless CLEAR. Nothing releases UNCERTAIN except hv_usb_ctrl_reacquire, which no code calls: a late
   answer, a new session and a RESET do not (nothing here shows that answers cannot cross a session). Requests are serialised by refusing, not by queueing. */
enum { HV_CTRL_CLEAR = 0, HV_CTRL_PENDING = 1, HV_CTRL_UNCERTAIN = 2, HV_CTRL_RESETTING = 3 };
/* Changes the mode. Applied only when no control request or RESET is inside hv_usb_control_c / hv_usb_reset (either mode) and the endpoint is CLEAR; otherwise refused with
   HV_USB_EBUSY (a call in flight) or HV_USB_EUNCERTAIN (an unsettled request), and nothing changes. Meant for set-up, before the first request. Returns 0 when applied. */
int hv_usb_set_strict(HvVirtioUsb *d, int on);
int hv_usb_ctrl_state(HvVirtioUsb *d);
/* The future recovery hook. It only REFUSES wrong calls; it does not show that a release is safe: whoever calls it must have made sure no answer to an earlier request can still
   come (nothing here can show that). Refused, with nothing changed: HV_USB_EBUSY while a control request or RESET is in flight or any caller is still inside a control call;
   HV_USB_ESTATE when the endpoint is not UNCERTAIN. Returns 0 when it released. */
int hv_usb_ctrl_reacquire(HvVirtioUsb *d);
/* hv_usb_control with a cancel flag (read atomically; honoured in strict mode only). */
int hv_usb_control_c(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                     void *data, uint16_t wLength, int timeout_ms, volatile int *cancel);

int hv_usb_reset(HvVirtioUsb *d, int timeout_ms);
int hv_usb_control(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                   void *data, uint16_t wLength, int timeout_ms);
int hv_usb_bulk(HvVirtioUsb *d, uint8_t ep, void *data, uint32_t len, int timeout_ms);
/* Wait until the guest is on the bus; returns its session number, or -1. */
int64_t hv_usb_wait_online(HvVirtioUsb *d, int timeout_ms);

/* host/libusb_shim.c: the device its libusb API drives. */
void hv_libusb_shim_attach(HvVirtioUsb *d);

/* The control requests the stand-in libusb has carried, oldest first (the last HV_CTRL_LOG_MAX are kept). `result` is the byte count or a negative libusb
   error; `done` is 0 while the request is still in flight. */
#define HV_CTRL_LOG_MAX 128
typedef struct {
    int seq, result, done;
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} HvCtrlRecord;
int hv_libusb_ctrl_count(void);
int hv_libusb_data_transfer_count(void); /* bulk/interrupt/isochronous transfers submitted through the API so far (firmware goes out this way; control requests are counted above) */
int hv_libusb_open_handles(void);          /* device handles opened and not yet closed */
int hv_libusb_handles_opened_total(void);  /* handles opened so far */
int hv_libusb_hotplug_live(void);          /* hotplug callbacks registered and not yet deregistered (the restore tool's device-event subscription) */
int hv_libusb_ctrl_get(int k, HvCtrlRecord *out); /* 1 if entry k is still held */

/* Debugging (HV_VUSB_PROBE=1): a thread that reads the device's descriptors. */
void hv_usb_probe_start(HvVirtioUsb *d);

#endif
