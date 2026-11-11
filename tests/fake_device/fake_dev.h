/* S1 test harness only (docs/fake-device-impl-scope.md). A fake usbmuxd (TCP, loopback, ephemeral port) and a fake restored behind it. TEST CODE: nothing here is
   linked into the app, and no product source is changed. Only clearly fake identifiers are used (FAKE_ECID / FAKE_UDID). */
#ifndef FAKE_DEV_H
#define FAKE_DEV_H
#include <stdint.h>
#include <plist/plist.h>

#define FAKE_ECID 0x1F4C0DEull
#define FAKE_UDID "FAKE-UDID-FOR-TEST"
#define FAKE_DEVICE_ID 7

/* what the fake restored does after it has received StartRestore */
enum { FAKE_END_HOLD = 0, FAKE_END_CLOSE = 1 };   /* HOLD: keep answering requests (Goodbye), send nothing more; CLOSE: close the connection */
typedef struct {
    plist_t *msgs;       /* messages to send, in order, right after StartRestore (ownership stays with the caller) */
    int nmsgs;
    int end;
} FakeScenario;

/* Starts the fake usbmuxd. Returns the loopback port (>0) or -1. The scenario must outlive the test. */
int fake_mux_start(const FakeScenario *scn);
/* counters (read after the case): connections accepted by the mux, ListDevices, Connect to restored, Connect refused (other ports), StartRestore received */
typedef struct { int accepted, list_devices, connect_restored, connect_other, last_other_port, start_restore, goodbye, final_ack; } FakeStats;
void fake_stats(FakeStats *s);
#endif
