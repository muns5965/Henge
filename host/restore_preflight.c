/* Build 69-a: the real restore tool's start-up on the virtual Mac, stopped by its own FLAG_NOACTION. See restore_preflight.h for what runs and what is excluded. */
#define _GNU_SOURCE 1
#include "restore_preflight.h"

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

/* ---- the private usbmuxd, as host/install.c starts it ---- */
static char g_mux[64], g_cache[1024];
static void *mux_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "hv-usbmuxd");
    char *argv[] = {"usbmuxd", "-f", "-p", "-P", "NONE", "-S", g_mux, "-C", g_cache, NULL};
    usbmuxd_main(9, argv);
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
typedef struct { const char *name; const char *status; char detail[400]; } Row;
static Row g_rows[24];
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
    g_rc = idevicerestore_start(g_client);
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

int hv_restore_preflight_run(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, FILE *out, int cap_ms) {
    g_nrows = g_failed = g_findings = 0;
    g_calls = 0, g_max_step = -1, g_last_step = -1, g_last_p = -1, g_rc = 12345, g_done = 0;
    if (!usb) { fprintf(out, "restore preflight: no USB port\n"); return -1; }

    /* 1. the safety boundary first: deny mode on, and proven on before anything can ask for the network */
    hv_curl_deny_network(1);
    int att0 = hv_curl_attempt_count(), den0 = hv_curl_denied_count();
    if (hv_curl_deny_active()) row("deny_mode", "ok", "the curl stand-in refuses every request before any lookup, connection or TLS (scope: that client only; the loopback usbmuxd socket is not counted)");
    else row("deny_mode", "FAILED", "deny mode did not turn on; nothing else is started");
    if (g_failed) goto report;

    if (hv_usb_wait_online(usb, 60000) < 0) { row("guest_on_bus", "FAILED", "the guest did not come on the USB bus within 60 s; the tool was not started"); goto report; }
    FdSnap fd0; ThSnap th0;
    fd_snap(&fd0); th_snap(&th0);
    int ctrl0 = hv_libusb_ctrl_count(), data0 = hv_libusb_data_transfer_count();

    /* 2. the private usbmuxd, then the real tool */
    snprintf(g_cache, sizeof g_cache, "%s", workdir);
    snprintf(g_mux, sizeof g_mux, "127.0.0.1:%d", 27100 + (int)(getpid() % 800));
    setenv("USBMUXD_SOCKET_ADDRESS", g_mux, 1);
    hv_libusb_shim_attach(usb);
    pthread_t tm, tr;
    if (pthread_create(&tm, NULL, mux_thread, NULL) != 0) { row("init", "FAILED", "usbmuxd thread did not start"); goto report; }
    pthread_detach(tm);
    usleep(300000);
    /* the baseline of the tool's own subscription and handles is taken with usbmuxd already up: it registers its own hotplug callback and keeps it for good */
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
#ifdef HV_PREFLIGHT_NEGATIVE_CONTROL
    /* tests/test_preflight.c only (a second build of this file): without the stop the tool must go on, which proves the stop is what ends the run. Never in a shipped build. */
    idevicerestore_set_flags(g_client, FLAG_ERASE);
#else
    idevicerestore_set_flags(g_client, FLAG_ERASE | FLAG_NOACTION);
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
    if (g_done) pthread_join(tr, NULL);
    double took = now_ms() - t0;

    row("init", (!timed_out && g_rc == 0) ? "ok" : "FAILED", "idevicerestore_start returned %d after %.0f ms%s", g_rc, took, timed_out ? " -- TIMEOUT, quit flag raised" : "");
    char note[240];
    int pv = hv_restore_preflight_progress_verdict(g_calls, g_max_step, g_last_step, g_last_p, note, sizeof note);
    row("progress", pv ? "ok" : "FAILED", "%s", note);

    /* 3. what crossed the boundary */
    int att = hv_curl_attempt_count() - att0, den = hv_curl_denied_count() - den0;
    row("network_requests", att == 0 ? "ok" : "FAILED", "curl stand-in: %d attempt(s), %d refused by deny mode (scope: the curl client only; the loopback usbmuxd socket and other libraries are not counted)", att, den);
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
    int stopped = !timed_out && g_rc == 0 && pv && att == 0;
    row("stopped=noaction", stopped ? "inferred" : "FAILED",
        "%s -- INFERRED from the code: with FLAG_PWN, FLAG_SHSHONLY and FLAG_NO_RESTORE unset, a 0 return before any later step can only come from the FLAG_NOACTION branch (every other early exit returns negative); the branch itself writes no log line and no callback", stopped ? "the tool stopped at its no-action point" : "the conditions for that inference do not hold");

    /* 5. clean-up, category by category. Only then: the tool frees its client. */
    idevicerestore_client_free(g_client);
    g_client = NULL;
    usleep(500000); /* the event thread of the tool needs a moment to wind down */
    FdSnap fd1; ThSnap th1;
    fd_snap(&fd1); th_snap(&th1);
    int hd1 = hv_libusb_open_handles(), hp1 = hv_libusb_hotplug_live(), ho1 = hv_libusb_handles_opened_total();
    row("usb_handles", hd1 == hd0 ? "ok" : "FAILED", "device handles open: %d before, %d after (%d opened during the run) -- observed at the libusb stand-in", hd0, hd1, ho1 - ho0);
    row("event_subscription", hp1 == hp0 ? "ok" : "FAILED", "hotplug callbacks registered: %d before the tool started (the private usbmuxd holds its own and keeps it), %d after the tool freed its client -- observed at the libusb stand-in", hp0, hp1);
    int ipsw_fds_after = fd_count_target(&fd1, real);
    if (ipsw_fds_after > ipsw_fds_open) row("ipsw_fd", "FAILED", "file descriptors on the IPSW: %d while open, %d after the client was freed: it grew -- by /proc/self/fd", ipsw_fds_open, ipsw_fds_after);
    else if (ipsw_fds_after > 0) {
        g_findings++;
        row("ipsw_fd", "leaked", "file descriptors on the IPSW: %d while open, %d after the client was freed -- by /proc/self/fd. This is the vendored tool's own leak (ipsw_open calls zip_open and never zip_close; ipsw_close frees only the path), one descriptor per opened IPSW, reclaimed by the OS at process exit; not changed here (vendored source)", ipsw_fds_open, ipsw_fds_after);
    } else row("ipsw_fd", ipsw_fds_open > 0 ? "ok" : "info", "file descriptors on the IPSW: %d while open, %d after the client was freed -- by /proc/self/fd%s", ipsw_fds_open, ipsw_fds_after, ipsw_fds_open > 0 ? "" : " (none was visible while open, so this says nothing about release)");
    int mux_up = mux_listening();
    {
        int nthr = th1.n - th0.n, mux_new = th_count(&th1, "hv-usbmuxd") - th_count(&th0, "hv-usbmuxd");
        char names[200] = "";
        for (int i = 0; i < th1.n; i++) { if (strstr(names, th1.name[i])) continue; char one[48]; snprintf(one, sizeof one, "%s%s x%d", names[0] ? ", " : "", th1.name[i], th_count(&th1, th1.name[i])); strncat(names, one, sizeof names - strlen(names) - 1); }
        int other = nthr - mux_new;
        row("threads", other <= 0 ? "ok" : "info", "%d before, %d after; +%d is the usbmuxd thread, %d other(s) not explained by it. After: %s", th0.n, th1.n, mux_new, other < 0 ? 0 : other, names);
    }
    row("usbmuxd", "info", "the usbmuxd thread NEVER exits (usbmuxd_main has no stop) and its loopback socket %s: left running by design, so \"all resources released\" is NOT claimed. At process exit the OS reclaims it; that is not the library's own clean-up", mux_up ? "still listens" : "no longer answers");
    row("fds", "info", "open descriptors %d before, %d after (sockets %d->%d, pipes %d->%d, anon_inode %d->%d, files %d->%d); the difference is the usbmuxd listener and its poll set, not attributed further", fd0.n, fd1.n,
        fd_count_kind(&fd0, "socket"), fd_count_kind(&fd1, "socket"), fd_count_kind(&fd0, "pipe"), fd_count_kind(&fd1, "pipe"), fd_count_kind(&fd0, "anon_inode"), fd_count_kind(&fd1, "anon_inode"), fd_count_kind(&fd0, "file"), fd_count_kind(&fd1, "file"));
    row("process", !timed_out ? "ok" : "FAILED", "the restore thread %s within %d ms", timed_out ? "did NOT finish" : "finished", cap_ms);

report:
    fprintf(out, "restore preflight: IPSW is opened and the restore tool runs with FLAG_ERASE|FLAG_NOACTION; no signing request, no image transfer, no rootfs extraction, no disk write\n");
    for (int i = 0; i < g_nrows; i++) fprintf(out, "restore preflight: %-20s %-9s %s\n", g_rows[i].name, g_rows[i].status, g_rows[i].detail);
    if (g_failed == 0) fprintf(out, "restore preflight RESULT: preflight_completed stopped=noaction (inferred), %d clean-up finding(s) in the library (rows marked leaked / usbmuxd). This is NOT a restore: nothing was personalized, sent or installed.\n", g_findings + 1);
    else fprintf(out, "restore preflight RESULT: FAILED (%d row(s)). Not a restore.\n", g_failed);
    fflush(out);
    return g_failed;
}

/* ---- detached thread for vmrun ---- */
static struct { HvVirtioUsb *usb; char ipsw[1024], work[1024]; uint64_t ecid; } g_start;
static volatile int g_finished;
static void *start_thread(void *arg) {
    (void)arg;
    hv_restore_preflight_run(g_start.usb, g_start.ipsw, g_start.work, g_start.ecid, stdout, 120000);
    g_finished = 1;
    return NULL;
}
void hv_restore_preflight_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid) {
    g_start.usb = usb, g_start.ecid = ecid;
    snprintf(g_start.ipsw, sizeof g_start.ipsw, "%s", ipsw);
    snprintf(g_start.work, sizeof g_start.work, "%s", workdir);
    pthread_t t;
    if (pthread_create(&t, NULL, start_thread, NULL) == 0) pthread_detach(t);
    else g_finished = 1;
}
int hv_restore_preflight_finished(void) { return g_finished; }
