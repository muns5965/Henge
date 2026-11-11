/* Installing macOS on a fresh virtual Mac, the way Virtualization.framework's
   VZMacOSInstaller does: the machine boots its ROM in DFU mode and shows up
   as a USB device, and a standard restore (personalized with Apple's signing
   server for this machine's ECID, then the restore OS writes the disk) runs
   over that link. The restore tools are the open-source libirecovery,
   usbmuxd and idevicerestore, unmodified apart from the boot-ROM exploit left
   out, talking to the guest through host/libusb_shim.c. Nothing of Apple's is
   decrypted or patched here: encrypted images go to the guest as they are, and
   the restore OS asks for its keys itself (the host just fetches the URL). */

#include "install.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "idevicerestore/src/idevicerestore.h"
#include "usb_host.h"
#ifdef __ANDROID__
#include <libtatsu/tss.h>
#include "restore_session.h"
/* curl stand-in (host/curl_mbedtls.c): a download stops when this flag turns non-zero. */
void hv_curl_set_cancel_flag(volatile int *flag);
extern int global_quit_flag;
#endif

int usbmuxd_main(int argc, char **argv);

static HvInstallProgress g_progress;
static void *g_progress_user;
static char g_ipsw[1024], g_cache[1024], g_mux[64];
static uint64_t g_ecid;
static volatile int g_result = 1; /* 1 running, 0 done, < 0 failed */

static void *mux_thread(void *arg) {
    (void)arg;
    char *argv[] = {"usbmuxd", "-f", "-p", "-P", "NONE", "-S", g_mux, "-C", g_cache, NULL};
    int argc = 9;
    if (getenv("HV_INSTALL_LOG")) {
        static char *v[] = {"usbmuxd", "-f", "-v", "-v", "-p", "-P", "NONE", "-S", NULL, "-C", NULL, NULL};
        v[8] = g_mux, v[10] = g_cache;
        usbmuxd_main(11, v);
    } else {
        usbmuxd_main(argc, argv);
    }
    fprintf(stderr, "install: usbmuxd stopped\n");
    return NULL;
}

static void progress(int step, double p, void *user) {
    (void)user;
    if (g_progress) g_progress(step, p, g_progress_user);
}

static void *restore_thread(void *arg) {
    (void)arg;
    struct idevicerestore_client_t *c = idevicerestore_client_new();
    if (!c) {
        g_result = -1;
        return NULL;
    }
    int flags = FLAG_ERASE;
    if (getenv("HV_INSTALL_LOG")) flags |= FLAG_DEBUG;
    idevicerestore_set_flags(c, flags);
#ifdef __ANDROID__
    /* libtatsu prints why a signing request failed (a certificate error, the server's refusal) only at debug level >= 1, and idevicerestore sets that level only
       from its command line, which this host does not have. Level 1 is errors; level 2 would print the request, which carries the device's identifiers. */
    tss_set_debug_level(1);
    /* idevicerestore's own quit flag also ends a stalled download (the stand-in polls it about once a second). */
    hv_curl_set_cancel_flag((volatile int *)&global_quit_flag);
#endif
    idevicerestore_set_ipsw(c, g_ipsw);
    idevicerestore_set_cache_path(c, g_cache);
    if (g_ecid) idevicerestore_set_ecid(c, g_ecid);
    idevicerestore_set_progress_callback(c, progress, NULL);
    int r;
#ifdef __ANDROID__
    /* Android: the restore tool starts only through the session, which refuses to run unless the connection policy (host/net_guard.h) is in force. The allow
       list holds the private usbmuxd and NOTHING else, so a signing request or a URLAsset download is refused until its destination is approved and added
       (hv_net_guard_allow_host) -- that approval is a separate step. */
    if (hv_restore_session_run_client(c, &r) != 0) {
        fprintf(stderr, "install: refused -- the connection policy is not in force\n");
        r = -1;
    }
#else
    r = idevicerestore_start(c);
#endif
    fprintf(stderr, "install: restore finished: %d\n", r);
#ifdef __ANDROID__
    /* C18: close the session first, then ask whether releasing is allowed. When the session did not quiesce, the client is deliberately leaked and the policy
       stays in force -- freeing it under a task that may still use it is the use-after-free the fake-device baseline (T5) reproduced. */
    if (hv_restore_session_close() != 0 || !hv_restore_session_release_permitted()) {
        fprintf(stderr, "install: the session did not quiesce -- nothing released, policy kept\n");
        r = -1;
    } else {
        idevicerestore_client_free(c);
        if (hv_restore_session_end() != 0) fprintf(stderr, "install: session end refused (policy stays)\n");
    }
#else
    idevicerestore_client_free(c);
#endif
    g_result = r == 0 ? 0 : -1;
    if (g_progress) g_progress(r == 0 ? HV_INSTALL_DONE : HV_INSTALL_FAILED, 1.0, g_progress_user);
    return NULL;
}

int hv_install_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, HvInstallProgress cb,
                     void *user) {
    if (!usb) return -1;
#ifdef __ANDROID__
    /* One restore at a time: a second start would run two restore threads over the same files and the same usbmuxd port. */
    static volatile int started;
    if (started && g_result == 1) {
        fprintf(stderr, "install: a restore is already running\n");
        return -1;
    }
    started = 1;
    g_result = 1;
#endif
    snprintf(g_ipsw, sizeof g_ipsw, "%s", ipsw);
    snprintf(g_cache, sizeof g_cache, "%s", workdir);
    /* A private usbmuxd on loopback; the restore tools find it through this. */
    int port = 27100 + (int)(getpid() % 800);
    snprintf(g_mux, sizeof g_mux, "127.0.0.1:%d", port);
    setenv("USBMUXD_SOCKET_ADDRESS", g_mux, 1);
#ifdef __ANDROID__
    if (hv_restore_session_begin() != 0) {
        fprintf(stderr, "install: not started -- the connection policy could not be put in force (a previous run may still be ending)\n");
        g_result = -1;
        return -1;
    }
    hv_restore_session_allow_usbmuxd((uint16_t)port);
#endif
    g_ecid = ecid;
    g_progress = cb;
    g_progress_user = user;
    hv_libusb_shim_attach(usb);
    pthread_t t;
    if (pthread_create(&t, NULL, mux_thread, NULL) != 0) return -1;
    pthread_detach(t);
    usleep(300000);
    if (pthread_create(&t, NULL, restore_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

int hv_install_result(void) { return g_result; }

void hv_install_cancel(void) {
#ifdef __ANDROID__
    hv_restore_session_request_cancel();
    global_quit_flag = 1;   /* the same flag idevicerestore's own signal handler sets */
#endif
}
