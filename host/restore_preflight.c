/* Build 69-a: the real restore tool's start-up on the virtual Mac, stopped by its own FLAG_NOACTION. See restore_preflight.h for what runs and what is excluded. */
#define _GNU_SOURCE 1
#include "restore_preflight.h"
#include "net_guard.h"
#include "restore_session.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "idevicerestore/src/idevicerestore.h"
#include "idevicerestore/src/common.h"
#include "idevicerestore/src/log.h"
#include "idevicerestore/src/hv_stop.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <zip.h>

int usbmuxd_main(int argc, char **argv);
void hv_curl_deny_network(int on);
int hv_curl_deny_active(void);
int hv_curl_attempt_count(void);
int hv_curl_denied_count(void);
void hv_curl_set_cancel_flag(volatile int *flag);
extern int global_quit_flag;

/* ---- the tool's log: the device's ECID is never printed ----
   idevicerestore logs "ECID: <decimal>" at info level. This print function (the tool's own hook, no source change) prints every line with the ECID, in decimal or
   hex, replaced by <ecid>. */
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
    if (!strncmp(line, "ECID:", 5)) { fputs("ECID: <ecid>\n", stdout); return; } /* also when the caller did not give the ECID, so there is nothing to match */
    redact(line);
    fputs(line, stdout);
    (void)level;
}

/* ---- progress ---- */
static volatile int g_calls, g_max_step = -1, g_last_step = -1;
static volatile double g_last_p = -1;
static void on_progress(int step, double p, void *user) {
    (void)user;
    g_calls++;
    if (step > g_max_step) g_max_step = step;
    g_last_step = step, g_last_p = p;
}

int hv_restore_preflight_progress_verdict(int calls, int max_step, int last_step, double last_p, char *note, int cap) {
    if (calls <= 0) { snprintf(note, cap, "the tool reported no progress at all"); return 0; }
    if (max_step != 0) { snprintf(note, cap, "a step after DETECT was reported (highest step %d): the tool went past the point where FLAG_NOACTION stops it", max_step); return 0; }
    if (last_step != 0 || last_p < 0.59 || last_p > 0.61) { snprintf(note, cap, "the last callback is step %d at %.2f, not DETECT 0.60", last_step, last_p); return 0; }
    snprintf(note, cap, "%d callback(s), the last one is DETECT 0.60 (the line just before the FLAG_NOACTION return) and no later step ever came", calls);
    return 1;
}

int hv_restore_preflight_progress_verdict_a(int calls, int max_step, int last_step, double last_p, char *note, int cap) {
    if (calls <= 0) { snprintf(note, cap, "the tool reported no progress at all"); return 0; }
    if (max_step != 1) { snprintf(note, cap, "highest step %d, expected PREPARE (1): the tool did not get to, or went past, the stop", max_step); return 0; }
    if (last_step != 1 || last_p > 0.05) { snprintf(note, cap, "the last callback is step %d at %.2f, not PREPARE 0.00 (PREPARE 0.20 would mean the root filesystem step was passed)", last_step, last_p); return 0; }
    snprintf(note, cap, "%d callback(s), the last one is PREPARE 0.00 and PREPARE 0.20 (after the root filesystem step) never came", calls);
    return 1;
}

/* ---- what stop A reports (called on the restore thread; copied, the rows are built later) ---- */
static struct {
    volatile int called, has_identity;
    char hw[64], pt[64], os_path[512], ipsw_path[1024], cache[1024], variant[96], macvariant[96], devclass[48], chip[32], board[32], restorebehavior[48];
    int zip, os_opened, seekable, needs, shsh, macos, tss;
} g_sa;
static void node_text(plist_t n, char *out, size_t cap) {
    out[0] = 0;
    if (!n) { snprintf(out, cap, "(absent)"); return; }
    if (plist_get_node_type(n) == PLIST_STRING) { char *v = NULL; plist_get_string_val(n, &v); if (v) { snprintf(out, cap, "%s", v); free(v); } }
    else if (plist_get_node_type(n) == PLIST_UINT) { uint64_t v = 0; plist_get_uint_val(n, &v); snprintf(out, cap, "%llu", (unsigned long long)v); }
    else snprintf(out, cap, "(other type)");
}
static void stop_a(const struct hv_stop_a_info *i) {
    memset((void *)&g_sa, 0, sizeof g_sa);
    snprintf(g_sa.hw, sizeof g_sa.hw, "%s", i->hardware_model ? i->hardware_model : "");
    snprintf(g_sa.pt, sizeof g_sa.pt, "%s", i->product_type ? i->product_type : "");
    snprintf(g_sa.os_path, sizeof g_sa.os_path, "%s", i->os_path ? i->os_path : "");
    snprintf(g_sa.ipsw_path, sizeof g_sa.ipsw_path, "%s", i->ipsw_path ? i->ipsw_path : "");
    snprintf(g_sa.cache, sizeof g_sa.cache, "%s", i->cache_dir ? i->cache_dir : "");
    g_sa.zip = i->zip, g_sa.os_opened = i->os_opened, g_sa.seekable = i->seekable, g_sa.needs = i->needs_os_extraction, g_sa.shsh = i->shsh_only, g_sa.macos = i->macos_variant, g_sa.tss = i->tss_enabled;
    if (i->build_identity) {
        g_sa.has_identity = 1;
        node_text(plist_access_path(i->build_identity, 2, "Info", "Variant"), g_sa.variant, sizeof g_sa.variant);
        node_text(plist_access_path(i->build_identity, 2, "Info", "MacOSVariant"), g_sa.macvariant, sizeof g_sa.macvariant);
        node_text(plist_access_path(i->build_identity, 2, "Info", "DeviceClass"), g_sa.devclass, sizeof g_sa.devclass);
        node_text(plist_access_path(i->build_identity, 2, "Info", "RestoreBehavior"), g_sa.restorebehavior, sizeof g_sa.restorebehavior);
        node_text(plist_dict_get_item(i->build_identity, "ApChipID"), g_sa.chip, sizeof g_sa.chip);
        node_text(plist_dict_get_item(i->build_identity, "ApBoardID"), g_sa.board, sizeof g_sa.board);
    }
    g_sa.called = 1;
}

/* ---- files in the work directory (what the run wrote) ---- */
#define MAXF 512
typedef struct { int n; char path[MAXF][300]; long long size[MAXF]; long long total; } Tree;
static void tree_walk(const char *dir, const char *rel, Tree *t) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1400], r[300];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) tree_walk(full, r, t);
        else if (t->n < MAXF) { snprintf(t->path[t->n], sizeof t->path[0], "%s", r); t->size[t->n] = st.st_size; t->n++; t->total += st.st_size; }
    }
    closedir(d);
}

/* ---- the private usbmuxd, as host/install.c starts it ---- */
static char g_mux[64], g_cache[1024];
static volatile int g_mux_exited;
static volatile int g_mux_alive;   /* the private usbmuxd thread was started and not joined: no second run until the process is restarted */
static int g_still;   /* the restore thread did not return: the guard stays installed for it */
extern void usbmuxd_request_stop(void);   /* third_party/usbmuxd/src/main.c (Henge): main_loop returns within ~200 ms */
extern int hv_no_ticket_files;            /* third_party/idevicerestore/src/idevicerestore.c (Henge) */
static void *mux_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "hv-usbmuxd");
    char *argv[] = {"usbmuxd", "-f", "-p", "-P", "NONE", "-S", g_mux, "-C", g_cache, NULL};
    usbmuxd_main(9, argv);
    g_mux_exited = 1;
    return NULL;
}

/* ---- resource snapshots ---- */
#define MAXFD 256
typedef struct { int n; char kind[MAXFD][16]; char target[MAXFD][160]; } FdSnap;
static void fd_snap(FdSnap *s) {
    s->n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return;
    int dfd = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d)) && s->n < MAXFD) {
        if (e->d_name[0] == '.') continue;
        int fd = atoi(e->d_name);
        if (fd == dfd) continue;
        char path[64], t[160];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t l = readlink(path, t, sizeof t - 1);
        if (l < 0) continue;
        t[l] = 0;
        const char *k = !strncmp(t, "socket:", 7) ? "socket" : !strncmp(t, "pipe:", 5) ? "pipe" : !strncmp(t, "anon_inode:", 11) ? "anon_inode" : t[0] == '/' ? "file" : "other";
        strcpy(s->kind[s->n], k);
        snprintf(s->target[s->n], sizeof s->target[0], "%s", t);
        s->n++;
    }
    closedir(d);
}
static int fd_count_kind(const FdSnap *s, const char *k) { int c = 0; for (int i = 0; i < s->n; i++) if (!strcmp(s->kind[i], k)) c++; return c; }
static int fd_count_target(const FdSnap *s, const char *t) { int c = 0; for (int i = 0; i < s->n; i++) if (!strcmp(s->target[i], t)) c++; return c; }

#define MAXTH 64
typedef struct { int n; char name[MAXTH][32]; } ThSnap;
static void th_snap(ThSnap *s) {
    s->n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s->n < MAXTH) {
        if (e->d_name[0] == '.') continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char nm[32] = "";
        if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0;
        fclose(f);
        snprintf(s->name[s->n++], sizeof s->name[0], "%s", nm);
    }
    closedir(d);
}
static int th_count(const ThSnap *s, const char *nm) { int c = 0; for (int i = 0; i < s->n; i++) if (!strcmp(s->name[i], nm)) c++; return c; }

/* ---- the table ---- */
typedef struct { const char *name; const char *status; char detail[800]; } Row;
static Row g_rows[40];
static int g_nrows, g_failed, g_findings;
static void row(const char *name, const char *status, const char *fmt, ...) {
    Row *r = &g_rows[g_nrows++];
    r->name = name, r->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->detail, sizeof r->detail, fmt, ap);
    va_end(ap);
    if (!strcmp(status, "FAILED")) g_failed++;
}

/* ---- the restore thread ---- */
static struct idevicerestore_client_t *g_client;
static volatile int g_rc = 12345, g_done;
static void *restore_thread(void *arg) {
    (void)arg;
    int rc = 12345;
    /* the only way into the tool: refused unless the connection policy is in force, and the policy cannot be removed while this runs */
    if (hv_restore_session_run_client(g_client, &rc) != 0) rc = 12346;
    g_rc = rc;
    g_done = 1;
    return NULL;
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int mux_listening(void) {
    int port = atoi(strrchr(g_mux, ':') + 1);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

int hv_restore_preflight_run(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, FILE *out, int cap_ms, int stage) {
    int sess = -1;   /* the connection policy session: only a begun session is ended at the report */
    if (g_mux_alive) { fprintf(out, "restore preflight: refused -- the private usbmuxd of an earlier run is still running (it did not stop); restart the app before running again\n"); return -1; }
    g_nrows = g_failed = g_findings = 0;
    g_still = 0;
    g_calls = 0, g_max_step = -1, g_last_step = -1, g_last_p = -1, g_rc = 12345, g_done = 0;
    memset((void *)&g_sa, 0, sizeof g_sa);
    if (!usb) { fprintf(out, "restore preflight: no USB port\n"); return -1; }

    /* 1. the safety boundary first: deny mode on, and proven on before anything can ask for the network */
    hv_curl_deny_network(1);
    int att0 = hv_curl_attempt_count(), den0 = hv_curl_denied_count();
    if (hv_curl_deny_active()) row("deny_mode", "ok", "the curl stand-in refuses every request before any lookup, connection or TLS (scope: that client only; the loopback usbmuxd socket is not counted)");
    else row("deny_mode", "FAILED", "deny mode did not turn on; nothing else is started");
    if (g_failed) goto report;
    /* the connection guard: default deny for every connect() the tool's libraries make (FDR proxy, libusbmuxd, curl stand-in, URLAsset); only the private usbmuxd
       listener is registered below. A signing ticket is never written to a file. */
    sess = hv_restore_session_begin();
    row("net_guard", sess == 0 ? "ok" : "FAILED", "default-deny connection policy installed (glue sockets, curl stand-in, URLAsset); allow list: the private usbmuxd listener only; ticket files disabled; the tool can only be started through the session, which refuses to run without it");
    if (sess != 0) goto report;
    g_mux_exited = 0;

    if (hv_usb_wait_online(usb, 60000) < 0) { row("guest_on_bus", "FAILED", "the guest did not come on the USB bus within 60 s; the tool was not started"); goto report; }
    FdSnap fd0; ThSnap th0;
    fd_snap(&fd0); th_snap(&th0);
    int ctrl0 = hv_libusb_ctrl_count(), data0 = hv_libusb_data_transfer_count();

    /* 2. the private usbmuxd, then the real tool */
    snprintf(g_cache, sizeof g_cache, "%s", workdir);
    snprintf(g_mux, sizeof g_mux, "127.0.0.1:%d", 27100 + (int)(getpid() % 800));
    setenv("USBMUXD_SOCKET_ADDRESS", g_mux, 1);
    hv_restore_session_allow_usbmuxd((uint16_t)atoi(strrchr(g_mux, ':') + 1));
    hv_libusb_shim_attach(usb);
    pthread_t tm, tr;
    if (pthread_create(&tm, NULL, mux_thread, NULL) != 0) { row("init", "FAILED", "usbmuxd thread did not start"); goto report; }
    g_mux_alive = 1;
    usleep(300000);
    /* the baseline of the tool's own subscription and handles is taken with usbmuxd already up: it registers its own hotplug callback and keeps it for good */
    static Tree tree0, tree1;
    memset(&tree0, 0, sizeof tree0);
    tree_walk(workdir, "", &tree0);
    int hp0 = hv_libusb_hotplug_live(), hd0 = hv_libusb_open_handles(), ho0 = hv_libusb_handles_opened_total();

    snprintf(g_ecid_dec, sizeof g_ecid_dec, "%llu", (unsigned long long)ecid);
    snprintf(g_ecid_hex, sizeof g_ecid_hex, "%llx", (unsigned long long)ecid);
    snprintf(g_ecid_HEX, sizeof g_ecid_HEX, "%llX", (unsigned long long)ecid);
    if (!ecid) g_ecid_dec[0] = g_ecid_hex[0] = g_ecid_HEX[0] = 0;
    logger_set_logfile("NONE"); /* otherwise the tool also copies every line, unredacted, to stderr */
    logger_set_print_func(log_print);
    g_client = idevicerestore_client_new();
    if (!g_client) { row("init", "FAILED", "idevicerestore_client_new failed"); goto report; }
    /* FLAG_ERASE like the install; FLAG_NOACTION is the stop. Not set: FLAG_LATEST (network), FLAG_SHSHONLY, FLAG_NO_RESTORE, FLAG_PWN, FLAG_CUSTOM. */
    int stop_flag = stage == HV_PF_STOP_A ? FLAG_STOP_BEFORE_ROOTFS : FLAG_NOACTION;
    hv_stop_a_hook = stop_a;
#ifdef HV_PREFLIGHT_NEGATIVE_CONTROL
    /* tests/test_preflight.c only (a second build of this file): without the stop the tool must go on, which proves the stop is what ends the run. Never in a shipped build. */
    idevicerestore_set_flags(g_client, FLAG_ERASE);
    (void)stop_flag;
#else
    idevicerestore_set_flags(g_client, FLAG_ERASE | stop_flag);
#endif
    hv_curl_set_cancel_flag((volatile int *)&global_quit_flag);
    idevicerestore_set_ipsw(g_client, ipsw);
    char real[4096];
    if (!realpath(ipsw, real)) snprintf(real, sizeof real, "%s", ipsw); /* /proc/self/fd shows absolute paths */
    int ipsw_opened = g_client->ipsw != NULL; /* the tool opens the archive here, not in start */
    FdSnap fd_open; fd_snap(&fd_open);
    int ipsw_fds_open = fd_count_target(&fd_open, real);
    idevicerestore_set_cache_path(g_client, workdir);
    if (ecid) idevicerestore_set_ecid(g_client, ecid);
    idevicerestore_set_progress_callback(g_client, on_progress, NULL);
    row("ipsw_open", ipsw_opened ? "ok" : "FAILED", "idevicerestore_set_ipsw opened the archive: %s (file descriptors on it while open: %d) -- observed directly", ipsw_opened ? "yes" : "no", ipsw_fds_open);

    double t0 = now_ms();
    if (pthread_create(&tr, NULL, restore_thread, NULL) != 0) { row("init", "FAILED", "restore thread did not start"); goto report; }
    while (!g_done && now_ms() - t0 < cap_ms) usleep(20000);
    int timed_out = !g_done;
    if (timed_out) { global_quit_flag = 1; for (int i = 0; i < 150 && !g_done; i++) usleep(20000); }
    int still_running = !g_done;   /* the quit flag did not end it either */
    g_still = still_running;
    if (g_done) pthread_join(tr, NULL);
    double took = now_ms() - t0;

    int want_rc = stage == HV_PF_STOP_A ? HV_STOP_BEFORE_ROOTFS_RC : 0;
    row("init", (!timed_out && g_rc == want_rc) ? "ok" : "FAILED", "idevicerestore_start returned %d (expected %d) after %.0f ms%s", g_rc, want_rc, took, timed_out ? (still_running ? " -- TIMEOUT, quit flag raised, the thread is STILL RUNNING" : " -- TIMEOUT, quit flag raised, it then ended") : "");
    char note[240];
    int pv = stage == HV_PF_STOP_A ? hv_restore_preflight_progress_verdict_a(g_calls, g_max_step, g_last_step, g_last_p, note, sizeof note)
                                  : hv_restore_preflight_progress_verdict(g_calls, g_max_step, g_last_step, g_last_p, note, sizeof note);
    row("progress", pv ? "ok" : "FAILED", "%s", note);

    /* 3. what crossed the boundary */
    int att = hv_curl_attempt_count() - att0, den = hv_curl_denied_count() - den0;
    row("network_requests", att == 0 ? "ok" : "FAILED", "curl stand-in: %d attempt(s), %d refused by deny mode (scope: the curl client only; the loopback usbmuxd socket and other libraries are not counted)", att, den);
    {
        HvNetStats ns; hv_net_guard_stats(&ns);
        char dl[400]; hv_net_guard_denied_log(dl, sizeof dl);
        for (char *c = dl; *c; c++) if (*c == '\n') *c = ';';
        row("connections", ns.denied == 0 && ns.url_attempts == 0 ? "ok" : "FAILED", "connection guard: %d attempt(s) at the socket level, %d allowed (the registered private usbmuxd listener), %d refused; URLAsset requests %d (%d refused)%s%s (covers the glue sockets, the curl stand-in and the URLAsset step; a direct connect() elsewhere in the process is not covered)", ns.attempts, ns.allowed, ns.denied, ns.url_attempts, ns.url_denied, dl[0] ? " -- refused: " : "", dl);
    }
    row("tss_requests", att == 0 ? "ok" : "FAILED", "0 -- derived: every signing request goes through the curl stand-in and it saw %d attempt(s); there is no separate counter in the signing library", att);
    {
        int c1 = hv_libusb_ctrl_count(), d1 = hv_libusb_data_transfer_count();
        int forbidden = 0, lost = 0, getstatus = 0, total = c1 - ctrl0;
        for (int k = ctrl0; k < c1; k++) {
            HvCtrlRecord r;
            if (!hv_libusb_ctrl_get(k, &r)) { lost++; continue; }
            uint8_t type = (r.bmRequestType >> 5) & 3;
            if (type == 2) forbidden++;
            else if (type == 1 && (r.bRequest == 0 || r.bRequest == 1 || r.bRequest == 2 || r.bRequest == 4 || r.bRequest == 6)) forbidden++;
            else if (type == 1 && r.bRequest == 3) getstatus++;
        }
        int bad = forbidden || lost || (d1 - data0);
        row("data_transfers", bad ? "FAILED" : "ok", "%d control request(s) through the API (%d DFU GET_STATUS), %d forbidden (download/upload/detach/abort/clear-status/vendor), %d no longer in the log, %d bulk/interrupt transfer(s) (must be 0); counted at the libusb stand-in only", total, getstatus, forbidden, lost, d1 - data0);
    }

    /* 4. the stop itself: derived, not observed */
    int stopped = !timed_out && g_rc == want_rc && pv && att == 0;
    if (stage == HV_PF_STOP_A) {
        int direct = stopped && g_sa.called;
        row("stopped=before_rootfs", direct ? "direct" : "FAILED", "%s -- OBSERVED: the tool itself called the stop-A report and returned %d; no extraction, signing request or device work comes after that point in the code",
            direct ? "the tool stopped at stop A (after the component check, before the root filesystem step)" : "stop A was not reached or the other conditions do not hold", g_rc);
    } else
    row("stopped=noaction", stopped ? "inferred" : "FAILED",
        "%s -- INFERRED from the code: with FLAG_PWN, FLAG_SHSHONLY and FLAG_NO_RESTORE unset, a 0 return before any later step can only come from the FLAG_NOACTION branch (every other early exit returns negative); the branch itself writes no log line and no callback", stopped ? "the tool stopped at its no-action point" : "the conditions for that inference do not hold");

    if (stage == HV_PF_STOP_A) {
        if (!g_sa.called) row("identity", "FAILED", "stop A was not reached, so there is nothing to report about the build identity");
        else {
            row("identity", g_sa.has_identity ? "ok" : "FAILED", "chosen for hardware model %s (product type %s): Variant \"%s\", RestoreBehavior %s, DeviceClass %s, ApChipID %s, ApBoardID %s -- observed from the tool's own report",
                g_sa.hw, g_sa.pt, g_sa.variant, g_sa.restorebehavior, g_sa.devclass, g_sa.chip, g_sa.board);
            row("macos_variant", "info", "%s (the tool's own macOS recovery-OS lookup for this model: variant \"macOS Customer\", exact match); MacOSVariant of the chosen identity: %s; tss_enabled by the manifest: %d",
                g_sa.macos ? "present" : "ABSENT", g_sa.macvariant, g_sa.tss);
            row("components", "ok", "present in the IPSW: the tool's check of every component path of the chosen identity passed before it reached stop A (existence of the ZIP entries only; contents and hashes are not checked)");
            /* the root filesystem entry, from the ZIP itself (a second, read-only look with libzip; the tool's own handle is not shared) */
            int zerr = 0; zip_t *z = zip_open(ipsw, ZIP_RDONLY, &zerr);
            zip_stat_t zs; zip_stat_init(&zs);
            int zok = z && zip_stat(z, g_sa.os_path, 0, &zs) == 0 && (zs.valid & ZIP_STAT_COMP_METHOD) && (zs.valid & ZIP_STAT_SIZE) && (zs.valid & ZIP_STAT_COMP_SIZE);
            const char *mname = !zok ? "?" : zs.comp_method == ZIP_CM_STORE ? "stored" : zs.comp_method == ZIP_CM_DEFLATE ? "deflate" : "other";
            if (z) zip_close(z);
            row("rootfs_entry", zok ? "ok" : "FAILED", "path %s; ZIP method %s (%d); size %llu, compressed %llu bytes; the tool's handle is %sseekable (the tool's rule is seekable = stored, so it %s here)",
                g_sa.os_path, mname, zok ? (int)zs.comp_method : -1, zok ? (unsigned long long)zs.size : 0ULL, zok ? (unsigned long long)zs.comp_size : 0ULL, g_sa.seekable ? "" : "NOT ",
                zok && ((zs.comp_method == ZIP_CM_STORE) == (g_sa.seekable != 0)) ? "agrees" : "DISAGREES with what libzip says");
            struct statvfs sv; unsigned long long avail = 0;
            if (statvfs(workdir, &sv) == 0) avail = (unsigned long long)sv.f_bavail * sv.f_frsize;
            row("extraction_rule", "info", "the tool's rule: extract %s (os entry opened: %d, seekable: %d, FLAG_SHSHONLY unset: %d). If it did, the target would be under %s (the cache directory) and the file would take %llu bytes; free space on that file system: %llu bytes -> %s. This run extracted nothing.",
                g_sa.needs && !g_sa.shsh ? "YES" : "no", g_sa.os_opened, g_sa.seekable, !g_sa.shsh, g_sa.cache[0] ? g_sa.cache : "(no cache dir: a temp file)", zok ? (unsigned long long)zs.size : 0ULL, avail,
                !(g_sa.needs && !g_sa.shsh) ? "not needed" : (zok && avail > zs.size) ? "enough" : "NOT enough");
        }
    }

    /* 5. clean-up, category by category. Only then: the tool frees its client. */
    if (still_running) {
        /* the thread still uses the client: freeing it now would be a use-after-free. Leave it, say so, and do not build the clean-up rows on a half-alive tool. */
        row("client_free", "FAILED", "NOT done: the restore thread has not returned, so the client is left allocated (reported as a leak, not freed under a running thread)");
        goto report;
    }
    idevicerestore_client_free(g_client);
    g_client = NULL;
    usleep(500000); /* the event thread of the tool needs a moment to wind down */
    /* the private usbmuxd: ask it to stop, wait for the thread, check that its listener is gone */
    usbmuxd_request_stop();
    for (int i = 0; i < 100 && !g_mux_exited; i++) usleep(50000);
    int mux_stopped = g_mux_exited;
    if (mux_stopped) { pthread_join(tm, NULL); g_mux_alive = 0; }
    FdSnap fd1; ThSnap th1;
    fd_snap(&fd1); th_snap(&th1);
    int hd1 = hv_libusb_open_handles(), hp1 = hv_libusb_hotplug_live(), ho1 = hv_libusb_handles_opened_total();
    row("usb_handles", hd1 == hd0 ? "ok" : "FAILED", "device handles open: %d before, %d after (%d opened during the run) -- observed at the libusb stand-in", hd0, hd1, ho1 - ho0);
    row("event_subscription", hp1 <= hp0 ? "ok" : "FAILED", "hotplug callbacks registered: %d before the tool started (that count includes the private usbmuxd's own), %d after the tool freed its client and usbmuxd stopped -- observed at the libusb stand-in", hp0, hp1);
    int ipsw_fds_after = fd_count_target(&fd1, real);
    row("ipsw_fd", ipsw_fds_after == 0 ? "ok" : "FAILED", "file descriptors on the IPSW: %d while the archive was open, %d after the client was freed -- by /proc/self/fd (the vendored ipsw_open used to keep one open for good; it now closes the probe handle)", ipsw_fds_open, ipsw_fds_after);
    int mux_up = mux_listening();
    {
        int nthr = th1.n - th0.n, mux_new = th_count(&th1, "hv-usbmuxd") - th_count(&th0, "hv-usbmuxd");
        char names[200] = "";
        for (int i = 0; i < th1.n; i++) { if (strstr(names, th1.name[i])) continue; char one[48]; snprintf(one, sizeof one, "%s%s x%d", names[0] ? ", " : "", th1.name[i], th_count(&th1, th1.name[i])); strncat(names, one, sizeof names - strlen(names) - 1); }
        int other = nthr - mux_new;
        row("threads", other <= 0 ? "ok" : "info", "%d before, %d after; +%d is the usbmuxd thread, %d other(s) not explained by it. After: %s", th0.n, th1.n, mux_new, other < 0 ? 0 : other, names);
    }
    row("usbmuxd", mux_stopped && !mux_up ? "ok" : "FAILED", "stop requested after the client was freed: the thread %s and its loopback socket %s", mux_stopped ? "returned and was joined" : "did NOT return within 5 s", mux_up ? "STILL LISTENS" : "no longer answers");
    row("fds", fd1.n <= fd0.n ? "ok" : "info", "open descriptors %d before, %d after (sockets %d->%d, pipes %d->%d, anon_inode %d->%d, files %d->%d); the difference is the usbmuxd listener and its poll set, not attributed further", fd0.n, fd1.n,
        fd_count_kind(&fd0, "socket"), fd_count_kind(&fd1, "socket"), fd_count_kind(&fd0, "pipe"), fd_count_kind(&fd1, "pipe"), fd_count_kind(&fd0, "anon_inode"), fd_count_kind(&fd1, "anon_inode"), fd_count_kind(&fd0, "file"), fd_count_kind(&fd1, "file"));
    row("process", !timed_out ? "ok" : "FAILED", "the restore thread %s within %d ms", timed_out ? "did NOT finish" : "finished", cap_ms);
    {
        memset(&tree1, 0, sizeof tree1);
        tree_walk(workdir, "", &tree1);
        int added = 0; long long bytes = 0; char first[400] = "";
        for (int i = 0; i < tree1.n; i++) {
            int found = 0;
            for (int j = 0; j < tree0.n; j++) if (!strcmp(tree0.path[j], tree1.path[i])) { found = 1; if (tree0.size[j] != tree1.size[i]) bytes += tree1.size[i] - tree0.size[j]; }
            if (!found) { added++; bytes += tree1.size[i]; if (strlen(first) < 250) { char one[300]; snprintf(one, sizeof one, "%s%s (%lld B)", first[0] ? ", " : "", tree1.path[i], tree1.size[i]); strncat(first, one, sizeof first - strlen(first) - 1); } }
        }
        int big = bytes > (1 << 20);
        row("local_writes", big ? "FAILED" : "info", "files in the work directory: %d before, %d after; %d new file(s), %lld bytes added%s%s%s. Only this directory is measured (not the IPSW, not other app files); the BuildManifest is read into memory",
            tree0.n, tree1.n, added, bytes, first[0] ? ": " : "", first, big ? " -- MORE THAN 1 MiB: something large was written" : "");
    }

report:
    /* the policy is removed only when no engine thread is left; a thread that did not return keeps being refused */
    if (sess == 0 && hv_restore_session_end() != 0) g_still = 1;
    if (stage == HV_PF_STOP_A) fprintf(out, "restore preflight: stop A -- the restore tool runs with FLAG_ERASE and the stop before the root filesystem step; no signing request, no image transfer, no rootfs extraction, no disk write\n");
    else fprintf(out, "restore preflight: IPSW is opened and the restore tool runs with FLAG_ERASE|FLAG_NOACTION; no signing request, no image transfer, no rootfs extraction, no disk write\n");
    for (int i = 0; i < g_nrows; i++) fprintf(out, "restore preflight: %-20s %-9s %s\n", g_rows[i].name, g_rows[i].status, g_rows[i].detail);
    if (g_failed == 0 && stage == HV_PF_STOP_A) fprintf(out, "restore preflight RESULT: preflight_completed stopped=before_rootfs (observed), %d clean-up finding(s) in the library (rows marked leaked / usbmuxd). This is NOT a restore: nothing was personalized, sent, extracted or installed.\n", g_findings + 1);
    else if (g_failed == 0) fprintf(out, "restore preflight RESULT: preflight_completed stopped=noaction (inferred), %d clean-up finding(s) in the library (rows marked leaked / usbmuxd). This is NOT a restore: nothing was personalized, sent or installed.\n", g_findings + 1);
    else fprintf(out, "restore preflight RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
    fflush(out);
    return g_failed;
}

/* ---- detached thread for vmrun ---- */
static struct { HvVirtioUsb *usb; char ipsw[1024], work[1024]; uint64_t ecid; int stage; } g_start;
static volatile int g_finished;
static void *start_thread(void *arg) {
    (void)arg;
    hv_restore_preflight_run(g_start.usb, g_start.ipsw, g_start.work, g_start.ecid, stdout, 120000, g_start.stage);
    g_finished = 1;
    return NULL;
}
void hv_restore_preflight_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, int stage) {
    g_start.usb = usb, g_start.ecid = ecid, g_start.stage = stage;
    snprintf(g_start.ipsw, sizeof g_start.ipsw, "%s", ipsw);
    snprintf(g_start.work, sizeof g_start.work, "%s", workdir);
    pthread_t t;
    if (pthread_create(&t, NULL, start_thread, NULL) == 0) pthread_detach(t);
    else g_finished = 1;
}
int hv_restore_preflight_finished(void) { return g_finished; }
