/* I4/I5 (docs/i3-i6-contract-2026-11-12.md): restored_send serialization, idevice connection registration/references, fd reuse. Linux, fake usbmuxd, real host session registry.
   Usage: test_i45 <case>   cases: t4 reg wake refwait refleak reuse null */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <plist/plist.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/restore.h>
#include "restore.h"
#include "restore_session.h"
#include "restore_registry.h"
#include "fake_device/fake_dev.h"

static int nfail;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) nfail++; } while (0)
#define LDV(v) __atomic_load_n(&(v), __ATOMIC_SEQ_CST)
#define STV(v, x) __atomic_store_n(&(v), (x), __ATOMIC_SEQ_CST)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static int fd_open(int fd) { return fcntl(fd, F_GETFD) != -1; }

static int g_mp;
static int mux(int data_mode) {
    static FakeScenario scn; memset(&scn, 0, sizeof scn); scn.data_port_mode = data_mode;
    int mp = fake_mux_start(&scn); char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%d", mp); setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);
    g_mp = mp; return mp;
}
static int open_conn(idevice_t *dev, idevice_connection_t *c, int *fd) {
    if (idevice_new_with_options(dev, FAKE_UDID, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS) return -1;
    if (idevice_connect(*dev, FAKE_DATA_PORT, c) != IDEVICE_E_SUCCESS) return -1;
    return idevice_connection_get_fd(*c, fd) == IDEVICE_E_SUCCESS ? 0 : -1;
}

/* ---- T4 ---- */
static restored_client_t g_rc;
#define NSEND 300
static void *sender(void *a) {
    (void)a;
    char *pad = malloc(200000); memset(pad, 'x', 199999); pad[199999] = 0;
    for (int i = 0; i < NSEND; i++) {
        plist_t d = plist_new_dict(); plist_dict_set_item(d, "Request", plist_new_string("StartRestore")); plist_dict_set_item(d, "Pad", plist_new_string(pad));
        restored_send(g_rc, d); plist_free(d);
    }
    free(pad); return NULL;
}
/* ---- wake ---- */
static int w_state; static idevice_error_t w_ret;
static void *recv_thread(void *a) { char b[16]; uint32_t n = 0; STV(w_state, 1); w_ret = idevice_connection_receive((idevice_connection_t)a, b, sizeof b, &n); STV(w_state, 2); return NULL; }
static int d_done; static idevice_error_t d_ret;
static void *disc_thread(void *a) { d_ret = idevice_disconnect((idevice_connection_t)a); STV(d_done, 1); return NULL; }

int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
    hv_restore_session_set_close_timeout_ms(400);
    if (!strcmp(c, "t4")) {
        mux(FAKE_DP_REFUSE);
        idevice_t dev = NULL; restored_error_t e;
        if (idevice_new_with_options(&dev, FAKE_UDID, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS) { printf("FAIL setup\n"); return 2; }
        e = restored_client_new(dev, &g_rc, "t4");
        CHECK(e == RESTORE_E_SUCCESS, "restored client");
        FakeStats s0; fake_stats(&s0);
        pthread_t t[3]; for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, sender, NULL);
        for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
        usleep(500000);
        FakeStats s1; fake_stats(&s1);
        printf("INFO frames received by the fake device: %d of %d\n", s1.start_restore - s0.start_restore, 3 * NSEND);
        CHECK(s1.start_restore - s0.start_restore == 3 * NSEND, "T4: 3 threads x 300 concurrent restored_send frames all arrive whole");
    } else if (!strcmp(c, "reg") || !strcmp(c, "null")) {
        mux(FAKE_DP_OPEN_SILENT);
        int reg = !strcmp(c, "reg");
        if (reg) { CHECK(hv_restore_session_begin() == 0, "begin"); hv_restore_session_allow_usbmuxd((uint16_t)g_mp); }
        idevice_t dev = NULL; idevice_connection_t cn = NULL; int fd = -1;
        CHECK(open_conn(&dev, &cn, &fd) == 0, "connect");
        CHECK(hv_reg_conn_count() == (reg ? 1 : 0), reg ? "T15: the connection is registered while a session exists" : "no session: nothing registered (original behaviour)");
        CHECK(idevice_disconnect(cn) == IDEVICE_E_SUCCESS && !fd_open(fd), "disconnect closes the descriptor");
        CHECK(hv_reg_conn_count() == 0, "and the registration is gone");
        if (reg) { hv_restore_session_close(); CHECK(hv_restore_session_end() == 0, "end"); }
    } else if (!strcmp(c, "wake")) {
        mux(FAKE_DP_OPEN_SILENT);
        hv_restore_session_begin(); hv_restore_session_allow_usbmuxd((uint16_t)g_mp);
        idevice_t dev = NULL; idevice_connection_t cn = NULL; int fd = -1;
        CHECK(open_conn(&dev, &cn, &fd) == 0, "connect");
        pthread_t t; pthread_create(&t, NULL, recv_thread, cn);
        usleep(300000); CHECK(LDV(w_state) == 1, "the receiver is blocked");
        double t0 = now();
        CHECK(hv_restore_session_cancel() == 1, "cancel");
        pthread_join(t, NULL);
        printf("INFO woke after %.0f ms, ret=%d\n", now() - t0, (int)w_ret);
        CHECK(LDV(w_state) == 2 && now() - t0 < 1000, "T8: the blocked receive on a registered connection returned promptly after cancel");
        CHECK(idevice_disconnect(cn) == IDEVICE_E_SUCCESS && hv_reg_conn_count() == 0, "disconnect afterwards works");
        hv_restore_session_close(); CHECK(hv_restore_session_end() == 0, "end");
    } else if (!strcmp(c, "refwait")) {
        mux(FAKE_DP_OPEN_SILENT);
        hv_restore_session_begin(); hv_restore_session_allow_usbmuxd((uint16_t)g_mp); uint64_t g = hv_reg_generation();
        idevice_t dev = NULL; idevice_connection_t cn = NULL; int fd = -1;
        open_conn(&dev, &cn, &fd);
        int fds[4]; int n = hv_reg_conn_acquire(g, fds, 4);   /* what a cancel in progress holds */
        CHECK(n == 1 && fds[0] == fd, "a cancel holds a reference");
        pthread_t t; pthread_create(&t, NULL, disc_thread, cn);
        usleep(150000);
        CHECK(!LDV(d_done) && fd_open(fd), "T18: disconnect waits and the descriptor is still open while the reference is out");
        hv_reg_conn_release(g, fds, n);
        pthread_join(t, NULL);
        CHECK(d_ret == IDEVICE_E_SUCCESS && !fd_open(fd), "after the reference came back the descriptor is closed");
        hv_restore_session_close(); hv_restore_session_end();
    } else if (!strcmp(c, "refleak")) {
        mux(FAKE_DP_OPEN_SILENT);
        hv_restore_session_begin(); hv_restore_session_allow_usbmuxd((uint16_t)g_mp); uint64_t g = hv_reg_generation();
        idevice_t dev = NULL; idevice_connection_t cn = NULL; int fd = -1;
        open_conn(&dev, &cn, &fd);
        int fds[4]; int n = hv_reg_conn_acquire(g, fds, 4);
        idevice_error_t r = idevice_disconnect(cn);   /* the reference never comes back within the deadline */
        CHECK(r != IDEVICE_E_SUCCESS, "disconnect reports failure when a reference did not come back");
        CHECK(fd_open(fd), "and the descriptor is NOT closed (its number cannot be reused under the holder)");
        CHECK(hv_restore_session_close() == -1 && hv_reg_state() == HV_REG_QUIESCENCE_FAILED, "close then fails: the session is QUIESCENCE_FAILED (hard failure recorded)");
        hv_reg_conn_release(g, fds, n);
        close(fd);
    } else if (!strcmp(c, "reuse")) {
        mux(FAKE_DP_OPEN_SILENT);
        hv_restore_session_begin(); hv_restore_session_allow_usbmuxd((uint16_t)g_mp);
        idevice_t dev = NULL; idevice_connection_t cn = NULL; int fd = -1;
        open_conn(&dev, &cn, &fd);
        CHECK(idevice_disconnect(cn) == IDEVICE_E_SUCCESS, "disconnect");
        int sp[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
        int reused = sp[0] == fd || sp[1] == fd;
        printf("INFO descriptor number %d reused by the new socket: %s\n", fd, reused ? "yes" : "no (the race condition was not reproduced by number)");
        CHECK(hv_restore_session_cancel() == 1, "a (late) cancel after the connection was closed");
        char b = 'k'; CHECK(write(sp[0], &b, 1) == 1, "T18: the new socket was not shut down by the stale cancel (write)");
        char r = 0; CHECK(read(sp[1], &r, 1) == 1 && r == 'k', "and its peer receives the data");
        close(sp[0]); close(sp[1]);
        hv_restore_session_close(); hv_restore_session_end();
    } else if (!strcmp(c, "reject_full")) {   /* step 6: the registry refuses the registration (table full): who owns the new descriptor? */
        mux(FAKE_DP_OPEN_SILENT);
        hv_restore_session_begin(); hv_restore_session_allow_usbmuxd((uint16_t)g_mp); uint64_t g = hv_reg_generation();
        int dummy[40], nd = 0;
        for (; nd < 40; nd++) { dummy[nd] = open("/dev/null", O_RDONLY); if (hv_reg_conn_register(g, dummy[nd]) != 0) break; }
        printf("INFO table full after %d registrations\n", nd);
        unsigned char was[256]; int before = 0; for (int i = 0; i < 256; i++) { was[i] = (unsigned char)fd_open(i); before += was[i]; }
        idevice_t dev = NULL; idevice_connection_t cn = NULL; int fd = -1;
        int r = open_conn(&dev, &cn, &fd);
        usleep(300000);   /* the fake server closes its side of the refused connection a moment later: count after that */
        int after = 0; for (int i = 0; i < 256; i++) after += fd_open(i);
        for (int i = 0; i < 256; i++) if (fd_open(i) && !was[i]) { char l[64], t[128] = ""; snprintf(l, sizeof l, "/proc/self/fd/%d", i); ssize_t n = readlink(l, t, sizeof t - 1); if (n > 0) t[n] = 0; printf("INFO new descriptor %d -> %s\n", i, t); }
        CHECK(r != 0 && cn == NULL, "idevice_connect fails when the registration is refused");
        printf("INFO open descriptors before %d, after %d (the refused dummy %d is still open and counted in both)\n", before, after, nd < 40 ? dummy[nd] : -1);
        CHECK(after <= before, "no descriptor of the refused connection stays open");
        CHECK(hv_restore_session_close() == -1 && hv_reg_state() == HV_REG_QUIESCENCE_FAILED, "the refusal is a recorded hard failure: the session cannot reach CLOSED (QUIESCENCE_FAILED)");
        printf("INFO qf reason %d (expected conn_table = %d)\n", (int)hv_reg_qf_reason(), (int)HV_REG_QF_CONN_TABLE);
    } else if (!strcmp(c, "reject_dup")) {   /* step 6: a stale registration for the number the next socket will get */
        mux(FAKE_DP_OPEN_SILENT);
        hv_restore_session_begin(); hv_restore_session_allow_usbmuxd((uint16_t)g_mp); uint64_t g = hv_reg_generation();
        int probe = dup(0); close(probe);   /* the lowest free descriptor number: the connection's socket will most likely get it */
        CHECK(hv_reg_conn_register(g, probe) == 0, "a stale entry for that number (it is not open)");
        int opened_before = 0; for (int i = 0; i < 256; i++) opened_before += fd_open(i);
        idevice_t dev = NULL; idevice_connection_t cn = NULL; int fd = -1;
        int r = open_conn(&dev, &cn, &fd);
        int opened_after = 0; for (int i = 0; i < 256; i++) opened_after += fd_open(i);
        printf("INFO connect %s; descriptor %d, stale number %d, open before %d after %d\n", r == 0 ? "succeeded" : "failed", fd, probe, opened_before, opened_after);
        if (r == 0 && fd == probe) CHECK(0, "the stale entry did not stop the connection (unexpected: registration should have been refused)");
        else if (r != 0) CHECK(opened_after <= opened_before + 1, "refused duplicate: its descriptor is closed (no leak)");
        else printf("INFO the socket did not get the stale number: the duplicate case was not reproduced\n");
        hv_restore_session_close();
    } else { printf("unknown case\n"); return 2; }
    fflush(stdout);
    return nfail ? 1 : 0;
}
