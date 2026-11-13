#ifndef HENGEVM_VIRTIO_USB_H
#define HENGEVM_VIRTIO_USB_H

#include <pthread.h>
#include <stdint.h>

#include "virtio.h"

/* Apple's virtio USB device-mode controller (PCI 0x106b:0x1a01), the port a
   virtual Mac uses to show up as a USB device (DFU, recovery, restore) to the
   machine that installs it. The guest is the USB device; the host side here
   plays the USB host.

   Two queues. Queue 0 (requests, host -> guest): the guest posts writable
   buffers and the host fills one per request with a 6-byte header
   {u32 len, u8 endpoint, u8 type} and `len` bytes of payload:
     type 1 on endpoint 0: a setup packet (8 bytes), then the OUT data stage;
     type 1 on another OUT endpoint: the data; on an IN endpoint (bit 7): no
       payload, len is how much the host can take;
     type 2: bus reset; type 3: pause the endpoint.
   Queue 1 (responses, guest -> host): readable buffers with a 2-byte header
   {u8 type, u8 endpoint} and data: type 1 data (IN data, or empty for a
   finished OUT transfer or control status), 2 stall, 3 abort, 4 reset done,
   5 paused. The guest connects (goes on the bus) when it sets DRIVER_OK and
   disconnects by resetting the device. */

#define HV_VUSB_REQ_DATA 1
#define HV_VUSB_REQ_RESET 2
#define HV_VUSB_REQ_PAUSE 3

#define HV_VUSB_RSP_DATA 1
#define HV_VUSB_RSP_STALL 2
#define HV_VUSB_RSP_ABORT 3
#define HV_VUSB_RSP_RESET 4
#define HV_VUSB_RSP_PAUSED 5

#define HV_VUSB_RING 256

typedef struct HvVusbMsg {
    uint8_t type, ep;
    uint32_t len;
    uint8_t *data; /* malloc'd, len bytes; NULL for an IN request (len = how much the host takes) */
} HvVusbMsg;

typedef struct HvVirtioUsb {
    HvVirtio vio;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    HvVusbMsg out[HV_VUSB_RING];   /* requests waiting for a guest buffer */
    uint32_t out_head, out_tail;
    HvVusbMsg in[HV_VUSB_RING];    /* responses waiting for the host */
    uint32_t in_head, in_tail;
    uint32_t session;              /* bumps on every connect and disconnect */
    int online;                    /* a front-end is connected */
    int pci_on;                    /* the PCI front-end saw DRIVER_OK */
    int rom_on;                    /* the ROM's front-end (bdif) owns the port; off once the kernel's driver takes it */
    int log;
    /* The control endpoint's state (host/usb_host.c; all zero = the legacy behaviour, nothing tracked): see hv_usb_set_strict. Guarded by usb_host.c's own mutex, never by mu. */
    int cstrict, cstate;
    uint32_t cgen;
} HvVirtioUsb;

HvVirtioUsb *hv_virtio_usb_create(HvMachine *m, HvPcie *bus);
/* CPU thread (board tick): move queued requests into guest buffers. */
void hv_virtio_usb_poll(HvVirtioUsb *d);

/* Host side, any thread. */
int hv_vusb_online(HvVirtioUsb *d, uint32_t *session);
/* Queue a request; data may be NULL. Returns 0, or -1 if the ring is full. */
int hv_vusb_send(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len);
/* The same, and the session the request was queued in, read in the same locked step: a wait that is given this session ends with -1 if the connection changes at any
   point after the request was queued (a session read before queueing could be a different one). Returns 0, or -1 (nothing was queued). */
int hv_vusb_send_session(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len, uint32_t *session);
/* An IN transfer request: header only, len = the most the host takes. */
int hv_vusb_send_in(HvVirtioUsb *d, uint8_t ep, uint32_t len);
/* Wait up to timeout_ms for the next response on `ep` (or any endpoint if
   ep == 0xff; reset responses always match). 1: got one (free m->data),
   0: timed out, -1: the guest disconnected. */
int hv_vusb_recv(HvVirtioUsb *d, uint8_t ep, HvVusbMsg *m, int timeout_ms);
/* A wait for the strict control path. `session` is the one hv_vusb_send_session returned. With reset_only the next RESET response is taken (and nothing else);
   otherwise the next response on `ep` that is not a RESET. The ring is looked at before the cancel flag, so a response that is already there wins over a cancel.
   The wait is made of slices of at most 50 ms and its end is a CLOCK_MONOTONIC deadline: the cancel flag is looked at every slice, which is not a promise to return
   within 50 ms. 1: got one (free m->data); 0: timed out; -1: offline or the session is not `session`; -2: cancelled. */
int hv_vusb_recv_ex(HvVirtioUsb *d, uint8_t ep, int reset_only, HvVusbMsg *m, int timeout_ms, uint32_t session, volatile int *cancel);
/* Drop queued requests and responses (after a reset or reconnect). */
void hv_vusb_flush(HvVirtioUsb *d);

/* For the boot ROM's front-end (bdif channel 0x100000, the same messages
   over Apple's MMIO transport): connect or disconnect, take the next request
   (1 if there was one; free m->data), hand over a response. */
void hv_vusb_set_online(HvVirtioUsb *d, int on);
int hv_vusb_take_request(HvVirtioUsb *d, HvVusbMsg *m);
void hv_vusb_put_response(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len);

#endif
