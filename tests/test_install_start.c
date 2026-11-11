/* host/install.c (compiled -D__ANDROID__ on Linux) with the session and the lifetime registry: start-failure paths, the final result, cancel and restart.
   Link-time wrappers replace pthread_create (failure injection), usbmuxd_main and idevicerestore_start (fakes); nothing touches a device or the network. */
#define _GNU_SOURCE 1
#include "install.h"
#include "restore_session.h"
#include "restore_registry.h"
#include "net_guard.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int global_quit_flag;
extern int should_exit;
extern volatile int usbmuxd_stopped;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

static int nfail;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); fflush(stdout); if (!ok_) nfail++; } while (0)

static volatile int arm_calls, fail_on;      /* fail the Nth pthread_create after arming (1-based); 0 = pass through */
static volatile int mux_ignores_stop, mux_running, mux_release, engine_mode, engine_rc, engine_seen_quit;
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *d) {
    if (fail_on && ++arm_calls == fail_on) return 11;
    return __real_pthread_create(t, a, f, d);
}
int __wrap_usbmuxd_main(int argc, char **argv) {
    (void)argc; (void)argv;
    should_exit = 0; mux_running = 1;
    while (!should_exit || (mux_ignores_stop && !mux_release)) usleep(5000);
    mux_running = 0; usbmuxd_stopped = 1;
    return 0;
}
int __wrap_idevicerestore_start(void *c) {
    (void)c;
    if (engine_mode == 1) { for (int i = 0; i < 400 && !global_quit_flag; i++) usleep(10000); engine_seen_quit = global_quit_flag; return global_quit_flag ? -1 : 0; }
    return engine_rc;
}

static int last_step, cb_calls, in_cb_start = 99, in_cb_quit = -1, in_cb_state = -1, in_cb_rel = 99, cb_hang;
static char dummy_usb[64];
static void cb(int step, double p, void *u) {
    (void)p; (void)u; last_step = step; cb_calls++;
    if (cb_hang == 2) {   /* re-entry from inside the callback: every one of these must be refused */
        in_cb_start = hv_install_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1, NULL, NULL);
        hv_install_cancel();
        in_cb_quit = global_quit_flag;
        in_cb_state = (int)hv_reg_state();
        in_cb_rel = hv_restore_session_release_idle();
    }
}
static int run_one(int *rc_start) {   /* start and wait for the result */
    last_step = 0; arm_calls = 0;
    int s = hv_install_start((HvVirtioUsb *)dummy_usb, "x.ipsw", "/tmp", 0x1F4C0DE, cb, NULL);
    if (rc_start) *rc_start = s;
    if (s != 0) return -2;
    for (int i = 0; i < 1500 && hv_install_result() == 1; i++) usleep(10000);
    return hv_install_result();
}

int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
    hv_restore_session_set_close_timeout_ms(300);
    int s;
    if (!strcmp(c, "ok")) {
        engine_rc = 0;
        CHECK(run_one(&s) == 0 && s == 0 && last_step == HV_INSTALL_DONE, "a clean run: result success, DONE callback");
        CHECK(hv_reg_report_final() == HV_REG_R_SUCCESS && hv_reg_state() == HV_REG_IDLE && !mux_running, "final report SUCCESS once, registry IDLE, the private usbmuxd stopped and joined");
        CHECK(!hv_net_guard_hooks_intact(), "the policy was removed");
        engine_rc = -1;
        CHECK(run_one(&s) == -1 && s == 0 && last_step == HV_INSTALL_FAILED && hv_reg_report_final() == HV_REG_R_RESTORE_FAILED, "a second run starts (restart works) and a failed engine is RESTORE_FAILED");
    } else if (!strcmp(c, "p1")) {
        fail_on = 1; arm_calls = 0;
        CHECK(hv_install_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1, cb, NULL) == -1, "first pthread_create fails: start returns -1");
        CHECK(hv_reg_state() == HV_REG_IDLE && !hv_net_guard_hooks_intact() && !mux_running, "session back to IDLE, policy removed, no thread left");
        fail_on = 0; engine_rc = 0;
        CHECK(run_one(&s) == 0, "the next start works");
    } else if (!strcmp(c, "p2")) {
        fail_on = 2; arm_calls = 0;
        CHECK(hv_install_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1, cb, NULL) == -1, "second pthread_create fails: start returns -1");
        CHECK(!mux_running && usbmuxd_stopped, "the usbmuxd thread that WAS started was stopped and joined");
        CHECK(hv_reg_state() == HV_REG_IDLE && !hv_net_guard_hooks_intact(), "session IDLE, policy removed");
        fail_on = 0; engine_rc = 0;
        CHECK(run_one(&s) == 0, "the next start works");
    } else if (!strcmp(c, "p2_stuck")) {
        mux_ignores_stop = 1; fail_on = 2; arm_calls = 0;
        CHECK(hv_install_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1, cb, NULL) == -1, "second create fails and the usbmuxd thread ignores the stop request: -1");
        CHECK(hv_reg_state() == HV_REG_QUIESCENCE_FAILED && hv_net_guard_hooks_intact(), "QUIESCENCE_FAILED, policy kept (not 'failed, so allow the next one')");
        fail_on = 0;
        CHECK(hv_install_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1, cb, NULL) == -1, "the next start is refused");
        mux_release = 1;
    } else if (!strcmp(c, "end_fail")) {   /* the engine succeeded; the cleanup (usbmuxd stop) did not */
        engine_rc = 0; mux_ignores_stop = 1;
        int r = run_one(&s);
        CHECK(s == 0 && r == -1 && last_step == HV_INSTALL_FAILED, "engine returned 0 but the usbmuxd did not stop: the result is NOT success");
        CHECK(hv_reg_report_final() == HV_REG_R_CLEANUP_FAILED && hv_reg_state() == HV_REG_CLOSED, "reported as CLEANUP_FAILED once (engine closed cleanly, the cleanup did not); the session stays CLOSED, not IDLE");
        CHECK(hv_install_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1, cb, NULL) == -1, "the next start is refused");
        CHECK(hv_net_guard_hooks_intact(), "policy kept");
        CHECK(hv_reg_cleanup_history() == HV_REG_CL_HOST_THREAD, "the reason is recorded: a host-owned thread is alive (not a generic cleanup failure)");
        mux_release = 1;
    } else if (!strcmp(c, "cancel")) {
        engine_mode = 1;
        s = hv_install_start((HvVirtioUsb *)dummy_usb, "x", "/tmp", 1, cb, NULL);
        usleep(400000);
        hv_install_cancel(); hv_install_cancel();
        for (int i = 0; i < 1500 && hv_install_result() == 1; i++) usleep(10000);
        CHECK(s == 0 && engine_seen_quit && hv_install_result() == -1, "cancel reached the engine through the quit flag; the run ends as a failure");
        CHECK(hv_reg_report_final() == HV_REG_R_ABORTED_BY_USER && hv_reg_state() == HV_REG_IDLE, "reported ABORTED_BY_USER once; session closed and IDLE");
        engine_mode = 0; engine_rc = 0;
        CHECK(run_one(&s) == 0 && global_quit_flag == 0, "after a clean close the next start clears the quit flag and succeeds");
        hv_install_cancel();
        CHECK(global_quit_flag == 0, "a cancel with no session running is refused and sets nothing");
    } else if (!strcmp(c, "reenter")) {
        engine_rc = 0; cb_hang = 2;
        CHECK(run_one(&s) == 0 && cb_calls == 1, "a clean run, the callback ran once");
        CHECK(in_cb_start == -1 && in_cb_quit == 0 && in_cb_state == HV_REG_CLOSED && in_cb_rel == -1, "inside the callback: start refused, cancel refused (quit flag untouched), session still CLOSED, release_idle refused");
        CHECK(hv_reg_state() == HV_REG_IDLE && hv_reg_pub_state() == HV_REG_PUB_PUBLISHED, "after it returned: published, IDLE");
        cb_hang = 0;
        CHECK(run_one(&s) == 0, "the next start works");
    } else { fprintf(stderr, "unknown case\n"); return 2; }
    return nfail;
}
