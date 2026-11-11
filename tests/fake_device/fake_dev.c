/* Fake usbmuxd + fake restored (S1). See fake_dev.h. Protocol facts: libusbmuxd packet = {length, version, message(8 = plist), tag} native-endian + XML plist;
   restored = 4-byte big-endian length + plist. Written from the vendored sources (libusbmuxd.c, libimobiledevice/src/restore.c); nothing here talks to a real device. */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "fake_dev.h"

static const FakeScenario *g_scn;
static FakeStats g_st;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
#define BUMP(f) do { pthread_mutex_lock(&g_mu); g_st.f++; pthread_mutex_unlock(&g_mu); } while (0)

void fake_stats(FakeStats *s) { pthread_mutex_lock(&g_mu); *s = g_st; pthread_mutex_unlock(&g_mu); }

static int rd(int fd, void *b, size_t n) {
    size_t g = 0;
    while (g < n) { ssize_t r = recv(fd, (char *)b + g, n - g, 0); if (r <= 0) return -1; g += (size_t)r; }
    return 0;
}
static int wr(int fd, const void *b, size_t n) {
    size_t g = 0;
    while (g < n) { ssize_t r = send(fd, (const char *)b + g, n - g, MSG_NOSIGNAL); if (r <= 0) return -1; g += (size_t)r; }
    return 0;
}

/* ---- restored framing ---- */
static int rs_send(int fd, plist_t p) {
    char *bin = NULL; uint32_t len = 0;
    plist_to_bin(p, &bin, &len);
    uint32_t be = htonl(len);
    int r = wr(fd, &be, 4) == 0 && wr(fd, bin, len) == 0 ? 0 : -1;
    free(bin);
    return r;
}
static plist_t rs_recv(int fd) {
    uint32_t be;
    if (rd(fd, &be, 4)) return NULL;
    uint32_t len = ntohl(be);
    if (len == 0 || len > (1u << 24)) return NULL;
    char *b = malloc(len);
    if (!b || rd(fd, b, len)) { free(b); return NULL; }
    plist_t p = NULL;
    plist_from_memory(b, len, &p, NULL);
    free(b);
    return p;
}
static char *dict_str(plist_t d, const char *k) {
    plist_t n = plist_dict_get_item(d, k);
    char *s = NULL;
    if (n && plist_get_node_type(n) == PLIST_STRING) plist_get_string_val(n, &s);
    return s;
}

static void *restored_conn(void *arg) {
    int fd = (int)(intptr_t)arg;
    int started = 0;
    for (;;) {
        plist_t req = rs_recv(fd);
        if (!req) break;
        char *r = dict_str(req, "Request");
        plist_free(req);
        if (!r) continue;   /* e.g. ReceivedFinalStatusMsg (MsgType): no reply */
        if (!strcmp(r, "QueryType")) {
            plist_t d = plist_new_dict();
            plist_dict_set_item(d, "Type", plist_new_string("com.apple.mobile.restored"));
            plist_dict_set_item(d, "RestoreProtocolVersion", plist_new_uint(15));
            rs_send(fd, d); plist_free(d);
        } else if (!strcmp(r, "QueryValue")) {
            plist_t d = plist_new_dict();
            plist_t hw = plist_new_dict();
            plist_dict_set_item(hw, "UniqueChipID", plist_new_uint(FAKE_ECID));
            plist_dict_set_item(hw, "ChipID", plist_new_uint(0x8103));
            plist_dict_set_item(hw, "BoardID", plist_new_uint(0x22));
            plist_dict_set_item(hw, "ProductionMode", plist_new_bool(0));
            plist_dict_set_item(d, "HardwareInfo", hw);   /* SavedDebugInfo is deliberately absent */
            rs_send(fd, d); plist_free(d);
        } else if (!strcmp(r, "Goodbye")) {
            BUMP(goodbye);
            plist_t d = plist_new_dict();
            plist_dict_set_item(d, "Result", plist_new_string("Success"));
            rs_send(fd, d); plist_free(d);
            if (started && g_scn && g_scn->late_msg) {   /* T1b: the loop has ended and the host is closing; the device now sends an async data request */
                for (int i = 0; i < 2; i++) {
                    usleep(300000);
                    errno = 0;
                    int rc = rs_send(fd, g_scn->late_msg), e = errno;
                    pthread_mutex_lock(&g_mu);
                    if (i == 0) { g_st.late_rc1 = rc; g_st.late_errno1 = e; } else { g_st.late_rc2 = rc; g_st.late_errno2 = e; }
                    g_st.late_attempts++;
                    pthread_mutex_unlock(&g_mu);
                }
            }
        } else if (!strcmp(r, "StartRestore")) {
            started = 1;
            BUMP(start_restore);
            if (g_scn && g_scn->msg_delay_ms) usleep((useconds_t)g_scn->msg_delay_ms * 1000);
            for (int i = 0; g_scn && i < g_scn->nmsgs; i++) rs_send(fd, g_scn->msgs[i]);
            if (g_scn && g_scn->end == FAKE_END_CLOSE) { free(r); break; }
        }
        free(r);
    }
    close(fd);
    return NULL;
}


/* drain a tunnel until the peer closes it; counts bytes */
static void drain(int fd, int *counter) {
    char b[4096];
    for (;;) { ssize_t r = recv(fd, b, sizeof b, 0); if (r <= 0) break; if (counter) { pthread_mutex_lock(&g_mu); *counter += (int)r; pthread_mutex_unlock(&g_mu); } }
    close(fd);
}
/* reverse-proxy framing (libimobiledevice reverse_proxy.c): plist = 4-byte LITTLE-endian length + binary plist */
static int px_send_plist(int fd, plist_t p) {
    char *bin = NULL; uint32_t len = 0;
    plist_to_bin(p, &bin, &len);
    uint32_t le = htole32(len);
    int r = wr(fd, &le, 4) == 0 && wr(fd, bin, len) == 0 ? 0 : -1;
    free(bin);
    return r;
}
static plist_t px_recv_plist(int fd) {
    uint32_t le;
    if (rd(fd, &le, 4)) return NULL;
    uint32_t len = le32toh(le);
    if (len == 0 || len > (1u << 20)) return NULL;
    char *b = malloc(len);
    if (!b || rd(fd, b, len)) { free(b); return NULL; }
    plist_t p = NULL;
    plist_from_memory(b, len, &p, NULL);
    free(b);
    return p;
}
/* control channel (port 1082, control protocol 2): host sends "BeginCtrl\0" and a plist, the device answers with ConnPort, then sends commands (uint32 LE): 1 = connection request */
static void proxy_ctrl(int fd) {
    BUMP(px_ctrl);
    char hello[10];
    if (rd(fd, hello, sizeof hello) || memcmp(hello, "BeginCtrl", 9)) { close(fd); return; }
    plist_t req = px_recv_plist(fd);
    plist_free(req);
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "ConnPort", plist_new_uint(FAKE_PROXY_CONN_PORT));
    px_send_plist(fd, d); plist_free(d);
    usleep(100000);
    int n = g_scn ? g_scn->proxy_connect_reqs : 0;
    if (n > 0) {
        uint32_t cmds[8];
        if (n > 8) n = 8;
        for (int i = 0; i < n; i++) cmds[i] = htole32(1);
        wr(fd, cmds, 4 * (size_t)n);   /* one write: the requests arrive with no gap (T13) */
    }
    drain(fd, NULL);
}
/* connection channel (port 1083): "HelloConn\0" in, plist {Command:HelloConn} out; optionally a proxy command (0x105: 0, 3, hostlen, host, port BE) that the host acks and echoes */
static void proxy_conn(int fd) {
    BUMP(px_conn);
    char hello[10];
    if (rd(fd, hello, sizeof hello) || memcmp(hello, "HelloConn", 9)) { close(fd); return; }
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "Command", plist_new_string("HelloConn"));
    plist_dict_set_item(d, "Identifier", plist_new_string("fake-proxy-device"));
    px_send_plist(fd, d); plist_free(d);
    if (g_scn && g_scn->proxy_do_cmd) {
        usleep(50000);
        uint16_t cmd = htole16(0x105);
        wr(fd, &cmd, 2);
        usleep(50000);
        const char *host = "127.0.0.1";
        unsigned char pl[64];
        size_t hl = strlen(host), n = 0;
        pl[n++] = 0; pl[n++] = 3; pl[n++] = (unsigned char)hl;
        memcpy(pl + n, host, hl); n += hl;
        pl[n++] = (unsigned char)(g_scn->proxy_target_port >> 8); pl[n++] = (unsigned char)(g_scn->proxy_target_port & 0xff);
        wr(fd, pl, n);
        uint16_t ack;
        if (rd(fd, &ack, 2) == 0) BUMP(px_cmd_acked);
        if (g_scn->proxy_close_after_ms > 0) { usleep((useconds_t)g_scn->proxy_close_after_ms * 1000); close(fd); return; }
    }
    drain(fd, NULL);
}

/* ---- usbmuxd framing ---- */
static void mux_reply(int fd, uint32_t version, uint32_t tag, plist_t p) {
    char *xml = NULL; uint32_t len = 0;
    plist_to_xml(p, &xml, &len);
    uint32_t h[4] = {16 + len, version, 8, tag};
    wr(fd, h, 16); wr(fd, xml, len);
    free(xml);
}
static void mux_result(int fd, uint32_t version, uint32_t tag, uint64_t n) {
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "MessageType", plist_new_string("Result"));
    plist_dict_set_item(d, "Number", plist_new_uint(n));
    mux_reply(fd, version, tag, d);
    plist_free(d);
}

static void *mux_conn(void *arg) {
    int fd = (int)(intptr_t)arg;
    for (;;) {
        uint32_t h[4];
        if (rd(fd, h, 16) || h[0] < 16 || h[0] > (1u << 20)) break;
        uint32_t plen = h[0] - 16;
        char *body = malloc(plen + 1);
        if (!body || rd(fd, body, plen)) { free(body); break; }
        plist_t req = NULL;
        if (h[2] == 8) plist_from_memory(body, plen, &req, NULL);
        free(body);
        char *mt = req ? dict_str(req, "MessageType") : NULL;
        if (!mt) { mux_result(fd, h[1], h[3], 1); plist_free(req); continue; }
        if (!strcmp(mt, "ListDevices")) {
            BUMP(list_devices);
            plist_t d = plist_new_dict(), arr = plist_new_array(), e = plist_new_dict(), pr = plist_new_dict();
            plist_dict_set_item(pr, "DeviceID", plist_new_uint(FAKE_DEVICE_ID));
            plist_dict_set_item(pr, "ProductID", plist_new_uint(0x1281));
            plist_dict_set_item(pr, "SerialNumber", plist_new_string(FAKE_UDID));
            plist_dict_set_item(pr, "ConnectionType", plist_new_string("USB"));
            plist_dict_set_item(e, "DeviceID", plist_new_uint(FAKE_DEVICE_ID));
            plist_dict_set_item(e, "MessageType", plist_new_string("Attached"));
            plist_dict_set_item(e, "Properties", pr);
            plist_array_append_item(arr, e);
            plist_dict_set_item(d, "DeviceList", arr);
            mux_reply(fd, h[1], h[3], d);
            plist_free(d);
        } else if (!strcmp(mt, "Connect")) {
            uint64_t pn = 0;
            plist_t n = plist_dict_get_item(req, "PortNumber");
            if (n) plist_get_uint_val(n, &pn);
            uint16_t port = ntohs((uint16_t)pn);
            if (port == 62078) {
                BUMP(connect_restored);
                mux_result(fd, h[1], h[3], 0);
                free(mt); plist_free(req);
                return restored_conn((void *)(intptr_t)fd);   /* the tunnel: raw restored traffic from here on */
            }
            if (g_scn && g_scn->proxy && port == FAKE_PROXY_CTRL_PORT) { mux_result(fd, h[1], h[3], 0); free(mt); plist_free(req); proxy_ctrl(fd); return NULL; }
            if (g_scn && g_scn->proxy && port == FAKE_PROXY_CONN_PORT) { mux_result(fd, h[1], h[3], 0); free(mt); plist_free(req); proxy_conn(fd); return NULL; }
            if (port == FAKE_DATA_PORT && g_scn) {
                BUMP(dp_connect);
                int m = g_scn->data_port_mode;
                if (m == FAKE_DP_DELAY_REFUSE || m == FAKE_DP_DELAY_OPEN) usleep((useconds_t)g_scn->data_port_delay_ms * 1000);
                if (m == FAKE_DP_OPEN_SILENT || m == FAKE_DP_DELAY_OPEN) { mux_result(fd, h[1], h[3], 0); free(mt); plist_free(req); drain(fd, &g_st.dp_bytes); return NULL; }
                if (m == FAKE_DP_NOREPLY) { free(mt); plist_free(req); drain(fd, &g_st.dp_bytes); return NULL; }   /* never answers the Connect */
                mux_result(fd, h[1], h[3], 3);
                free(mt); plist_free(req);
                continue;
            }
            pthread_mutex_lock(&g_mu); g_st.connect_other++; g_st.last_other_port = port; pthread_mutex_unlock(&g_mu);
            mux_result(fd, h[1], h[3], 3);   /* RESULT_CONNREFUSED */
        } else {
            mux_result(fd, h[1], h[3], 1);
        }
        free(mt); plist_free(req);
    }
    close(fd);
    return NULL;
}

static int g_ls = -1;
static void *mux_accept(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(g_ls, NULL, NULL);
        if (c < 0) break;
        BUMP(accepted);
        pthread_t t;
        if (pthread_create(&t, NULL, mux_conn, (void *)(intptr_t)c) == 0) pthread_detach(t); else close(c);
    }
    return NULL;
}

int fake_mux_start(const FakeScenario *scn) {
    g_scn = scn;
    g_ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback only, port chosen by the kernel */
    if (bind(g_ls, (struct sockaddr *)&a, sizeof a) || listen(g_ls, 8)) return -1;
    socklen_t l = sizeof a;
    getsockname(g_ls, (struct sockaddr *)&a, &l);
    pthread_t t;
    if (pthread_create(&t, NULL, mux_accept, NULL)) return -1;
    pthread_detach(t);
    return ntohs(a.sin_port);
}
