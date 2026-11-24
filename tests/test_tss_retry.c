/* libtatsu's retry timing (HV_TSS_HTTPS_ONLY flavour) with the real curl stand-in. Nothing leaves this machine: the address test runs in the stand-in's deny mode,
   the lookup test asks for a name that cannot exist (.invalid). The waits are shortened with tss_set_backoff_unit_ms (the app uses 1000 = whole seconds). */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <plist/plist.h>
#include "libtatsu/tss.h"

void tss_set_max_attempts(int n);
void tss_set_attempt_hook(void (*hook)(int, int, long, long long, int, long long));
void tss_set_backoff_unit_ms(long ms);
void tss_set_max_seconds(int s);
void tss_set_wait_abort(int (*fn)(void));
const char *tss_last_diag(void);
void hv_curl_deny_network(int on);

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

#define MAXN 8
static int n_calls, rcs[MAXN], urls[MAXN];
static long long since[MAXN];
static void hook(int attempt, int rc, long http, long long ms, int url_index, long long since_start) {
    (void)http; (void)ms;
    if (n_calls < MAXN) { rcs[n_calls] = rc; urls[n_calls] = url_index; since[n_calls] = since_start; }
    n_calls++; (void)attempt;
}
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void reset(void) { n_calls = 0; memset(rcs, 0, sizeof rcs); memset(urls, 0, sizeof urls); memset(since, 0, sizeof since);
    tss_set_max_seconds(150); tss_set_backoff_unit_ms(1000); tss_set_wait_abort(NULL); tss_set_max_attempts(6); }
static int abort_calls;
static int abort_after3(void) { return ++abort_calls >= 3; }

int main(void) {
    plist_t req = plist_new_dict();
    plist_dict_set_item(req, "@Test", plist_new_bool(1));
    tss_set_attempt_hook(hook);
    long long t0, wall;
    plist_t r;

    /* 1: no address named by the caller: every attempt uses the one built-in address (url 1), never the other two */
    reset(); tss_set_backoff_unit_ms(10); tss_set_max_attempts(5);
    hv_curl_deny_network(1);
    r = tss_request_send(req, NULL);
    hv_curl_deny_network(0);
    int same = n_calls == 5; for (int i = 0; i < n_calls && i < MAXN; i++) if (urls[i] != 1) same = 0;
    CHECK(r == NULL && same, "five attempts, all on built-in address 1 (calls=%d)", n_calls);

    /* 2: a failed lookup waits 2,4,8,16,30 units, and not after the last attempt */
    reset(); tss_set_backoff_unit_ms(20);
    t0 = now_ms();
    r = tss_request_send(req, "https://nonexistent.invalid/TSS/controller?action=2");
    wall = now_ms() - t0;
    int all6 = n_calls == 6; for (int i = 0; i < n_calls && i < MAXN; i++) if (rcs[i] != 6) all6 = 0;
    CHECK(r == NULL && all6, "six attempts, every one a failed lookup (curl 6; calls=%d)", n_calls);
    long long want[6] = {0, 40, 120, 280, 600, 1200};
    int timing = n_calls == 6;
    for (int i = 1; i < 6 && timing; i++) if (since[i] < want[i] - 10 || since[i] > want[i] + 250) timing = 0;
    CHECK(timing, "the starts of attempts 2..6 follow waits of 2,4,8,16,30 units (%lld %lld %lld %lld %lld ms)", since[1], since[2], since[3], since[4], since[5]);
    CHECK(n_calls == 6 && wall - since[5] < 150, "no wait after the last attempt (%lld ms after the last attempt's end)", wall - since[5]);

    /* 3: another retryable failure (curl 7 in deny mode) keeps the old 2-unit wait */
    reset(); tss_set_backoff_unit_ms(20); tss_set_max_attempts(3);
    hv_curl_deny_network(1);
    r = tss_request_send(req, NULL);
    hv_curl_deny_network(0);
    CHECK(r == NULL && n_calls == 3 && since[1] >= 30 && since[1] < 150 && since[2] >= 70 && since[2] < 250, "curl 7: constant 2-unit waits (%lld, %lld ms)", since[1], since[2]);

    /* 4: the whole request is bounded: with a 1 s limit the growing waits stop it before the sixth attempt */
    reset(); tss_set_backoff_unit_ms(100); tss_set_max_seconds(1);
    t0 = now_ms();
    r = tss_request_send(req, "https://nonexistent.invalid/TSS/controller?action=2");
    wall = now_ms() - t0;
    CHECK(r == NULL && n_calls == 3 && wall >= 950 && wall < 1800, "limit 1 s: gave up after %d attempts in %lld ms", n_calls, wall);

    /* 5: a cancel during a wait ends the request at once (the wait would have been 2 s) */
    reset(); abort_calls = 0; tss_set_wait_abort(abort_after3);
    t0 = now_ms();
    r = tss_request_send(req, "https://nonexistent.invalid/TSS/controller?action=2");
    wall = now_ms() - t0;
    CHECK(r == NULL && n_calls == 1 && wall < 1000 && strstr(tss_last_diag(), "STOPPED_WHILE_WAITING"), "cancel while waiting: one attempt, ended after %lld ms, diag says so", wall);

    printf(g_fail ? "TSSRETRY FAILED (%d)\n" : "TSSRETRY OK (%d failed)\n", g_fail);
    return g_fail != 0;
}
