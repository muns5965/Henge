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
#define FAKE_DATA_PORT 4444      /* the DataPort a fake DataRequestMsg names (S2) */
#define FAKE_PROXY_CTRL_PORT 1082
#define FAKE_PROXY_CONN_PORT 1083
/* what the fake mux does when the host connects to FAKE_DATA_PORT */
enum { FAKE_DP_REFUSE = 0, FAKE_DP_OPEN_SILENT = 1, FAKE_DP_NOREPLY = 2, FAKE_DP_DELAY_REFUSE = 3, FAKE_DP_DELAY_OPEN = 4 };
typedef struct {
    plist_t *msgs;       /* messages to send, in order, right after StartRestore (ownership stays with the caller) */
    int nmsgs;
    int end;
    int msg_delay_ms;    /* wait this long after StartRestore before the first message (S3: let the proxy connection come up first) */
    int data_port_mode;  /* FAKE_DP_*, for FAKE_DATA_PORT (S2) */
    int data_port_delay_ms;
    int proxy;           /* S3: 1 = answer the reverse-proxy control port (1082) like a device, 0 = refuse it (S1 behaviour) */
    int proxy_connect_reqs;   /* number of "connection request" commands (1) sent back to back after the control handshake */
    int proxy_do_cmd;    /* the connection channel then asks the host to open proxy_target_port on 127.0.0.1 (command 0x105) */
    uint16_t proxy_target_port;
    plist_t late_msg;    /* T1b: sent on the restore connection after the host has said Goodbye (the host's loop has ended and it is closing the connection) */
    int proxy_close_after_ms;   /* >0: the connection channel is closed by the device this long after the proxy command was acked (T12 variant) */
} FakeScenario;

/* Starts the fake usbmuxd. Returns the loopback port (>0) or -1. The scenario must outlive the test. */
int fake_mux_start(const FakeScenario *scn);
/* counters (read after the case): connections accepted by the mux, ListDevices, Connect to restored, Connect refused (other ports), StartRestore received */
typedef struct { int accepted, list_devices, connect_restored, connect_other, last_other_port, start_restore, goodbye, final_ack, dp_connect, dp_bytes, px_ctrl, px_conn, px_cmd_acked, late_attempts, late_rc1, late_errno1, late_rc2, late_errno2; } FakeStats;
void fake_stats(FakeStats *s);
#endif
