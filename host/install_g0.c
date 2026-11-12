/* G0 test build only: see install_g0.h. */
#define _GNU_SOURCE 1
#include "install_g0.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "crypto_portable.h"
#include "idevicerestore/src/common.h"
#include "idevicerestore/src/idevicerestore.h"
#include "idevicerestore/src/hv_stop.h"
#include "idevicerestore/src/log.h"
#include "install.h"
#include "net_guard.h"
#include "usb_host.h"
#include "vm.h"

#ifndef HV_STOP_BEFORE_ROOTFS
#error "install_g0.c needs the vendored restore tool built with -DHV_STOP_BEFORE_ROOTFS"
#endif

void hv_curl_deny_network(int on);
int hv_curl_deny_active(void);
int hv_curl_attempt_count(void);
int hv_curl_denied_count(void);

/* ---- what the restore thread reports (host/install.c) ---- */
static volatile int g_stop_called;
static volatile int g_rec_done, g_rec_engine_rc = -12345, g_rec_closed, g_rec_cleanup = -1, g_rec_report = -1;

static void stop_a(const struct hv_stop_a_info *i) {
    (void)i;   /* nothing of the build identity is kept or printed here; preflight (host/restore_preflight.c) reports it */
    __atomic_store_n(&g_stop_called, 1, __ATOMIC_SEQ_CST);
}
int hv_g0_extra_flags(void) {
    hv_stop_a_hook = stop_a;
    return FLAG_STOP_BEFORE_ROOTFS;
}
void hv_g0_record(int engine_rc, int closed, int cleanup_mask, int final_report) {
    g_rec_engine_rc = engine_rc;
    g_rec_closed = closed;
    g_rec_cleanup = cleanup_mask;
    g_rec_report = final_report;
    __atomic_store_n(&g_rec_done, 1, __ATOMIC_SEQ_CST);
}

/* ---- the tool's log: the ECID is never printed (same rule as host/restore_preflight.c) ---- */
static char g_ecid_dec[32], g_ecid_hex[32], g_ecid_HEX[32];
static void redact(char *line) {
    const char *pats[3] = {g_ecid_dec, g_ecid_hex, g_ecid_HEX};
    for (int k = 0; k < 3; k++) {
        if (!pats[k][0]) continue;
        char *p;
        while ((p = strstr(line, pats[k])) != NULL) {
            size_t pl = strlen(pats[k]);
            memcpy(p, "<ecid>", 6 <= pl ? 6 : pl);
            if (pl > 6) memmove(p + 6, p + pl, strlen(p + pl) + 1);
        }
    }
}
static void log_print(enum loglevel level, const char *fmt, va_list ap) {
    char line[1024];
    vsnprintf(line, sizeof line, fmt, ap);
    if (!strncmp(line, "ECID:", 5)) { fputs("ECID: <ecid>\n", stdout); return; }
    redact(line);
    fputs(line, stdout);
    (void)level;
}

/* ---- identity of a storage file: size, modification time and content ---- */
#define CONTENT_CAP (2ULL << 30)   /* more data than this: size and modification time only (said in the table) */
typedef struct { int ok, mode; uint64_t size, data_bytes; long long mtime_ns; uint8_t acc[32]; } FileId;   /* mode 1 content (data extents), 2 size + mtime only */

static void mix(uint8_t acc[32], const uint8_t digest[32], uint64_t off) {
    uint8_t buf[72];
    memcpy(buf, acc, 32), memcpy(buf + 32, digest, 32);
    for (int i = 0; i < 8; i++) buf[64 + i] = (uint8_t)(off >> (8 * i));
    hv_sha256(buf, sizeof buf, acc);
}
static void file_id(const char *path, FileId *o) {
    memset(o, 0, sizeof *o);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return; }
    o->size = (uint64_t)st.st_size;
    o->mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    uint8_t sz[8];
    for (int i = 0; i < 8; i++) sz[i] = (uint8_t)(o->size >> (8 * i));
    hv_sha256(sz, 8, o->acc);
    /* data extents: holes of a sparse file cost nothing. A file system without SEEK_DATA is treated as all data. */
    uint64_t data = 0;
    int sparse_ok = 1;
    off_t pos = 0;
    while ((uint64_t)pos < o->size) {
        off_t ds = lseek(fd, pos, SEEK_DATA);
        if (ds < 0) { if (errno != ENXIO) { sparse_ok = 0; } break; }
        off_t de = lseek(fd, ds, SEEK_HOLE);
        if (de < 0) { sparse_ok = 0; break; }
        data += (uint64_t)(de - ds);
        pos = de;
    }
    if (!sparse_ok) data = o->size;
    o->data_bytes = data;
    if (data > CONTENT_CAP) { o->mode = 2; o->ok = 1; close(fd); return; }
    o->mode = 1;
    uint8_t *buf = malloc(1 << 20);
    if (!buf) { close(fd); return; }
    int bad = 0;
    if (sparse_ok) {
        pos = 0;
        while ((uint64_t)pos < o->size && !bad) {
            off_t ds = lseek(fd, pos, SEEK_DATA);
            if (ds < 0) break;
            off_t de = lseek(fd, ds, SEEK_HOLE);
            if (de < 0) { bad = 1; break; }
            for (off_t at = ds; at < de && !bad;) {
                size_t n = (size_t)(de - at > (1 << 20) ? (1 << 20) : de - at);
                ssize_t got = pread(fd, buf, n, at);
                if (got <= 0) { bad = 1; break; }
                uint8_t d[32];
                hv_sha256(buf, (size_t)got, d);
                mix(o->acc, d, (uint64_t)at);
                at += got;
            }
            pos = de;
        }
    } else {   /* the whole file as one extent (it is at most CONTENT_CAP) */
        for (uint64_t at = 0; at < o->size && !bad;) {
            size_t n = (size_t)(o->size - at > (1 << 20) ? (1 << 20) : o->size - at);
            ssize_t got = pread(fd, buf, n, (off_t)at);
            if (got <= 0) { bad = 1; break; }
            uint8_t d[32];
            hv_sha256(buf, (size_t)got, d);
            mix(o->acc, d, at);
            at += (uint64_t)got;
        }
    }
    free(buf);
    close(fd);
    o->ok = !bad;
}
static void hex(const uint8_t *b, char *out) { for (int i = 0; i < 8; i++) sprintf(out + 2 * i, "%02x", b[i]); }   /* the first 8 bytes of the digest: enough to compare, short enough to read */

/* ---- state ---- */
static FileId g_disk0, g_aux0;
static int g_began, g_started, g_online_ok, g_att0, g_den0, g_ctrl0, g_data0, g_waited = -2, g_port;
static int g_ecid_set;

#define MAXTH 64
typedef struct { int n; char name[MAXTH][32]; } ThSnap;
static ThSnap g_th0;
static void th_snap(ThSnap *t) {
    t->n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) { t->n = -1; return; }
    struct dirent *e;
    while ((e = readdir(d)) && t->n < MAXTH) {
        if (e->d_name[0] == '.') continue;
        char path[64], nm[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0;
        fclose(f);
        snprintf(t->name[t->n++], sizeof t->name[0], "%s", nm);
    }
    closedir(d);
}
static int th_count(const ThSnap *t, const char *nm) { int c = 0; for (int i = 0; i < t->n; i++) if (!strcmp(t->name[i], nm)) c++; return c; }
static int mux_listening(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int hv_g0_begin(const char *disk, const char *aux, uint64_t ecid) {
    struct stat sb;
    if (stat(disk, &sb) != 0 || stat(aux, &sb) != 0) {
        printf("install g0: refused -- the disk and the auxiliary storage must already exist (G0 creates nothing); run a boot test first\n");
        return -1;
    }
    if (!ecid) { printf("install g0: refused -- no --ecid (G0 never makes one up)\n"); return -1; }
    file_id(disk, &g_disk0);
    file_id(aux, &g_aux0);
    if (!g_disk0.ok || !g_aux0.ok) { printf("install g0: refused -- the storage files could not be read to take their identity\n"); return -1; }
    snprintf(g_ecid_dec, sizeof g_ecid_dec, "%llu", (unsigned long long)ecid);
    snprintf(g_ecid_hex, sizeof g_ecid_hex, "%llx", (unsigned long long)ecid);
    snprintf(g_ecid_HEX, sizeof g_ecid_HEX, "%llX", (unsigned long long)ecid);
    g_ecid_set = 1;
    logger_set_logfile("NONE");
    logger_set_print_func(log_print);
    /* belt and braces: the curl stand-in refuses every request before any lookup (the session's connection policy is the second guard) */
    hv_curl_deny_network(1);
    if (!hv_curl_deny_active()) { printf("install g0: refused -- the curl stand-in's deny mode did not turn on\n"); return -1; }
    g_att0 = hv_curl_attempt_count();
    g_den0 = hv_curl_denied_count();
    g_began = 1;
    return 0;
}

int hv_g0_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid) {
    if (!g_began || !usb) return -1;
    if (hv_usb_wait_online(usb, 60000) < 0) { printf("install g0: the guest did not come on the USB bus within 60 s; the restore was not started\n"); return -1; }
    g_online_ok = 1;
    g_ctrl0 = hv_libusb_ctrl_count();
    g_data0 = hv_libusb_data_transfer_count();
    th_snap(&g_th0);
    g_port = 27100 + (int)(getpid() % 800);   /* the same rule as host/install.c */
    if (hv_install_start(usb, ipsw, workdir, ecid, NULL, NULL) != 0) { printf("install g0: hv_install_start refused to start\n"); return -1; }
    g_started = 1;
    return 0;
}

int hv_g0_wait(int cap_ms) {
    if (!g_started) return 1;
    double t0 = now_ms();
    while (hv_install_result() == 1 && now_ms() - t0 < cap_ms) usleep(20000);
    if (hv_install_result() != 1) { g_waited = 1; return 1; }
    hv_install_cancel();   /* a request, not an end: the thread must still be seen to finish */
    double t1 = now_ms();
    while (hv_install_result() == 1 && now_ms() - t1 < 20000) usleep(20000);
    g_waited = hv_install_result() == 1 ? -1 : 0;
    return g_waited;
}

/* ---- the table ---- */
static int g_failed;
static void row(FILE *out, const char *name, int ok, const char *fmt, ...) {
    char d[700];
    va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap);
    fprintf(out, "install g0: %-18s %-6s %s\n", name, ok ? "ok" : "FAILED", d);
    if (!ok) g_failed++;
}

int hv_g0_finish(FILE *out, const char *disk, const char *aux) {
    g_failed = 0;
    int ended = g_started && g_waited != -1 && hv_install_result() != 1;
    usleep(500000);   /* the tool's event thread needs a moment to wind down */
    fprintf(out, "install g0: the install entry point (host/install.c) ran with the tool's stop A; this is NOT a restore: nothing was personalized, sent, extracted or installed\n");
    row(out, "started", g_started, g_started ? "hv_install_start accepted the run (guest on the bus: %d)" : "the run did not start (guest on the bus: %d)", g_online_ok);
    row(out, "ended", ended && g_waited == 1, "%s", g_waited == 1 ? "the restore thread ended by itself" : g_waited == 0 ? "it did NOT end by itself: a cancel was requested (hv_install_cancel) and then it ended -- the time limit was hit, so this is a failure" : g_waited == -1 ? "the restore thread did NOT end, not even after the cancel request; nothing was released or closed under it" : "not started");
    if (g_waited == -1 || !g_started) {
        fprintf(out, "install g0 RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
        return g_failed;
    }
    int sa = __atomic_load_n(&g_stop_called, __ATOMIC_SEQ_CST), rec = __atomic_load_n(&g_rec_done, __ATOMIC_SEQ_CST);
    row(out, "stop_a_hook", sa, "%s", sa ? "the tool called the stop-A report" : "the tool never reached stop A");
    row(out, "engine_rc", rec && g_rec_engine_rc == HV_STOP_BEFORE_ROOTFS_RC, "the tool returned %d (stop A returns %d)", g_rec_engine_rc, HV_STOP_BEFORE_ROOTFS_RC);
    row(out, "session", rec && g_rec_closed && g_rec_cleanup == 0 && g_rec_report == 2, "closed %d, cleanup_mask 0x%x (0 = the client was released and the private usbmuxd stopped and joined), final report %d (2 = \"restore failed\" is what a stop-A return is classified as; 5 cleanup failed and 6 not quiescent are failures)", g_rec_closed, g_rec_cleanup, g_rec_report);
    row(out, "usbmuxd", !mux_listening(g_port), "the private usbmuxd's loopback socket %s", mux_listening(g_port) ? "STILL LISTENS" : "no longer answers");
    int att = hv_curl_attempt_count() - g_att0, den = hv_curl_denied_count() - g_den0;
    row(out, "network_curl", att == 0, "curl stand-in: %d attempt(s), %d refused (scope: the curl client only)", att, den);
    HvNetStats ns; hv_net_guard_stats(&ns);
    row(out, "connections", ns.denied == 0 && ns.url_attempts == 0, "connection guard: %d attempt(s), %d allowed (the private usbmuxd listener), %d refused, URLAsset %d; a refusal means the run went past where it had to stop, so it is not a pass (a direct connect() elsewhere in the process, and the guest's own network, which vmrun leaves off, are not covered)", ns.attempts, ns.allowed, ns.denied, ns.url_attempts);
    {
        int c1 = hv_libusb_ctrl_count(), d1 = hv_libusb_data_transfer_count();
        int forbidden = 0, lost = 0, getstatus = 0, total = c1 - g_ctrl0;
        for (int k = g_ctrl0; k < c1; k++) {
            HvCtrlRecord r;
            if (!hv_libusb_ctrl_get(k, &r)) { lost++; continue; }
            uint8_t type = (r.bmRequestType >> 5) & 3;
            if (type == 2) forbidden++;
            else if (type == 1 && (r.bRequest == 0 || r.bRequest == 1 || r.bRequest == 2 || r.bRequest == 4 || r.bRequest == 6)) forbidden++;
            else if (type == 1 && r.bRequest == 3) getstatus++;
        }
        row(out, "device_requests", !forbidden && !lost && d1 == g_data0, "%d control request(s) (%d DFU GET_STATUS), %d forbidden (download/upload/detach/abort/clear-status/vendor), %d no longer in the log, %d bulk/interrupt transfer(s) (must be 0); counted at the libusb stand-in", total, getstatus, forbidden, lost, d1 - g_data0);
    }
    uint64_t wc, wr;
    hv_vm_disk_write_stats(&wc, &wr);
    row(out, "storage_writes", wc == 0, "the disk and the auxiliary storage were opened read-only; writes the devices asked for: %llu, refused: %llu (must be 0 asked)", (unsigned long long)wc, (unsigned long long)wr);
    FileId d1, a1;
    file_id(disk, &d1); file_id(aux, &a1);
    char h0[20], h1[20];
    hex(g_disk0.acc, h0); hex(d1.acc, h1);
    row(out, "disk_identity", d1.ok && d1.size == g_disk0.size && d1.mtime_ns == g_disk0.mtime_ns && !memcmp(d1.acc, g_disk0.acc, 32) && d1.mode == g_disk0.mode,
        "size %llu -> %llu, modification time %s, %s %s -> %s%s", (unsigned long long)g_disk0.size, (unsigned long long)d1.size, d1.mtime_ns == g_disk0.mtime_ns ? "same" : "CHANGED",
        d1.mode == 1 ? "content (data extents, sparse holes skipped)" : "NO content hash (more than 2 GiB of data: size and modification time only)", h0, h1, d1.mode == 1 ? "" : " (not a content check)");
    hex(g_aux0.acc, h0); hex(a1.acc, h1);
    row(out, "aux_identity", a1.ok && a1.size == g_aux0.size && a1.mtime_ns == g_aux0.mtime_ns && !memcmp(a1.acc, g_aux0.acc, 32) && a1.mode == g_aux0.mode,
        "size %llu -> %llu, modification time %s, %s %s -> %s%s", (unsigned long long)g_aux0.size, (unsigned long long)a1.size, a1.mtime_ns == g_aux0.mtime_ns ? "same" : "CHANGED",
        a1.mode == 1 ? "content" : "NO content hash (size and modification time only)", h0, h1, a1.mode == 1 ? "" : " (not a content check)");
    {
        /* by name, like the preflight: no name may have more threads than before the run (a thread replaced by one of the same name is not seen) */
        ThSnap t1;
        th_snap(&t1);
        char grown[160] = "";
        int grew = 0;
        for (int i = 0; i < t1.n; i++)
            if (th_count(&t1, t1.name[i]) > th_count(&g_th0, t1.name[i]) && !strstr(grown, t1.name[i])) { grew++; strncat(grown, t1.name[i], sizeof grown - strlen(grown) - 2); strncat(grown, " ", sizeof grown - strlen(grown) - 1); }
        row(out, "threads", t1.n >= 0 && g_th0.n >= 0 && !grew && t1.n < MAXTH && g_th0.n < MAXTH, "%d before the run (guest on the bus), %d after; %s%s", g_th0.n, t1.n, grew ? "MORE threads than before for: " : "no name has more threads than before", grown);
    }
    if (g_failed == 0) fprintf(out, "install g0 RESULT: g0_passed stopped=before_rootfs (observed: hook called, tool returned %d), session report %d, 0 failed row(s). This is NOT a restore.\n", HV_STOP_BEFORE_ROOTFS_RC, g_rec_report);
    else fprintf(out, "install g0 RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
    (void)g_ecid_set;
    return g_failed;
}
