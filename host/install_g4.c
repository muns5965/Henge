/* G4 diagnostic build only: see install_g4.h. */
#define _GNU_SOURCE 1
#include "install_g4.h"

#include <arpa/inet.h>
#include <ctype.h>
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

#ifndef HV_STOP_AFTER_RECOVERYOS_ENTRY
#error "install_g4.c needs the vendored restore tool built with -DHV_STOP_AFTER_RECOVERYOS_ENTRY"
#endif

int hv_curl_attempt_count(void);
const char *tss_last_diag(void);

/* ---- what the restore thread reports (host/install.c) ---- */
static volatile int g_rec_done, g_rec_engine_rc = -12345, g_rec_closed, g_rec_cleanup = -1, g_rec_report = -1;
/* Same three real TSS hosts G2/G3 allow (third_party/libtatsu/src/tss.c's own https-only list): this stage makes no network request of its own -- it only
   consumes the TSS tickets (including the ApTicket recovery_send_ticket() sends) fetched earlier in the same call, so no new host is added here. */
static const struct { const char *host; } k_tss_hosts[] = {{"gs.apple.com"}, {"gs.apple.com.akadns.net"}, {"gs.apple.com.v.aaplimg.com"}};
static int g_host_alloc_failed;

/* ---- stop C: reported by the vendored tool right after RecoveryOS entry is confirmed (see hv_stop.h) ---- */
static volatile int g_stop_c_called;
static int g_mode_before = -1, g_mode_after = -1, g_tss_enabled = -1, g_macos_variant = -1;
static void stop_c(const struct hv_stop_c_info *i) {
    g_mode_before = i->mode_before;
    g_mode_after = i->mode_after;
    g_tss_enabled = i->tss_enabled;
    g_macos_variant = i->macos_variant;
    __atomic_store_n(&g_stop_c_called, 1, __ATOMIC_SEQ_CST);
}

int hv_g4_extra_flags(void) {
    for (size_t i = 0; i < sizeof k_tss_hosts / sizeof *k_tss_hosts; i++)
        if (hv_net_guard_allow_host(k_tss_hosts[i].host, 443) != 0) g_host_alloc_failed = 1;
    hv_stop_c_hook = stop_c;
    /* NOT FLAG_SHSHONLY and NOT FLAG_STOP_AFTER_DFU_RECOVERY: G4 must run PAST both the TSS stage (G2) and the DFU->Recovery firmware transfer (G3) and
       into recovery_enter_restore(); FLAG_STOP_AFTER_RECOVERYOS_ENTRY is the only thing that stops it, right after RecoveryOS entry is confirmed, before
       any disk-writing step. */
    return FLAG_STOP_AFTER_RECOVERYOS_ENTRY;
}
void hv_g4_record(int engine_rc, int closed, int cleanup_mask, int final_report) {
    g_rec_engine_rc = engine_rc;
    g_rec_closed = closed;
    g_rec_cleanup = cleanup_mask;
    g_rec_report = final_report;
    __atomic_store_n(&g_rec_done, 1, __ATOMIC_SEQ_CST);
}

/* ---- the tool's log: a whitelist, G3's plus the lines recovery_enter_restore()'s own path adds. "Sending" already covers "Sending APTicket (...)",
   "Sending <component> (... bytes)..." and "Sending <component>..." (recovery_send_ticket/recovery_send_component/recovery_send_applelogo all start that
   way), so no separate entry is needed for those. */
static const char *const k_ok[] = {"Found device in DFU mode", "Identified device as", "Device Product", "Extracting BuildManifest", "IPSW Product", "Device supports Image4",
                                   "Variant:", "This restore will erase", "Performing macOS restore", "Checking IPSW for required", "All required components",
                                   "Received SHSH blobs", "SHSH not saved", "Sending",
                                   "Recovery Mode Environment:", "iBoot build-version=", "iBoot build-style=", "iBoot boot-stage=", "ramdisk-size=",
                                   "Waiting for device to enter restore mode", NULL};
static volatile int g_lines_shown, g_lines_withheld, g_lines_sensitive;
static int looks_sensitive(const char *l) {
    if (!strncmp(l, "ECID", 4) || !strncmp(l, "Nonce", 5) || !strncmp(l, "ApNonce", 7) || !strncasecmp(l, "SepNonce", 8) || !strncasecmp(l, "Ticket", 6)) return 1;
    int run = 0;   /* a hex dump: "00 11 22 ..." (the tool's own format) or a long run of hex digits */
    for (const char *p = l; *p; p++) {
        if (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) && (p[2] == ' ' || p[2] == '\n' || !p[2])) { run++; p += 2; if (run >= 8) return 1; }
        else if (!isspace((unsigned char)*p)) run = 0;
    }
    int hx = 0;
    for (const char *p = l; *p; p++) { if (isxdigit((unsigned char)*p)) { if (++hx >= 32) return 1; } else hx = 0; }
    return 0;
}
static char g_ecid_dec[32], g_ecid_hex[32], g_ecid_HEX[32];
static void redact_name(char *line) {
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
static int filter_line(const char *line) {
    int sens = looks_sensitive(line);
    for (int i = 0; k_ok[i]; i++)
        if (!sens && !strncmp(line, k_ok[i], strlen(k_ok[i]))) { fputs(line, stdout); __atomic_add_fetch(&g_lines_shown, 1, __ATOMIC_SEQ_CST); return 1; }
    __atomic_add_fetch(&g_lines_withheld, 1, __ATOMIC_SEQ_CST);
    if (sens) __atomic_add_fetch(&g_lines_sensitive, 1, __ATOMIC_SEQ_CST);
    return 0;
}
static void log_print(enum loglevel level, const char *fmt, va_list ap) {
    char line[1024];
    vsnprintf(line, sizeof line, fmt, ap);
    (void)level;
    filter_line(line);
}

/* ---- identity of a storage file: size, modification time and content (same method as host/install_g0.c/install_g2.c/install_g3.c) ---- */
static uint64_t g_content_cap = 2ULL << 30;
typedef struct { int ok, mode; uint64_t size, data_bytes; long long mtime_ns; uint8_t acc[32]; } FileId;

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
    if (data > g_content_cap) { o->mode = 2; o->ok = 1; close(fd); return; }
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
    } else {
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
static void hex(const uint8_t *b, char *out) { for (int i = 0; i < 8; i++) sprintf(out + 2 * i, "%02x", b[i]); }

/* ---- state ---- */
static FileId g_disk0, g_aux0;
static int g_began, g_started, g_online_ok, g_att0, g_ctrl0, g_data0, g_waited = -2, g_port;
static int g_data_ok0, g_data_bad0;
static uint64_t g_data_bytes0;
static unsigned g_hist0[4][8]; /* hv_libusb_ctrl_hist() baseline, taken at the same moment as g_ctrl0: never evicted, so it survives past HV_CTRL_LOG_MAX */
static HvNetStats g_ns0;

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
        char path[300], nm[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%.255s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0;
        fclose(f);
        snprintf(t->name[t->n++], sizeof t->name[0], "%s", nm);
    }
    closedir(d);
}
static int th_count(const ThSnap *t, const char *nm) { int c = 0; for (int i = 0; i < t->n; i++) if (!strcmp(t->name[i], nm)) c++; return c; }

/* ---- the work directory: top-level entries only, before the run and after (same idea as install_g2.c/install_g3.c's local_writes/ticket_files rows) ---- */
#define MAXWF 512
typedef struct { int n; char name[MAXWF][200]; uint64_t size[MAXWF]; long long mtime[MAXWF]; } WorkSnap;
static WorkSnap g_wf0;
static char g_workdir[512];
static void wf_snap(const char *dir, WorkSnap *w) {
    w->n = 0;
    if (!dir || !dir[0]) return;
    DIR *d = opendir(dir);
    if (!d) { w->n = -1; return; }
    struct dirent *e;
    while ((e = readdir(d)) && w->n < MAXWF) {
        if (e->d_name[0] == '.') continue;
        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        snprintf(w->name[w->n], sizeof w->name[0], "%.199s", e->d_name);
        w->size[w->n] = (uint64_t)st.st_size;
        w->mtime[w->n] = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        w->n++;
    }
    closedir(d);
}
static int wf_find(const WorkSnap *w, const char *name) { for (int i = 0; i < w->n; i++) if (!strcmp(w->name[i], name)) return i; return -1; }
static int looks_like_ticket_name(const char *n) {
    char l[200]; int i = 0;
    for (; n[i] && i < 199; i++) l[i] = (char)tolower((unsigned char)n[i]);
    l[i] = 0;
    return strstr(l, "shsh") || strstr(l, "ticket") || strstr(l, "img4") || strstr(l, "apimg");
}

static int mux_listening(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int hv_g4_begin(const char *disk, const char *aux, uint64_t ecid) {
    struct stat sb;
    if (stat(disk, &sb) != 0 || stat(aux, &sb) != 0) {
        printf("install g4: refused -- the disk and the auxiliary storage must already exist (G4 creates nothing); run a boot test first\n");
        return -1;
    }
    if (!ecid) { printf("install g4: refused -- no --ecid (G4 never makes one up)\n"); return -1; }
    file_id(disk, &g_disk0);
    file_id(aux, &g_aux0);
    if (!g_disk0.ok || !g_aux0.ok) { printf("install g4: refused -- the storage files could not be read to take their identity\n"); return -1; }
    snprintf(g_ecid_dec, sizeof g_ecid_dec, "%llu", (unsigned long long)ecid);
    snprintf(g_ecid_hex, sizeof g_ecid_hex, "%llx", (unsigned long long)ecid);
    snprintf(g_ecid_HEX, sizeof g_ecid_HEX, "%llX", (unsigned long long)ecid);
    logger_set_logfile("NONE");
    logger_set_print_func(log_print);
    g_att0 = hv_curl_attempt_count();
    g_began = 1;
    return 0;
}

int hv_g4_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid) {
    if (!g_began || !usb) return -1;
    if (hv_usb_wait_online(usb, 60000) < 0) { printf("install g4: the guest did not come on the USB bus within 60 s; the restore was not started\n"); return -1; }
    g_online_ok = 1;
    g_ctrl0 = hv_libusb_ctrl_count();
    g_data0 = hv_libusb_data_transfer_count();
    g_data_ok0 = hv_libusb_data_transfer_ok_count();
    g_data_bad0 = hv_libusb_data_transfer_bad_count();
    g_data_bytes0 = hv_libusb_data_transfer_bytes();
    for (int t = 0; t < 4; t++) for (int b = 0; b < 8; b++) g_hist0[t][b] = hv_libusb_ctrl_hist(t, b);
    th_snap(&g_th0);
    hv_net_guard_stats(&g_ns0);
    g_port = 27100 + (int)(getpid() % 800);   /* the same rule as host/install.c */
    snprintf(g_workdir, sizeof g_workdir, "%s", workdir ? workdir : "");
    wf_snap(g_workdir, &g_wf0);
    if (hv_install_start(usb, ipsw, workdir, ecid, NULL, NULL) != 0) { printf("install g4: hv_install_start refused to start\n"); return -1; }
    g_started = 1;
    return 0;
}

int hv_g4_wait(int cap_ms) {
    if (!g_started) return 1;
    double t0 = now_ms();
    while (hv_install_result() == 1 && now_ms() - t0 < cap_ms) usleep(20000);
    if (hv_install_result() != 1) { g_waited = 1; return 1; }
    hv_install_cancel();
    double t1 = now_ms();
    while (hv_install_result() == 1 && now_ms() - t1 < 20000) usleep(20000);
    g_waited = hv_install_result() == 1 ? -1 : 0;
    return g_waited;
}

/* ---- libtatsu's own diagnosis of the LAST request made (never the server's text) ---- */
typedef struct { int kind, attempts, http, status, curl; char cls[44]; } Diag;
static int cls_ok(const char *c) { size_t n = strlen(c); if (!n || n >= 26) return 0; for (; *c; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0; return 1; }
static int parse_diag(const char *d, Diag *o) {
    memset(o, 0, sizeof *o);
    o->attempts = o->http = o->status = o->curl = -1;
    if (!d[0]) { o->kind = 4; return 1; }   /* EMPTY */
    int a, h, v, cc, n;
    char cl[40];
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / TRANSPORT: curl %d %39s / MESSAGE_RAW: suppressed%n", &a, &cc, cl, &n) == 3 && n > 0 && !d[n] && cls_ok(cl)) { o->kind = 3; o->attempts = a; o->curl = cc; snprintf(o->cls, sizeof o->cls, "%s", cl); return 1; }   /* TRANSPORT */
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / TSS_STATUS: 0 / MESSAGE_CLASS: SUCCESS / MESSAGE_RAW: suppressed%n", &a, &h, &n) == 2 && n > 0 && !d[n]) { o->kind = 0; o->attempts = a; o->http = h; o->status = 0; snprintf(o->cls, sizeof o->cls, "SUCCESS"); return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / NOT_A_TSS_REPLY / MESSAGE_RAW: suppressed%n", &a, &h, &n) == 2 && n > 0 && !d[n]) { o->kind = 1; o->attempts = a; o->http = h; return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / SUCCESS_BUT_NOT_A_PLIST / MESSAGE_RAW: suppressed%n", &a, &n) == 1 && n > 0 && !d[n]) { o->kind = 5; o->attempts = a; return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / TSS_STATUS: %d / MESSAGE_CLASS: %39s / MESSAGE_RAW: suppressed%n", &a, &h, &v, cl, &n) == 4 && n > 0 && !d[n] && cls_ok(cl)) { o->kind = 2; o->attempts = a; o->http = h; o->status = v; snprintf(o->cls, sizeof o->cls, "%s", cl); return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / TSS_STATUS: OTHER / MESSAGE_CLASS: %39s / MESSAGE_RAW: suppressed%n", &a, &h, cl, &n) == 3 && n > 0 && !d[n] && cls_ok(cl)) { o->kind = 2; o->attempts = a; o->http = h; o->status = -2; snprintf(o->cls, sizeof o->cls, "%s", cl); return 1; }
    return 0;
}
static const char *diag_name(int k) { static const char *n[] = {"success", "not_a_tss_reply", "status", "transport", "empty", "success_but_not_a_plist"}; return k >= 0 && k <= 5 ? n[k] : "?"; }
static const char *mode_name(int idx) { static const char *n[] = {"unknown", "wtf", "dfu", "recovery", "restore", "normal", "portdfu"}; return idx >= 0 && idx <= 6 ? n[idx] : "?"; }

/* ---- the table ---- */
static int g_failed, g_meta;
static void row(FILE *out, const char *name, int ok, const char *fmt, ...) {
    char d[1400];
    va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap);
    fprintf(out, "install g4: %-16s %-6s %s\n", name, ok ? "ok" : "FAILED", d);
    if (!ok) g_failed++;
}
static void row_meta(FILE *out, const char *name, const char *fmt, ...) {
    char d[700];
    va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap);
    fprintf(out, "install g4: %-16s %-6s %s\n", name, "META", d);
    g_meta++;
}

int hv_g4_finish(FILE *out, const char *disk, const char *aux) {
    g_failed = 0;
    g_meta = 0;
    int ended = g_started && g_waited != -1 && hv_install_result() != 1;
    usleep(500000);
    fprintf(out, "install g4: the install entry point (host/install.c) ran the real restore tool through TSS, the DFU->Recovery firmware transfer and recovery_enter_restore() against the real Apple signing server, then stopped (stop C); this is NOT a restore: no disk write happens before this point\n");
    row(out, "started", g_started, g_started ? "hv_install_start accepted the run (guest on the bus: %d)" : "the run did not start (guest on the bus: %d)", g_online_ok);
    row(out, "net_guard_hosts", !g_host_alloc_failed, "%s", g_host_alloc_failed ? "one or more of the three TSS hosts could not be added to the allow list; the run proceeded, so some signing requests may have been refused" : "the three TSS hosts (gs.apple.com and its two CDN aliases, port 443) were added to the allow list before the run started; no other host is allowed");
    row(out, "ended", ended && g_waited == 1, "%s", g_waited == 1 ? "the restore thread ended by itself" : g_waited == 0 ? "it did NOT end by itself: a cancel was requested and then it ended -- the time limit was hit, so this is a failure" : g_waited == -1 ? "the restore thread did NOT end, not even after the cancel request; nothing was released or closed under it" : "not started");
    if (g_waited == -1 || !g_started) {
        fprintf(out, "install g4 RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
        return g_failed;
    }
    int rec = __atomic_load_n(&g_rec_done, __ATOMIC_SEQ_CST);
    row(out, "engine_rc", rec && g_rec_engine_rc == HV_STOP_AFTER_RECOVERYOS_ENTRY_RC, "the tool returned %d (stop C, right after RecoveryOS entry is confirmed, returns %d; anything else means it stopped somewhere else or did not reach this point)", g_rec_engine_rc, HV_STOP_AFTER_RECOVERYOS_ENTRY_RC);
    row(out, "stop_c_hook", __atomic_load_n(&g_stop_c_called, __ATOMIC_SEQ_CST), "%s", __atomic_load_n(&g_stop_c_called, __ATOMIC_SEQ_CST) ? "the tool called the stop-C report" : "the tool never reached stop C");
    row(out, "mode_transition", g_mode_before == 3 && g_mode_after == 4, "device mode before this step: %s, after: %s (expected recovery -> restore)", mode_name(g_mode_before), mode_name(g_mode_after));
    row(out, "session", rec && g_rec_closed && g_rec_cleanup == 0 && g_rec_report == 2, "closed %d, cleanup_mask 0x%x (0 = the client was released and the private usbmuxd stopped and joined), final report %d (expected 2: the session always classifies a non-zero/non-success engine return as a failed restore, which is correct -- a stop-C return is not 0 and must not be treated as a completed restore)", g_rec_closed, g_rec_cleanup, g_rec_report);
    row(out, "usbmuxd", !mux_listening(g_port), "the private usbmuxd's loopback socket %s", mux_listening(g_port) ? "STILL LISTENS" : "no longer answers");
    int att = hv_curl_attempt_count() - g_att0;
    row(out, "tss_requests", att >= 1, "the curl client made %d request attempt(s) against the real Apple signing server (at least 1 expected: the TSS stage runs before stop B, well before stop C)", att);
    {
        Diag dg;
        int parsed = parse_diag(tss_last_diag(), &dg);
        row(out, "tss_diag", parsed, "libtatsu's diagnosis of the LAST request made: %s; kind=%s attempts=%d http=%d tss_status=%d curl_code=%d class=%s (describes the last request only; the real content of any request or reply is never printed)",
            parsed ? "parsed" : "UNKNOWN FORMAT (not printed)", parsed ? diag_name(dg.kind) : "?", dg.attempts, dg.http, dg.status, dg.curl, dg.cls[0] ? dg.cls : "-");
    }
    HvNetStats ns; hv_net_guard_stats(&ns);
    int den = ns.denied - g_ns0.denied, allowed = ns.allowed - g_ns0.allowed;
    row(out, "connections", den == 0 && ns.url_attempts == 0, "connection guard: %d attempt(s), %d allowed (the private usbmuxd listener and the three approved TSS hosts), %d refused, URLAsset %d; any refusal means something besides the private usbmuxd and the three TSS hosts was reached for, which is not a pass (a direct connect() elsewhere in the process is not covered)", ns.attempts - g_ns0.attempts, allowed, den, ns.url_attempts);
    {
        /* Same classification G3 uses (see install_g3.c for the full source citations): recovery_enter_restore() sends its components -- AppleLogo, the
           components iBoot loads, the restore ramdisk, the device tree, (SEP firmware), the kernelcache -- through the exact same irecv_send_buffer()/
           irecv_send_command()/irecv_send_command_breq() calls already analyzed for G3, just more of them (and this run accumulates G3's own requests too,
           since it runs through that same stage first). DFU DNLOAD (class/1) still carries every bulk component upload in Recovery mode; the vendor
           handshake (0x41, bRequest 0) and command_execute (bRequest 1, "ticket"/"devicetree"/"ramdisk"/"go"/... -- the histogram still can't say which)
           cover the rest of the legitimate vendor traffic. Only a vendor-class bRequest other than 0 or 1, DETACH/UPLOAD (limera1n-only, not reachable
           here), or a reserved request-type remain forbidden. */
        unsigned h[4][8];
        for (int t = 0; t < 4; t++) for (int b = 0; b < 8; b++) h[t][b] = hv_libusb_ctrl_hist(t, b) - g_hist0[t][b];
        int dnload = (int)h[1][1], getstatus = (int)h[1][3], getstate = (int)h[1][5], reset = (int)h[1][4] + (int)h[1][6];
        int handshake = (int)h[2][0], command_execute = (int)h[2][1], enumreq = 0, forbidden = 0;
        for (int b = 0; b < 8; b++) enumreq += (int)h[0][b];
        forbidden += (int)h[1][0] + (int)h[1][2] + (int)h[1][7];                 /* detach, upload, class-type "7 or more": none expected */
        for (int b = 2; b < 8; b++) forbidden += (int)h[2][b];                   /* any vendor-class call other than the handshake (breq 0) or a command-execute (breq 1) */
        for (int b = 0; b < 8; b++) forbidden += (int)h[3][b];                   /* reserved request-type bits: never expected */
        int c1 = hv_libusb_ctrl_count(), total = c1 - g_ctrl0, lost = 0;
        for (int k = g_ctrl0; k < c1; k++) { HvCtrlRecord r; if (!hv_libusb_ctrl_get(k, &r)) lost++; }
        int d_sub = hv_libusb_data_transfer_count() - g_data0;
        int d_ok = hv_libusb_data_transfer_ok_count() - g_data_ok0;
        int d_bad = hv_libusb_data_transfer_bad_count() - g_data_bad0;
        row(out, "device_requests", !forbidden && dnload >= 1 && d_bad == 0,
            "%d control request(s) total (%d no longer in the detail ring -- the ring only holds the last %d; the counts below come from the never-evicted histogram instead, so this does not affect them): "
            "%d DFU DNLOAD (must be >=1: components were personalized and sent all the way through RecoveryOS entry), %d DFU GET_STATUS, %d DFU GET_STATE, %d DFU CLRSTATUS/ABORT "
            "(libirecovery's own per-component state recovery, conditional -- not forbidden), %d Recovery-mode vendor handshake (libirecovery's one call before each bulk send), "
            "%d Recovery-mode vendor command_execute (bRequest 1 -- \"ticket\"/\"devicetree\"/\"ramdisk\"/\"go\"/... per libirecovery's irecv_send_command()/irecv_send_command_breq(); which one isn't recorded, only the count), "
            "%d standard/enumeration request(s), %d forbidden (detach/upload/any other vendor call/reserved type -- none of these are issued by this call path); "
            "%d bulk/interrupt transfer(s) submitted (%d completed OK, %llu bytes, %d completed with a non-success status -- must be 0; a bulk transfer is expected here, so only a failed one counts against the row)",
            total, lost, HV_CTRL_LOG_MAX, dnload, getstatus, getstate, reset, handshake, command_execute, enumreq, forbidden, d_sub, d_ok, (unsigned long long)(hv_libusb_data_transfer_bytes() - g_data_bytes0), d_bad);
    }
    FileId d1, a1;
    file_id(disk, &d1); file_id(aux, &a1);
    char h0[20], h1[20];
    hex(g_disk0.acc, h0); hex(d1.acc, h1);
    {
        int same = d1.ok && d1.size == g_disk0.size && d1.mtime_ns == g_disk0.mtime_ns && !memcmp(d1.acc, g_disk0.acc, 32) && d1.mode == g_disk0.mode;
        if (same && d1.mode == 2)
            row_meta(out, "disk_identity", "metadata_only: size %llu and modification time unchanged, but the disk holds more than the content-check limit of data, so its CONTENT was not compared (not a content check)", (unsigned long long)d1.size);
        else
            row(out, "disk_identity", same, "hash_verified: size %llu -> %llu, modification time %s, content (data extents, sparse holes skipped) %s -> %s (RecoveryOS entry never touches the virtual disk; it runs off its own ramdisk)", (unsigned long long)g_disk0.size, (unsigned long long)d1.size, d1.mtime_ns == g_disk0.mtime_ns ? "same" : "CHANGED", h0, h1);
    }
    hex(g_aux0.acc, h0); hex(a1.acc, h1);
    row(out, "aux_identity", a1.ok && a1.size == g_aux0.size && a1.mtime_ns == g_aux0.mtime_ns && !memcmp(a1.acc, g_aux0.acc, 32) && a1.mode == g_aux0.mode && a1.mode == 1,
        "%s: size %llu -> %llu, modification time %s, content %s -> %s (the auxiliary storage must be content-verified; metadata only is a failure)", a1.mode == 1 ? "hash_verified" : "metadata_only", (unsigned long long)g_aux0.size, (unsigned long long)a1.size, a1.mtime_ns == g_aux0.mtime_ns ? "same" : "CHANGED", h0, h1);
    {
        int shown, wh, se;
        shown = __atomic_load_n(&g_lines_shown, __ATOMIC_SEQ_CST);
        wh = __atomic_load_n(&g_lines_withheld, __ATOMIC_SEQ_CST);
        se = __atomic_load_n(&g_lines_sensitive, __ATOMIC_SEQ_CST);
        row(out, "tool_log", 1, "the tool's log is filtered: %d line(s) shown (only lines that begin like a known-harmless one and show no ECID/nonce/ticket pattern), %d withheld (%d of those matched an ECID, nonce or ticket pattern: blocked, which is the filter working; counts only, never the text)", shown, wh, se);
    }
    {
        ThSnap t1;
        th_snap(&t1);
        char grown[160] = "";
        int grew = 0;
        for (int i = 0; i < t1.n; i++)
            if (th_count(&t1, t1.name[i]) > th_count(&g_th0, t1.name[i]) && !strstr(grown, t1.name[i])) { grew++; strncat(grown, t1.name[i], sizeof grown - strlen(grown) - 2); strncat(grown, " ", sizeof grown - strlen(grown) - 1); }
        row(out, "threads", t1.n >= 0 && g_th0.n >= 0 && !grew && t1.n < MAXTH && g_th0.n < MAXTH, "%d before the run (guest on the bus), %d after; %s%s", g_th0.n, t1.n, grew ? "MORE threads than before for: " : "no name has more threads than before", grown);
    }
    {
        WorkSnap w1;
        wf_snap(g_workdir, &w1);
        int added = 0, removed = 0, changed = 0, ticketish = 0, snap_ok = w1.n >= 0 && g_wf0.n >= 0;
        if (snap_ok) {
            for (int i = 0; i < w1.n; i++) {
                int j = wf_find(&g_wf0, w1.name[i]);
                if (j < 0) { added++; if (looks_like_ticket_name(w1.name[i])) ticketish++; }
                else if (w1.size[i] != g_wf0.size[j] || w1.mtime[i] != g_wf0.mtime[j]) changed++;
            }
            for (int i = 0; i < g_wf0.n; i++) if (wf_find(&w1, g_wf0.name[i]) < 0) removed++;
        }
        row(out, "local_writes", snap_ok && added == 0 && removed == 0 && changed == 0, "work directory top-level entries: %d new, %d removed, %d changed (none expected; stop C never reaches a write)", added, removed, changed);
        row(out, "ticket_files", snap_ok && ticketish == 0, "%d new entry(ies) named like a signing ticket (shsh/ticket/img4/apimg); none expected (hv_no_ticket_files, the session's default)", ticketish);
    }
    (void)redact_name;
    (void)g_tss_enabled;
    (void)g_macos_variant;
    if (g_failed == 0 && g_meta == 0)
        fprintf(out, "install g4 RESULT: g4_passed (engine return %d, session report %d, %d TSS request attempt(s), device confirmed recovery->restore with RecoveryOS running off its own ramdisk, disk and aux content verified, 0 failed row(s)). This is NOT a restore: it stops right after RecoveryOS entry by design.\n", g_rec_engine_rc, g_rec_report, att);
    else if (g_failed == 0 && g_meta > 0)
        fprintf(out, "install g4 RESULT: g4_passed_metadata_only (engine return %d, session report %d, %d TSS request attempt(s), 0 failed row(s), but the disk was too large for a content check and was compared by size and modification time only). This is NOT a restore: it stops right after RecoveryOS entry by design.\n", g_rec_engine_rc, g_rec_report, att);
    else
        fprintf(out, "install g4 RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
    return g_failed;
}
