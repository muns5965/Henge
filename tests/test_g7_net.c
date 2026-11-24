/* The G7 network allow-list (host/install_g7.c) against host/net_guard.c. Pure policy decisions: nothing is connected, nothing leaves the machine. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "../host/install_g7.h"
#include "../host/net_guard.h"

int (*hv_urlasset_guard)(const char *url) = NULL;

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static struct sockaddr_storage v4(const char *ip, int port) { struct sockaddr_storage s; memset(&s, 0, sizeof s); struct sockaddr_in *a = (void *)&s; a->sin_family = AF_INET; a->sin_port = htons(port); inet_pton(AF_INET, ip, &a->sin_addr); return s; }
static int http(const char *ip, int port, const char *name) { struct sockaddr_storage s = v4(ip, port); return hv_net_guard_check_p((struct sockaddr *)&s, sizeof s, name, HV_NET_P_HTTP, NULL, 0); }
static int url(const char *u) { return hv_net_guard_check_url(u, NULL, 0); }
static void fresh(void) { hv_net_guard_reset(); hv_g7_extra_flags(); }

int main(void) {
    const char *ip = "17.253.1.1";   /* any public address; the policy keys on the NAME, the address is only checked for being public */
    size_t n = 0; const HvG7NetHost *h = hv_g7_net_hosts(&n);
    CHECK(n == 5, "exactly five hosts listed (%zu)", n);
    int wild = 0, port_ok = 1;
    for (size_t i = 0; i < n; i++) { if (strchr(h[i].name, '*') || h[i].name[0] == '.') wild++; if (h[i].port != 443) port_ok = 0; }
    CHECK(!wild && port_ok, "no wildcard / suffix entry, every port 443");
    CHECK(!strcmp(h[0].name, "gs.apple.com") && !strcmp(h[1].name, "gs.apple.com.akadns.net") && !strcmp(h[2].name, "gs.apple.com.v.aaplimg.com"), "the three TSS hosts, exact names");
    CHECK(!strcmp(h[3].name, "wkms-public.apple.com") && h[3].port == 443, "the approved URLAsset host, exact name, port 443");
    CHECK(!strcmp(h[4].name, "fcs-keys-pub-prod.cdn-apple.com") && h[4].port == 443, "the approved URLAsset redirect host, exact name, port 443");

    fresh();
    CHECK(hv_g7_extra_flags() == 0 && hv_g7_net_registered_ok(), "extra flags 0; registration ok (and idempotent)");
    for (size_t i = 0; i < n; i++) CHECK(http(ip, 443, h[i].name) == HV_NET_ALLOW, "allowed: %s:443", h[i].name);
    CHECK(http(ip, 443, "GS.Apple.COM") == HV_NET_ALLOW, "host name match is case-insensitive");
    CHECK(http(ip, 80, "gs.apple.com") == HV_NET_DENY, "wrong port 80 denied");
    CHECK(http(ip, 8443, "gs.apple.com") == HV_NET_DENY, "wrong port 8443 denied");
    CHECK(http(ip, 443, "updates.cdn-apple.com") == HV_NET_DENY, "unregistered host denied");
    CHECK(http(ip, 443, "evil.gs.apple.com") == HV_NET_DENY, "sub-domain of a listed host denied");
    CHECK(http(ip, 443, "apple.com") == HV_NET_DENY, "parent domain denied");
    CHECK(http(ip, 443, "gs.apple.com.evil.com") == HV_NET_DENY, "look-alike suffix denied");
    CHECK(http(ip, 80, "wkms-public.apple.com") == HV_NET_DENY && http(ip, 8443, "wkms-public.apple.com") == HV_NET_DENY, "wkms-public.apple.com: other ports denied");
    CHECK(http(ip, 443, "wkms.sd.apple.com") == HV_NET_DENY && http(ip, 443, "evil.wkms-public.apple.com") == HV_NET_DENY && http(ip, 443, "wkms-public.apple.com.evil.com") == HV_NET_DENY && http(ip, 443, "apple.com") == HV_NET_DENY, "wkms-public.apple.com: sibling, sub-domain, look-alike and parent denied");
    CHECK(http(ip, 443, "fcs-keys-pub-prod.cdn-apple.com") == HV_NET_ALLOW && http(ip, 80, "fcs-keys-pub-prod.cdn-apple.com") == HV_NET_DENY && http(ip, 8443, "fcs-keys-pub-prod.cdn-apple.com") == HV_NET_DENY, "fcs-keys-pub-prod.cdn-apple.com: 443 only");
    CHECK(http(ip, 443, "cdn-apple.com") == HV_NET_DENY && http(ip, 443, "evil.fcs-keys-pub-prod.cdn-apple.com") == HV_NET_DENY && http(ip, 443, "fcs-keys-pub-prod.cdn-apple.com.evil.com") == HV_NET_DENY && http(ip, 443, "fcs-keys-pub-prod.cdn-apple.net") == HV_NET_DENY && http(ip, 443, "updates.cdn-apple.com") == HV_NET_DENY, "fcs-keys-pub-prod.cdn-apple.com: parent, sub-domain, look-alikes and other cdn-apple hosts denied");
    CHECK(http(ip, 443, "gs.apple.com.") == HV_NET_DENY, "trailing-dot form not silently accepted");
    CHECK(http(ip, 443, NULL) == HV_NET_DENY, "bare address (no name) denied");
    CHECK(http("127.0.0.1", 443, "gs.apple.com") == HV_NET_DENY, "loopback with an allowed name denied");
    CHECK(http("10.0.0.5", 443, "gs.apple.com") == HV_NET_DENY, "private address with an allowed name denied");
    { struct sockaddr_storage s = v4(ip, 443); CHECK(hv_net_guard_check_p((struct sockaddr *)&s, sizeof s, NULL, HV_NET_P_FDR, NULL, 0) == HV_NET_DENY, "FDR (the glue hook passes no name): public address denied"); }

    /* URLAsset / key-request check */
    CHECK(url("https://gs.apple.com/TSS/controller?action=2") == HV_NET_ALLOW, "https URL on a listed host allowed");
    CHECK(url("https://wkms-public.apple.com/x") == HV_NET_ALLOW && url("http://wkms-public.apple.com/x") == HV_NET_DENY && url("https://wkms-public.apple.com:8443/x") == HV_NET_DENY && url("https://wkms.sd.apple.com/keys/fetch") == HV_NET_DENY, "URLAsset check: the approved host over https:443 only; http, other port and the key host stay refused");
    CHECK(url("https://fcs-keys-pub-prod.cdn-apple.com/fcs-keys/x") == HV_NET_ALLOW && url("http://fcs-keys-pub-prod.cdn-apple.com/x") == HV_NET_DENY && url("https://fcs-keys-pub-prod.cdn-apple.com:8443/x") == HV_NET_DENY, "URLAsset redirect hop: the approved CDN host over https:443 only");
    CHECK(url("http://gs.apple.com/x") == HV_NET_DENY, "http URL denied");
    CHECK(url("https://user:pw@gs.apple.com/x") == HV_NET_DENY, "user info denied");
    CHECK(url("https://updates.cdn-apple.com/x") == HV_NET_DENY, "unlisted URL host denied");
    CHECK(url("https://gs.apple.com:8443/x") == HV_NET_DENY, "listed host, other port denied");
    CHECK(url("https://*.apple.com/x") == HV_NET_DENY, "wildcard-looking host denied");
    /* redirect: each hop is checked on its own */
    CHECK(url("https://gs.apple.com/a") == HV_NET_ALLOW && url("https://cdn.example.net/b") == HV_NET_DENY, "redirect hop to an unlisted host denied");
    CHECK(url("https://gs.apple.com/a") == HV_NET_ALLOW && url("http://gs.apple.com/b") == HV_NET_DENY, "redirect hop downgraded to http denied");

    /* recording: a refused named destination is logged as host/ip:port + reason, never a URL or path */
    hv_net_guard_reset(); hv_g7_extra_flags();
    http(ip, 443, "updates.cdn-apple.com");
    http(ip, 80, "gs.apple.com");
    url("https://secret.example.org/path/with?token=abc");
    char log[1024]; hv_net_guard_denied_log(log, sizeof log);
    CHECK(strstr(log, "updates.cdn-apple.com") && strstr(log, ":443"), "unknown destination recorded with host name and port");
    CHECK(strstr(log, "gs.apple.com") && strstr(log, ":80"), "wrong-port attempt recorded");
    CHECK(!strstr(log, "token") && !strstr(log, "/path") && !strstr(log, "?"), "no path / query in the record");
    HvNetStats st; hv_net_guard_stats(&st);
    CHECK(st.denied >= 2, "denied counter increments (%d)", st.denied);

    /* reset: the policy does NOT survive it by itself; registering again restores it */
    hv_net_guard_reset();
    CHECK(http(ip, 443, "gs.apple.com") == HV_NET_DENY, "after reset without registration: denied (default deny)");
    hv_g7_extra_flags();
    CHECK(http(ip, 443, "gs.apple.com") == HV_NET_ALLOW, "after reset + registration: allowed again");
    CHECK(http(ip, 443, "updates.cdn-apple.com") == HV_NET_DENY, "and still nothing else");

    /* capacity: the table fits the guard's list (a failed registration must be visible) */
    hv_net_guard_reset();
    for (int i = 0; i < 8; i++) { char b[32]; snprintf(b, sizeof b, "fill%d.example", i); hv_net_guard_allow_host(b, 443); }
    hv_g7_extra_flags();
    CHECK(!hv_g7_net_registered_ok(), "full list: registration reported as failed, host stays refused");
    CHECK(http(ip, 443, "gs.apple.com") == HV_NET_DENY, "unregistered because full: denied");

    /* live refusal lines (opt-in): reported when refused, bounded, names/ports/purpose/reason only, decisions unchanged */
    {
        char path[] = "/tmp/g7netliveXXXXXX"; int tf = mkstemp(path); int saved = dup(2);
        fresh();
        CHECK(http(ip, 443, "wkms.sd.apple.com") == HV_NET_DENY, "(live off) a refusal prints nothing");
        fflush(stderr); dup2(tf, 2);
        hv_net_guard_live_log(1);
        fresh();   /* the switch survives a reset */
        for (int i = 0; i < 25; i++) http(ip, 443, "wkms.sd.apple.com");   /* 1st, 10th reported */
        CHECK(url("https://secret.example/path/token?ecid=1234567890#x") == HV_NET_DENY, "(live on) a URL refusal is still a refusal");
        CHECK(http(ip, 443, "gs.apple.com") == HV_NET_ALLOW, "(live on) an allowed destination is still allowed and prints nothing");
        for (int i = 0; i < 40; i++) { char b[32]; snprintf(b, sizeof b, "h%d.example", i); http(ip, 443, b); }   /* more than 24 different destinations */
        hv_net_guard_live_log(0);
        http(ip, 443, "after-off.example");
        fflush(stderr); dup2(saved, 2); close(saved); close(tf);
        char buf[16384] = ""; FILE *f = fopen(path, "r"); size_t got = f ? fread(buf, 1, sizeof buf - 1, f) : 0; buf[got] = 0; if (f) fclose(f); remove(path);
        int lines = 0; for (char *q = buf; *q; q++) if (*q == '\n') lines++;
        CHECK(strstr(buf, "g7-net-deny: purpose=HTTP host=wkms.sd.apple.com port=443 reason=host not on the allow list count=1\n") != NULL, "first refusal printed with purpose, host, port, reason");
        CHECK(strstr(buf, "host=wkms.sd.apple.com port=443 reason=host not on the allow list count=10\n") != NULL && !strstr(buf, "count=2\n") && !strstr(buf, "count=25\n"), "repeats are counted: printed again only at 10 (25 refusals gave 2 lines)");
        CHECK(strstr(buf, "purpose=URL host=secret.example port=443 reason=host not on the allow list") != NULL, "a URL refusal prints host and port only");
        CHECK(!strstr(buf, "path") && !strstr(buf, "token") && !strstr(buf, "1234567890") && !strstr(buf, "ecid"), "no URL path, query, token or identifier in any line");
        CHECK(!strstr(buf, "gs.apple.com port") && !strstr(buf, "after-off"), "nothing for an allowed destination or after switching it off");
        CHECK(strstr(buf, "more destinations refused than are listed") != NULL && lines <= 24 + 6, "more than 24 different destinations: one 'more' line, output stays bounded (%d lines)", lines);
    }

    printf(g_fail ? "G7NET FAILED (%d)\n" : "G7NET OK (%d failed)\n", g_fail);
    return g_fail != 0;
}
