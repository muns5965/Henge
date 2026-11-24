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
/* device references that submitted transfers took and gave back; returns the difference (0 when every transfer has completed). For tests. */
int hv_libusb_xfer_dev_refs(int *acquired, int *released);
/* How long a bulk transfer waits for room in the guest's request ring, and for the answers an earlier OUT transfer left, when it has no time limit of its own (default 60000 ms). For tests. */
void hv_libusb_set_queue_wait_ms(int ms);
int hv_libusb_open_handles(void);          /* device handles opened and not yet closed */
int hv_libusb_handles_opened_total(void);  /* handles opened so far */
int hv_libusb_hotplug_live(void);          /* hotplug callbacks registered and not yet deregistered (the restore tool's device-event subscription) */
int hv_libusb_ctrl_get(int k, HvCtrlRecord *out); /* 1 if entry k is still held */
/* A cumulative count of every control request made so far, by (bmRequestType's type bits 0-3) and bRequest (0-6, or 7 for "7 or more"). Unlike the ring
   above, this is never evicted: a caller that wants a total for a request shape across a run longer than HV_CTRL_LOG_MAX reads this instead of the ring. It
   says nothing about a request's outcome, direction, interface or value/index (only the ring gives those, and only for requests still in it). */
unsigned hv_libusb_ctrl_hist(int type, int breq);
/* Outcome of every non-control (bulk/interrupt/isochronous) transfer once it completes: the first two never shrink the other's count (a transfer completes
   exactly one way), and the byte count only grows for an OK completion. hv_libusb_data_transfer_count() (above) is submissions, which can be ahead of these
   two by the number still in flight. */
int hv_libusb_data_transfer_ok_count(void);
int hv_libusb_data_transfer_bad_count(void);   /* completed with a status other than LIBUSB_TRANSFER_COMPLETED (error, cancel, timeout, stall, no device) */
uint64_t hv_libusb_data_transfer_bytes(void);  /* total actual_length of every OK completion */

/* Every non-control (bulk/interrupt/isochronous) transfer that completed with a status other than LIBUSB_TRANSFER_COMPLETED, oldest first (the last
   HV_DATA_BAD_LOG_MAX are kept; same ring-with-never-evicted-histogram shape as the control-request ring above). Diagnostic only: nothing here changes a
   transfer's outcome, retries it or affects the virtual USB device. `status` is a libusb_transfer_status value (never LIBUSB_TRANSFER_COMPLETED=0 here). */
#define HV_DATA_BAD_LOG_MAX 16
typedef struct {
    int seq;
    uint8_t ep;             /* raw endpoint address; bit 0x80 set means IN */
    uint8_t type;            /* libusb_transfer_type: BULK=2, INTERRUPT=3, ISOCHRONOUS=1 */
    uint8_t status;          /* libusb_transfer_status at completion */
    int requested, actual;   /* transfer length requested, and actual_length reported with the failing completion */
} HvDataBadRecord;
int hv_libusb_data_bad_count(void);                 /* total bad non-control completions recorded so far (same total hv_libusb_data_transfer_bad_count() gives, tracked alongside the ring) */
int hv_libusb_data_bad_get(int k, HvDataBadRecord *out); /* 1 if entry k is still held */
unsigned hv_libusb_data_bad_hist(int status);        /* never evicted: a count by libusb_transfer_status, for a run longer than HV_DATA_BAD_LOG_MAX */

/* Debugging (HV_VUSB_PROBE=1): a thread that reads the device's descriptors. */
void hv_usb_probe_start(HvVirtioUsb *d);
/* Before hv_usb_probe_start: also ask five recovery environment variables (read-only; see host/usb_host.c). */
void hv_usb_probe_enable_getenv(int on);
/* Mode E only (one approved clone): the fixed getenv/setenv auto-boot true/getenv/saveenv sequence in host/usb_host.c. Needs hv_usb_probe_enable_getenv(1) too. */
void hv_usb_probe_enable_autoboot_fix(int on);

#endif
