/* G1 test build only: see install_g1.h. */
#define _GNU_SOURCE 1
#include "install_g1.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "idevicerestore/src/common.h"
#include "idevicerestore/src/idevicerestore.h"
#include "idevicerestore/src/log.h"
#include "install.h"
#include "net_guard.h"
#include "usb_host.h"

extern int hv_no_ticket_files;   /* third_party/idevicerestore/src/idevicerestore.c */
int hv_curl_attempt_count(void);
const char *tss_last_diag(void);

/* ---- what the restore thread reports (host/install.c) ---- */
static volatile int g_rec_done, g_rec_engine_rc = -12345, g_rec_closed, g_rec_cleanup = -1, g_rec_report = -1;
static char g_url[256];
static int g_port, g_tickets_on, g_hooked, g_bypass;
void hv_g1_set_log_bypass(int on) { g_bypass = on; }
void hv_g1_set_server(const char *url, int port) { snprintf(g_url, sizeof g_url, "%s", url ? url : ""); g_port = port; }
void hv_g1_test_allow_tickets(int on) { g_tickets_on = on; }

int hv_g1_extra_flags(struct idevicerestore_client_t *c) {
    if (g_url[0]) c->tss_url = strdup(g_url);   /* the client frees it; without it libtatsu would use its built-in list, and nothing in G1 may ask for that */
    if (g_port > 0) hv_net_guard_test_allow_loopback((uint16_t)g_port);   /* exactly 127.0.0.1:port, HTTP purpose; every other loopback port, address and purpose stays refused */
    if (g_tickets_on) hv_no_ticket_files = 0;   /* control only */
    g_hooked = 1;
    return FLAG_SHSHONLY;
}
void hv_g1_record(int engine_rc, int closed, int cleanup_mask, int final_report) {
    g_rec_engine_rc = engine_rc;
    g_rec_closed = closed;
    g_rec_cleanup = cleanup_mask;
    g_rec_report = final_report;
    __atomic_store_n(&g_rec_done, 1, __ATOMIC_SEQ_CST);
}

/* ---- the tool's log: a whitelist ----
   Only these lines (by their beginning) are printed. Everything else is counted and dropped: the tool's log holds the device nonce (a hex dump) and, in a real run,
   the ECID, and a redaction of what we know to look for is not enough. The test program can switch the filter off for its control (hv_g1_set_log_bypass); the table then fails its tool_log row. */
static const char *const k_ok[] = {"Found device in DFU mode", "Identified device as", "Device Product", "Extracting BuildManifest", "IPSW Product", "Device supports Image4",
                                   "Variant:", "This restore will erase", "Performing macOS restore", "Checking IPSW for required", "All required components",
                                   "Received SHSH blobs", "SHSH not saved", NULL};
static volatile int g_lines_shown, g_lines_withheld, g_lines_sensitive, g_bypassed_lines;
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
/* The one place a line of the tool's log is decided: the real log hook (log_print) and the test entry (hv_g1_filter_line) both call it. 1: printed, 0: withheld. */
static int filter_line(const char *line) {
    if (g_bypass) { fputs(line, stdout); __atomic_add_fetch(&g_bypassed_lines, 1, __ATOMIC_SEQ_CST); return 1; }   /* a control of the test program, never a default */
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
int hv_g1_filter_line(const char *line) { return filter_line(line); }
void hv_g1_log_counts(int *shown, int *withheld, int *sensitive, int *bypassed) {
    *shown = __atomic_load_n(&g_lines_shown, __ATOMIC_SEQ_CST);
    *withheld = __atomic_load_n(&g_lines_withheld, __ATOMIC_SEQ_CST);
    *sensitive = __atomic_load_n(&g_lines_sensitive, __ATOMIC_SEQ_CST);
    *bypassed = __atomic_load_n(&g_bypassed_lines, __ATOMIC_SEQ_CST);
}

/* ---- the work directory: names, sizes and times before and after ---- */
#define MAXF 512
typedef struct { int n, overflow, unreadable; char path[MAXF][200]; long long size[MAXF], mtime[MAXF]; } Snap;
static Snap g_s0;
static void snap_dir(const char *root, const char *rel, int depth, Snap *s) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    DIR *d = opendir(rel[0] ? full : root);
    if (!d) { s->unreadable++; return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r2[400], f2[1400];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(f2, sizeof f2, "%s/%s", root, r2);
        struct stat st;
        if (lstat(f2, &st) != 0) { s->unreadable++; continue; }
        if (s->n >= MAXF) { s->overflow++; continue; }
        snprintf(s->path[s->n], sizeof s->path[0], "%s", r2);
        s->size[s->n] = st.st_size;
        s->mtime[s->n] = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        s->n++;
        if (S_ISDIR(st.st_mode) && depth < 4) snap_dir(root, r2, depth + 1, s);
    }
    closedir(d);
}
static void snap(const char *root, Snap *s) { memset(s, 0, sizeof *s); snap_dir(root, "", 0, s); }
static int snap_find(const Snap *s, const char *p) { for (int i = 0; i < s->n; i++) if (!strcmp(s->path[i], p)) return i; return -1; }
static int ticketish(const char *p) {
    char l[200]; size_t i = 0;
    for (; p[i] && i < sizeof l - 1; i++) l[i] = (char)tolower((unsigned char)p[i]);
    l[i] = 0;
    return strstr(l, "shsh") || strstr(l, "ticket") || strstr(l, "img4") || strstr(l, "apimg");
}

/* ---- threads, like host/install_g0.c ---- */
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

static char g_ecid_dec[32], g_ecid_hex[32], g_ecid_HEX[32];
/* a file name can carry the ECID (the tool's own ticket file does): names are printed with it replaced */
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
static int g_began, g_started, g_online_ok, g_att0, g_ctrl0, g_data0, g_waited = -2, g_mux_port;
static HvNetStats g_ns0;

int hv_g1_begin(const char *workdir, uint64_t ecid) {
    snprintf(g_ecid_dec, sizeof g_ecid_dec, "%llu", (unsigned long long)ecid);
    snprintf(g_ecid_hex, sizeof g_ecid_hex, "%llx", (unsigned long long)ecid);
    snprintf(g_ecid_HEX, sizeof g_ecid_HEX, "%llX", (unsigned long long)ecid);
    struct stat sb;
    if (stat(workdir, &sb) != 0 || !S_ISDIR(sb.st_mode)) { printf("install g1: refused -- the work directory must exist\n"); return -1; }
    snap(workdir, &g_s0);
    if (g_s0.overflow || g_s0.unreadable) { printf("install g1: refused -- the work directory could not be listed completely\n"); return -1; }
    logger_set_logfile("NONE");
    logger_set_print_func(log_print);
    g_att0 = hv_curl_attempt_count();
    g_began = 1;
    return 0;
}

int hv_g1_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid) {
    if (!g_began || !usb) return -1;
    if (hv_usb_wait_online(usb, 60000) < 0) { printf("install g1: the guest did not come on the USB bus within 60 s; the restore was not started\n"); return -1; }
    g_online_ok = 1;
    g_ctrl0 = hv_libusb_ctrl_count();
    g_data0 = hv_libusb_data_transfer_count();
    th_snap(&g_th0);
    hv_net_guard_stats(&g_ns0);
    g_mux_port = 27100 + (int)(getpid() % 800);   /* the same rule as host/install.c */
    if (hv_install_start(usb, ipsw, workdir, ecid, NULL, NULL) != 0) { printf("install g1: hv_install_start refused to start\n"); return -1; }
    g_started = 1;
    return 0;
}

int hv_g1_wait(int cap_ms) {
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

/* ---- libtatsu's diagnosis of the last request, parsed and rebuilt from numbers and a short class name (the raw string is never printed) ---- */
typedef struct { int kind, attempts, http, status, curl; char cls[28]; } Diag;
static int cls_ok(const char *c) { size_t n = strlen(c); if (!n || n >= 26) return 0; for (; *c; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0; return 1; }
static int parse_diag(const char *d, Diag *o) {
    memset(o, 0, sizeof *o);
    o->attempts = o->http = o->status = o->curl = -1;
    if (!d[0]) { o->kind = HV_G1D_EMPTY; return 1; }
    int a, h, v, cc, n;
    char cl[40];
    /* sscanf's count does not say that the literal text after the last conversion matched: each pattern ends in %n, which stays 0 unless the whole pattern matched, and the rest of the string must be exhausted */
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / TRANSPORT: curl %d %39s / MESSAGE_RAW: suppressed%n", &a, &cc, cl, &n) == 3 && n > 0 && !d[n] && cls_ok(cl)) { o->kind = HV_G1D_TRANSPORT; o->attempts = a; o->curl = cc; snprintf(o->cls, sizeof o->cls, "%s", cl); return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / TSS_STATUS: 0 / MESSAGE_CLASS: SUCCESS / MESSAGE_RAW: suppressed%n", &a, &h, &n) == 2 && n > 0 && !d[n]) { o->kind = HV_G1D_SUCCESS; o->attempts = a; o->http = h; o->status = 0; snprintf(o->cls, sizeof o->cls, "SUCCESS"); return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / NOT_A_TSS_REPLY / MESSAGE_RAW: suppressed%n", &a, &h, &n) == 2 && n > 0 && !d[n]) { o->kind = HV_G1D_NOT_REPLY; o->attempts = a; o->http = h; return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / SUCCESS_BUT_NOT_A_PLIST / MESSAGE_RAW: suppressed%n", &a, &n) == 1 && n > 0 && !d[n]) { o->kind = HV_G1D_NOT_PLIST; o->attempts = a; return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / TSS_STATUS: %d / MESSAGE_CLASS: %39s / MESSAGE_RAW: suppressed%n", &a, &h, &v, cl, &n) == 4 && n > 0 && !d[n] && cls_ok(cl)) { o->kind = HV_G1D_STATUS; o->attempts = a; o->http = h; o->status = v; snprintf(o->cls, sizeof o->cls, "%s", cl); return 1; }
    n = 0; if (sscanf(d, "TSS_ATTEMPTS: %d / HTTP: %d / TSS_STATUS: OTHER / MESSAGE_CLASS: %39s / MESSAGE_RAW: suppressed%n", &a, &h, cl, &n) == 3 && n > 0 && !d[n] && cls_ok(cl)) { o->kind = HV_G1D_STATUS; o->attempts = a; o->http = h; o->status = -2; snprintf(o->cls, sizeof o->cls, "%s", cl); return 1; }
    return 0;   /* an unknown format is a failure, and is not printed */
}
static const char *diag_name(int k) { static const char *n[] = {"unchecked", "success", "status", "not_a_tss_reply", "transport", "empty", "success_but_not_a_plist"}; return k >= 0 && k <= 6 ? n[k] : "?"; }

/* ---- the table ---- */
static int g_failed;
static void row(FILE *out, const char *name, int ok, const char *fmt, ...) {
    char d[900];
    va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap);
    fprintf(out, "install g1: %-16s %-6s %s\n", name, ok ? "ok" : "FAILED", d);
    if (!ok) g_failed++;
}

int hv_g1_finish(FILE *out, const char *workdir, const HvG1Expect *x) {
    g_failed = 0;
    int ended = g_started && g_waited != -1 && hv_install_result() != 1;
    usleep(500000);   /* the tool's event thread needs a moment to wind down */
    fprintf(out, "install g1: the install entry point (host/install.c) and the real restore tool ran up to the signing requests against a synthetic server on 127.0.0.1; this is NOT a restore: nothing was sent to the device, extracted or installed\n");
    row(out, "started", g_started && g_hooked, g_started ? (g_hooked ? "hv_install_start accepted the run (guest on the bus: %d); the G1 hook ran on the restore thread" : "started, but the G1 hook never ran (guest on the bus: %d)") : "the run did not start (guest on the bus: %d)", g_online_ok);
    row(out, "ended", ended && g_waited == 1, "%s", g_waited == 1 ? "the restore thread ended by itself" : g_waited == 0 ? "it did NOT end by itself: a cancel was requested and then it ended -- the time limit was hit, so this is a failure" : g_waited == -1 ? "the restore thread did NOT end, not even after the cancel request; nothing was released or closed under it" : "not started");
    if (g_waited == -1 || !g_started) {
        fprintf(out, "install g1 RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
        return g_failed;
    }
    int rec = __atomic_load_n(&g_rec_done, __ATOMIC_SEQ_CST);
    int want_rep = x->engine_zero ? 1 : 2;
    row(out, "engine_rc", rec && ((g_rec_engine_rc == 0) == (x->engine_zero != 0)), "the tool returned %d; this scenario expects %s", g_rec_engine_rc, x->engine_zero ? "0 (all three requests answered, then the tool's own stop)" : "a non-zero return");
    row(out, "session", rec && g_rec_closed && g_rec_cleanup == 0 && g_rec_report == want_rep, "closed %d, cleanup_mask 0x%x (0 = the client was released and the private usbmuxd stopped and joined), final report %d (expected %d: 1 success for a return of 0, 2 restore failed for any other)", g_rec_closed, g_rec_cleanup, g_rec_report, want_rep);
    row(out, "usbmuxd", !mux_listening(g_mux_port), "the private usbmuxd's loopback socket %s", mux_listening(g_mux_port) ? "STILL LISTENS" : "no longer answers");
    int att = hv_curl_attempt_count() - g_att0;
    row(out, "tss_requests", att >= x->tss_min && att <= x->tss_max, "the curl client made %d request attempt(s) (expected %d to %d; retries count); the signing URL was the forced test URL", att, x->tss_min, x->tss_max);
    {
        Diag dg;
        int parsed = parse_diag(tss_last_diag(), &dg);   /* read once, after the restore thread ended: nothing can overwrite it (it only describes the LAST request) */
        int ok = parsed && (x->diag_kind == HV_G1D_UNCHECKED || (dg.kind == x->diag_kind && (x->diag_val < 0 || (dg.kind == HV_G1D_STATUS ? dg.status : dg.curl) == x->diag_val)));
        row(out, "tss_diag", ok, "libtatsu's diagnosis of the last request: %s; kind=%s attempts=%d http=%d tss_status=%d curl_code=%d class=%s; expected kind=%s value=%d%s (it describes the last request only; the earlier ones are covered by the server's request count and the tool's return)",
            parsed ? "parsed" : "UNKNOWN FORMAT (not printed)", parsed ? diag_name(dg.kind) : "?", dg.attempts, dg.http, dg.status, dg.curl, dg.cls[0] ? dg.cls : "-", diag_name(x->diag_kind), x->diag_val, x->diag_kind == HV_G1D_UNCHECKED ? " (observation only)" : "");
    }
    HvNetStats ns; hv_net_guard_stats(&ns);
    int den = ns.denied - g_ns0.denied, allowed = ns.allowed - g_ns0.allowed;
    row(out, "connections", den >= x->denied_min && den <= x->denied_max && ns.url_attempts == 0, "connection guard: %d attempt(s), %d allowed (the private usbmuxd listener and the one approved test-server port), %d refused (expected %d to %d), URLAsset %d; a refusal that is not the scenario means the tool went somewhere it must not (a direct connect() elsewhere in the process is not covered)", ns.attempts - g_ns0.attempts, allowed, den, x->denied_min, x->denied_max, ns.url_attempts);
    {
        int c1 = hv_libusb_ctrl_count(), d1 = hv_libusb_data_transfer_count();
        int forbidden = 0, lost = 0, total = c1 - g_ctrl0;
        for (int k = g_ctrl0; k < c1; k++) {
            HvCtrlRecord r;
            if (!hv_libusb_ctrl_get(k, &r)) { lost++; continue; }
            uint8_t type = (r.bmRequestType >> 5) & 3;
            if (type == 2) forbidden++;
            else if (type == 1 && (r.bRequest == 0 || r.bRequest == 1 || r.bRequest == 2 || r.bRequest == 4 || r.bRequest == 6)) forbidden++;
        }
        row(out, "device_requests", !forbidden && !lost && d1 == g_data0, "%d control request(s), %d forbidden (download/upload/detach/abort/clear-status/vendor), %d no longer in the log, %d bulk/interrupt transfer(s) (must be 0); counted at the libusb stand-in", total, forbidden, lost, d1 - g_data0);
    }
    {
        Snap s1;
        snap(workdir, &s1);
        int nw = 0, rm = 0, ch = 0, tick = 0;
        char names[300] = "";
        for (int i = 0; i < s1.n; i++) {
            int j = snap_find(&g_s0, s1.path[i]);
            int isnew = j < 0, ischg = j >= 0 && (g_s0.size[j] != s1.size[i] || g_s0.mtime[j] != s1.mtime[i]);
            if (isnew) nw++; else if (ischg) ch++;
            if ((isnew || ischg) && ticketish(s1.path[i])) tick++;
            if ((isnew || ischg) && strlen(names) < sizeof names - 210) { strcat(names, " "); strcat(names, s1.path[i]); }
        }
        for (int j = 0; j < g_s0.n; j++) if (snap_find(&s1, g_s0.path[j]) < 0) rm++;
        redact_name(names);
        row(out, "local_writes", s1.overflow == 0 && s1.unreadable == 0 && nw == 0 && rm == 0 && ch == 0, "work directory: %d entries before, %d after; %d new, %d removed, %d changed%s%s%s", g_s0.n, s1.n, nw, rm, ch, s1.overflow || s1.unreadable ? " (listing incomplete)" : "", names[0] ? ":" : "", names);
        row(out, "ticket_files", tick == 0, "%d new or changed entr%s whose name looks like a signing ticket (shsh / ticket / img4); ticket files are disabled by the session (hv_no_ticket_files)", tick, tick == 1 ? "y" : "ies");
    }
    {
        int shown, wh, se, by;
        hv_g1_log_counts(&shown, &wh, &se, &by);
        row(out, "tool_log", !g_bypass && by == 0, "%s: %d line(s) shown (only lines that begin like a known-harmless one and show no ECID/nonce/ticket pattern), %d withheld (%d of those matched an ECID, nonce or ticket pattern: blocked, which is the filter working; counts only, never the text), %d printed unfiltered; the filter itself is tested on boundary inputs by the scenario \"filter\"",
            g_bypass ? "the filter was SWITCHED OFF (a control; never allowed in a real G1 run)" : "the tool's log is filtered", shown, wh, se, by);
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
    if (g_failed == 0) fprintf(out, "install g1 RESULT: g1_passed (engine return %d as expected, session report %d, %d request attempt(s), %d refused connection(s) as expected, 0 failed row(s)). Scope: synthetic replies, only the work directory was watched for writes, no disk or aux took part. This is NOT a restore.\n", g_rec_engine_rc, g_rec_report, att, den);
    else fprintf(out, "install g1 RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
    return g_failed;
}
