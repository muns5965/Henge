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
#ifdef HV_INSTALL_G0
#include "install_g0.h"   /* the G0 test build only (tools/vmrun-install-g0-build.sh): see host/install_g0.h */
#endif
#ifdef HV_INSTALL_G1
#include "install_g1.h"   /* the G1 test build only (tools/install-g1-test.sh): see host/install_g1.h */
#endif
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
static int g_result = 1; /* 1 running, 0 done, < 0 failed; read and written only through LD/ST (the UI thread polls it, the restore thread sets it) */
#define LD(v) __atomic_load_n(&(v), __ATOMIC_SEQ_CST)
#define ST(v, x) __atomic_store_n(&(v), (x), __ATOMIC_SEQ_CST)

#ifdef __ANDROID__
#include <time.h>
#include "restore_registry.h"
extern void usbmuxd_request_stop(void);   /* third_party/usbmuxd/src/main.c (Henge) */
extern volatile int usbmuxd_stopped;
static pthread_t g_mux_th;
static int g_mux_alive;     /* a private usbmuxd thread was started and is not joined: no new start until the process restarts */
/* PROVISIONAL, not measured on a device: how long the private usbmuxd gets to stop (its main loop wakes within ~200 ms). */
static int g_mux_stop_ms = 5000;
#endif

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

#ifdef __ANDROID__
/* Ask the private usbmuxd to stop and wait for its thread (bounded). 1 = stopped and joined. On failure the thread stays (g_mux_alive) and the session is
   declared not quiescent: the thread still owns sockets and the USB shim. */
static int hv_install_stop_mux(void) {
    if (!LD(g_mux_alive)) return 1;
    usbmuxd_request_stop();
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    int64_t end_ns = (int64_t)dl.tv_sec * 1000000000LL + dl.tv_nsec + (int64_t)g_mux_stop_ms * 1000000LL;
    for (;;) {
        if (usbmuxd_stopped) { pthread_join(g_mux_th, NULL); ST(g_mux_alive, 0); return 1; }
        clock_gettime(CLOCK_MONOTONIC, &dl);
        if ((int64_t)dl.tv_sec * 1000000000LL + dl.tv_nsec >= end_ns) break;
        usleep(10000);
    }
    hv_restore_session_fail(HV_REG_QF_ENGINE_NOT_EXITED);   /* closest reason: a thread of the restore did not exit */
    return 0;
}
#endif

#ifdef __ANDROID__
/* the publication callback: the progress callback with the final step. Runs only through hv_restore_session_publish (outside every lock). */
static void publish_final(void *p) {
    int ok = (int)(intptr_t)p;
    if (g_progress) g_progress(ok ? HV_INSTALL_DONE : HV_INSTALL_FAILED, 1.0, g_progress_user);
}
#endif

static void *restore_thread(void *arg) {
#ifdef __ANDROID__
    /* take over the session from the starting thread (C13): the token was issued to this thread's creator and passed as the argument */
    if (hv_restore_session_claim((uint64_t)(uintptr_t)arg) != 0) {
        fprintf(stderr, "install: this thread could not take over the session; nothing was run\n");
        ST(g_result, -1);   /* the session stays with its owner and refuses a new start (not quiescent for lack of an owner who can end it) */
        return NULL;
    }
#else
    (void)arg;
#endif
    struct idevicerestore_client_t *c = idevicerestore_client_new();
    if (!c) {
        ST(g_result, -1);
        return NULL;
    }
    int flags = FLAG_ERASE;
#ifdef HV_INSTALL_G0
    flags |= hv_g0_extra_flags();   /* G0 test build: the tool's own stop before the root filesystem step (and the hook that reports it) */
#endif
#ifdef HV_INSTALL_G1
    flags |= hv_g1_extra_flags(c);   /* G1 test build: the tool's own end after the signing requests, the signing URL forced to the test server, its one port approved (after the session began) */
#endif
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
    /* C18/C9: close the session first (CLOSED or QUIESCENCE_FAILED), then release what the session allows, then ONE final result composed from three things:
       the engine's return value, the cleanup (tasks, client, private usbmuxd) and the session end (policy removal). Nothing is released after a failed close:
       the client is deliberately leaked and the policy and usbmuxd stay, because a task may still use them (the use-after-free the T5 baseline reproduced). */
#if defined(HV_INSTALL_G0) || defined(HV_INSTALL_G1)
    int engine_rc = r;
#endif
    int closed = hv_restore_session_close() == 0;
    int cleanup_mask = 0;
    if (closed && hv_restore_session_release_permitted()) {
        if (idevicerestore_client_release(c) != 0) {
            /* an async handler is still running: the client was NOT freed (it stays valid) and the private usbmuxd keeps running for it */
            cleanup_mask |= HV_REG_CL_OTHER;
            fprintf(stderr, "install: async handlers did not finish; the client is left allocated and the private usbmuxd keeps running\n");
        } else if (!hv_install_stop_mux()) {
            cleanup_mask |= HV_REG_CL_HOST_THREAD;
            fprintf(stderr, "install: the private usbmuxd did not stop; no new restore can start until the app restarts\n");
        }
    } else if (closed) {
        cleanup_mask |= HV_REG_CL_OTHER;   /* release was not permitted: a cleanup failure was already recorded, nothing is released */
    } else {
        fprintf(stderr, "install: the session did not quiesce -- nothing released, policy kept\n");
    }
    int may_publish = 0;
    int rep = hv_restore_session_finalize(r, cleanup_mask, &may_publish);
#ifdef HV_INSTALL_G0
    hv_g0_record(engine_rc, closed, cleanup_mask, rep);   /* before g_result changes, so a poller that sees the end also sees this */
#endif
#ifdef HV_INSTALL_G1
    hv_g1_record(engine_rc, closed, cleanup_mask, rep);
#endif
    fprintf(stderr, "install: final result %d (1 success, 2 restore failed, 3 user cancel, 5 cleanup failed, 6 not quiescent; 0 = not decided)\n", rep);
    r = rep == HV_REG_R_SUCCESS ? 0 : -1;
    /* Publishing: only the caller that received the right publishes, outside every lock; the session leaves CLOSED only after the publication returned (C15 (e)).
       Called from inside the callback, start/cancel/release_idle are refused (the result is still 1 / PUBLISHING). A callback that never returns keeps the session
       CLOSED and refuses the next start -- the safe way to fail; the host sets no time limit on a callback. */
    if (may_publish) hv_restore_session_publish(publish_final, (void *)(intptr_t)(r == 0));
    if (rep != 0 && hv_restore_session_release_idle() != 0)
        fprintf(stderr, "install: the session stays closed (cleanup failure, reasons 0x%x; or not quiescent); no new restore can start until the app restarts\n", hv_reg_cleanup_history());
    ST(g_result, r == 0 ? 0 : -1);   /* published last: a start that sees it also sees the finished session */
    return NULL;
#else
    if (idevicerestore_client_release(c) != 0) {
        fprintf(stderr, "install: async handlers did not finish; the client is left allocated\n");
        r = -1;
    }
    if (g_progress) g_progress(r == 0 ? HV_INSTALL_DONE : HV_INSTALL_FAILED, 1.0, g_progress_user);
    ST(g_result, r == 0 ? 0 : -1);
    return NULL;
#endif
}

int hv_install_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, HvInstallProgress cb,
                     void *user) {
    if (!usb) return -1;
#ifdef __ANDROID__
    /* One restore at a time: a second start would run two restore threads over the same files and the same usbmuxd port. */
    static int started;
    if (LD(started) && LD(g_result) == 1) {
        fprintf(stderr, "install: a restore is already running\n");
        return -1;
    }
    if (LD(g_mux_alive)) {
        fprintf(stderr, "install: refused -- the private usbmuxd of an earlier run did not stop; restart the app\n");
        return -1;
    }
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
        return -1;
    }
    /* begin() is only reachable from IDLE, i.e. after the previous session closed cleanly, so no task of the earlier run can still read these flags */
    ST(global_quit_flag, 0);
    ST(started, 1);
    ST(g_result, 1);
    hv_restore_session_allow_usbmuxd((uint16_t)port);
#endif
    g_ecid = ecid;
    g_progress = cb;
    g_progress_user = user;
    hv_libusb_shim_attach(usb);
    pthread_t t;
#ifdef __ANDROID__
    usbmuxd_stopped = 0;
    if (pthread_create(&g_mux_th, NULL, mux_thread, NULL) != 0) {   /* nothing is running yet: back to IDLE */
        fprintf(stderr, "install: not started -- the usbmuxd thread could not be created\n");
        if (hv_restore_session_abort_start() != 0) fprintf(stderr, "install: the session could not be ended; no new restore can start until the app restarts\n");
        ST(started, 0);
        ST(g_result, -1);
        return -1;
    }
    ST(g_mux_alive, 1);
    usleep(300000);
    uint64_t tok = hv_restore_session_approve_successor();   /* the restore thread takes over the session with this; a failed creation never uses it (abort_start drops it) */
    if (tok == 0 || pthread_create(&t, NULL, restore_thread, (void *)(uintptr_t)tok) != 0) {
        /* usbmuxd IS running: it has to be stopped and joined before the session may end; if it does not stop, the session is not quiescent and stays */
        fprintf(stderr, "install: not started -- the restore thread could not be created\n");
        if (hv_install_stop_mux()) {
            if (hv_restore_session_abort_start() != 0) fprintf(stderr, "install: the session could not be ended; no new restore can start until the app restarts\n");
        } else {
            fprintf(stderr, "install: the private usbmuxd did not stop; session kept, policy kept\n");
        }
        ST(started, 0);
        ST(g_result, -1);
        return -1;
    }
    pthread_detach(t);
    return 0;
#else
    if (pthread_create(&t, NULL, mux_thread, NULL) != 0) return -1;
    pthread_detach(t);
    usleep(300000);
    if (pthread_create(&t, NULL, restore_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
#endif
}

int hv_install_result(void) { return LD(g_result); }

void hv_install_cancel(void) {
#ifdef __ANDROID__
    /* Accepted once per session (C17). It sets the session's terminate flag and the tool's quit flag; it does NOT wait for anything to stop -- the result
       arrives through the callback like any other. The flag is cleared again only at the next begin, which exists only after a clean close. */
    if (hv_restore_session_cancel() == 1) {
        ST(global_quit_flag, 1);   /* the same flag idevicerestore's own signal handler sets */
    }
#endif
}
