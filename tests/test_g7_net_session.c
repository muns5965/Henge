/* The G7 network gate against the REAL restore session and lifetime registry (host/restore_session.c, host/restore_registry.c), the way host/install.c's restore thread uses them:
   begin -> hv_g7_extra_flags -> hv_g7_net_gate -> (run only when the gate is open) -> close -> finalize -> publish -> release_idle. The engine is a stand-in function: no restore tool,
   no device, no network; the destinations are only asked of the policy, never connected. The Android-only restore_thread itself is not compiled here (it is under __ANDROID__). */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "install_g7.h"
#include "net_guard.h"
#include "restore_registry.h"
#include "restore_session.h"

static int nfail, engine_calls, engine_allowed, engine_denied;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) nfail++; } while (0)

static int dec(const char *name, int port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, "17.253.1.1", &a.sin_addr);
    return hv_net_guard_check_p((struct sockaddr *)&a, sizeof a, name, HV_NET_P_HTTP, NULL, 0);
}
static int engine(void *a) {
    (void)a; engine_calls++;
    engine_allowed = dec("gs.apple.com", 443) == HV_NET_ALLOW;
    engine_denied = dec("updates.cdn-apple.com", 443) == HV_NET_DENY && dec("gs.apple.com", 80) == HV_NET_DENY;
    return 7;
}
static int end_session(void) {   /* the same tail as install.c: close, finalize, publish, release */
    int mp = 0, ok = hv_restore_session_close() == 0;
    hv_restore_session_finalize(-1, 0, &mp);
    if (mp) hv_restore_session_publish(NULL, NULL);
    return ok && hv_restore_session_release_idle() == 0 && hv_reg_state() == HV_REG_IDLE;
}

int main(void) {
    hv_restore_session_set_close_timeout_ms(300);
    int rc = 0;
    /* 1: every host registers -> the gate is open -> the tool may run, the list is what the policy enforces, refusals are recorded */
    CHECK(hv_restore_session_begin() == 0, "begin");
    CHECK(hv_g7_net_gate() == -1, "before registration the gate is closed (fail closed)");
    hv_g7_extra_flags();
    CHECK(hv_g7_net_gate() == 0, "after registration the gate is open");
    CHECK(hv_restore_session_run(engine, NULL, &rc) == 0 && rc == 7 && engine_calls == 1, "the engine runs");
    CHECK(engine_allowed, "inside the engine: gs.apple.com:443 allowed");
    CHECK(engine_denied, "inside the engine: an unlisted host and the wrong port refused");
    char log[1024]; hv_net_guard_denied_log(log, sizeof log);
    CHECK(strstr(log, "updates.cdn-apple.com") && strstr(log, ":443") && strstr(log, "gs.apple.com") && strstr(log, ":80"), "the refusals are in the denied log (host name and port)");
    CHECK(end_session(), "the session ends normally (registry IDLE)");

    /* 2: registration fails AFTER begin() reset the lists (the list is filled then), so it fails at the real registration point -> the engine is not entered, the session still ends */
    engine_calls = 0;
    CHECK(hv_restore_session_begin() == 0, "begin again: the earlier policy is gone, the lists are reset");
    for (int i = 0; i < 8; i++) { char b[32]; snprintf(b, sizeof b, "fill%d.example", i); hv_net_guard_allow_host(b, 443); }
    hv_g7_extra_flags();
    CHECK(hv_g7_net_registered_ok() == 0 && hv_g7_net_gate() == -1, "registration failed: the gate stays closed");
    CHECK(dec("gs.apple.com", 443) == HV_NET_DENY, "and the host is refused by the policy as well");
    int ran = 0;
    if (hv_g7_net_gate() == 0) { hv_restore_session_run(engine, NULL, &rc); ran = 1; }
    CHECK(!ran && engine_calls == 0, "the restore client is not entered");
    CHECK(hv_net_guard_engine_count() == 0, "no engine thread is registered");
    CHECK(end_session(), "the session still closes, finalizes and returns to IDLE (nothing left behind)");

    /* 3: the next session starts clean and registers again */
    CHECK(hv_restore_session_begin() == 0, "the next session can begin after a gated one");
    hv_g7_extra_flags();
    CHECK(hv_g7_net_gate() == 0 && dec("gs.apple.com", 443) == HV_NET_ALLOW, "and registers fully (the filled list did not survive begin)");
    CHECK(end_session(), "and ends");
    hv_restore_session_end();

    printf(nfail ? "G7NETSESSION FAILED (%d)\n" : "G7NETSESSION OK (%d failed)\n", nfail);
    return nfail != 0;
}
