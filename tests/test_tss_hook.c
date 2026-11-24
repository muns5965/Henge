/* libtatsu's per-attempt hook with the real curl stand-in: a TSS request to a closed loopback port. Nothing leaves this machine. Built against the HV_TSS_HTTPS_ONLY flavour. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <plist/plist.h>
#include "libtatsu/tss.h"
#include "../host/curl_last.h"

void tss_set_max_attempts(int n);
void tss_set_attempt_hook(void (*hook)(int, int, long, long long, int, long long));
const char *tss_last_diag(void);

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_calls, g_first_attempt, g_last_attempt, g_rc, g_conn_errno, g_tried, g_connected, g_diag_has_attempt;
static long long g_ms;
static int g_url_idx = -1;
static void hook(int attempt, int rc, long http, long long ms, int url_index, long long since_start) {
    (void)since_start;
    g_url_idx = url_index;
    HvCurlLast l; hv_curl_last(&l);   /* same thread, right after the transfer */
    if (!g_calls) g_first_attempt = attempt;
    g_calls++; g_last_attempt = attempt; g_rc = rc; g_ms = ms; (void)http;
    g_conn_errno = l.conn_errno; g_tried = l.addrs_tried; g_connected = l.connected;
    char want[32]; snprintf(want, sizeof want, "TSS_ATTEMPTS: %d", attempt);
    g_diag_has_attempt = strstr(tss_last_diag(), want) != NULL;
}

int main(void) {
    plist_t req = plist_new_dict();
    plist_dict_set_item(req, "@Test", plist_new_bool(1));
    tss_set_max_attempts(2);
    tss_set_attempt_hook(hook);
    plist_t r = tss_request_send(req, "https://127.0.0.1:1/TSS/controller?action=2");
    CHECK(r == NULL, "a request to a closed port fails");
    CHECK(g_calls == 2 && g_first_attempt == 1 && g_last_attempt == 2, "the hook ran once per attempt (2 attempts, numbered 1 and 2)");
    CHECK(g_rc == 7, "curl code is COULDNT_CONNECT (7)");
    CHECK(g_tried == 1 && g_connected == 0, "the stand-in tried one address and did not connect");
    CHECK(g_conn_errno == ECONNREFUSED, "connect errno is recorded (ECONNREFUSED)");
    CHECK(g_ms >= 0 && g_ms < 5000, "transfer time is a small number of milliseconds (%lld)", g_ms);
    CHECK(g_diag_has_attempt, "tss_last_diag() already describes the attempt when the hook runs");
    CHECK(g_url_idx == 0, "an address named by the caller is reported as url=0");
    tss_set_attempt_hook(NULL);
    g_calls = 0;
    r = tss_request_send(req, "https://127.0.0.1:1/TSS/controller?action=2");
    CHECK(r == NULL && g_calls == 0, "with no hook set nothing is called");
    printf(g_fail ? "TSSHOOK FAILED (%d)\n" : "TSSHOOK OK (%d failed)\n", g_fail);
    return g_fail != 0;
}
