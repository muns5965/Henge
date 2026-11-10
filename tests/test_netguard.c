/* host/net_guard.c: the default-deny connection policy, tested alone (policy table), against the real libimobiledevice-glue sockets (what the FDR proxy and
   libusbmuxd use), and against the curl stand-in. Linux. Nothing leaves the machine: every listener is on the loopback interface. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <curl/curl.h>
#include <libimobiledevice-glue/socket.h>

#include "../host/net_guard.h"

int (*hv_urlasset_guard)(const char *url) = NULL;   /* restore.c's variable; the test links without restore.c */

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static struct sockaddr_storage v4(const char *ip, int port) { struct sockaddr_storage s; memset(&s, 0, sizeof s); struct sockaddr_in *a = (void *)&s; a->sin_family = AF_INET; a->sin_port = htons(port); inet_pton(AF_INET, ip, &a->sin_addr); return s; }
static struct sockaddr_storage v6(const char *ip, int port) { struct sockaddr_storage s; memset(&s, 0, sizeof s); struct sockaddr_in6 *a = (void *)&s; a->sin6_family = AF_INET6; a->sin6_port = htons(port); inet_pton(AF_INET6, ip, &a->sin6_addr); return s; }
static int dec(struct sockaddr_storage s, const char *name) { return hv_net_guard_check((struct sockaddr *)&s, sizeof s, name, NULL, 0); }

static int listener(int *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET}; inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(s, (void *)&a, sizeof a); listen(s, 4);
    socklen_t l = sizeof a; getsockname(s, (void *)&a, &l); *port = ntohs(a.sin_port);
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}
static int pending(int ls) { usleep(150000); int c = accept(ls, NULL, NULL); if (c >= 0) { close(c); return 1; } return 0; }

int main(void) {
    hv_net_guard_reset();
    hv_net_guard_allow_local("127.0.0.1", 27123);
    hv_net_guard_allow_host("gs.apple.example", 443);

    /* 1. the policy table */
    CHECK(dec(v4("127.0.0.1", 27123), NULL) == HV_NET_ALLOW, "registered local endpoint is allowed");
    CHECK(dec(v4("127.0.0.1", 27124), NULL) == HV_NET_DENY, "same address, other port: refused (no port range)");
    CHECK(dec(v4("127.0.0.2", 27123), NULL) == HV_NET_DENY, "other loopback address, same port: refused");
    CHECK(dec(v4("127.0.0.1", 27123), "gs.apple.example") == HV_NET_ALLOW, "a name does not change the exact local rule");
    const char *priv[] = {"10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.1.1", "100.64.0.1", "0.0.0.0", "224.0.0.1", "255.255.255.255"};
    for (unsigned i = 0; i < sizeof priv / sizeof *priv; i++) CHECK(dec(v4(priv[i], 443), "gs.apple.example") == HV_NET_DENY, "%s refused even for an approved name", priv[i]);
    CHECK(dec(v4("172.32.0.1", 443), "gs.apple.example") == HV_NET_ALLOW, "172.32.0.1 is public (outside 172.16/12): approved name allowed");
    CHECK(dec(v4("8.8.8.8", 443), NULL) == HV_NET_DENY, "public address without a name (the glue callers): refused");
    CHECK(dec(v4("8.8.8.8", 443), "other.example") == HV_NET_DENY, "public address, name not on the list: refused");
    CHECK(dec(v4("8.8.8.8", 8443), "gs.apple.example") == HV_NET_DENY, "approved name, wrong port: refused");
    CHECK(dec(v4("8.8.8.8", 443), "GS.APPLE.EXAMPLE") == HV_NET_ALLOW, "approved name, public address, right port (case-insensitive): allowed");
    const char *p6[] = {"::1", "::", "fc00::1", "fd12::1", "fe80::1", "ff02::1", "::ffff:127.0.0.1", "::ffff:10.0.0.1"};
    for (unsigned i = 0; i < sizeof p6 / sizeof *p6; i++) CHECK(dec(v6(p6[i], 443), "gs.apple.example") == HV_NET_DENY, "%s refused", p6[i]);
    CHECK(dec(v6("2001:4860:4860::8888", 443), "gs.apple.example") == HV_NET_ALLOW, "public IPv6, approved name: allowed");
    CHECK(dec(v6("::ffff:8.8.8.8", 443), "gs.apple.example") == HV_NET_ALLOW, "IPv4-mapped public address is judged as IPv4: allowed");

    /* 2. the URLAsset decision */
    char why[96];
    CHECK(hv_net_guard_check_url("http://gs.apple.example/x", why, sizeof why) == HV_NET_DENY, "http URL refused (%s)", why);
    CHECK(hv_net_guard_check_url("https://user:pw@gs.apple.example/x", why, sizeof why) == HV_NET_DENY, "URL with user info refused (%s)", why);
    CHECK(hv_net_guard_check_url("https://other.example/x", why, sizeof why) == HV_NET_DENY, "host not on the list refused (%s)", why);
    CHECK(hv_net_guard_check_url("https://gs.apple.example:8443/x", why, sizeof why) == HV_NET_DENY, "listed host, unlisted port refused (%s)", why);
    CHECK(hv_net_guard_check_url("https://[::1]/x", why, sizeof why) == HV_NET_DENY, "IPv6 literal refused (%s)", why);
    CHECK(hv_net_guard_check_url("https://127.0.0.1/x", why, sizeof why) == HV_NET_DENY, "address literal not on the list refused (%s)", why);
    CHECK(hv_net_guard_check_url("https://gs.apple.example/x?token=SECRET#f", why, sizeof why) == HV_NET_ALLOW, "listed host, default port, with a query: allowed");
    CHECK(hv_net_guard_check_url("HTTPS://GS.Apple.Example:443/x", why, sizeof why) == HV_NET_ALLOW, "scheme, host and explicit default port, any case: allowed");
    hv_net_guard_check_url("https://other.example/path?secret=SECRET", NULL, 0);
    char lg[800]; hv_net_guard_denied_log(lg, sizeof lg);
    CHECK(!strstr(lg, "SECRET") && !strstr(lg, "/path") && !strstr(lg, "pw"), "the refusal log holds host and port only (no path, query or user info)");

    /* 3. the real glue sockets */
    hv_net_guard_reset();
    int pa, pb;
    int la = listener(&pa), lb = listener(&pb);
    hv_net_guard_allow_local("127.0.0.1", (uint16_t)pa);
    int s0 = socket_connect("127.0.0.1", (uint16_t)pb);
    CHECK(s0 >= 0 && pending(lb), "before install: nothing changes, a connection to B succeeds");
    if (s0 >= 0) close(s0);
    hv_net_guard_install();
    int s1 = socket_connect("127.0.0.1", (uint16_t)pa);
    CHECK(s1 >= 0 && pending(la), "installed: the registered endpoint A is reachable (what libusbmuxd needs)");
    if (s1 >= 0) close(s1);
    errno = 0;
    int s2 = socket_connect("127.0.0.1", (uint16_t)pb);
    CHECK(s2 < 0 && !pending(lb), "installed: another loopback port B is refused and its server saw no connection");
    int s3 = socket_connect("localhost", (uint16_t)pb);
    CHECK(s3 < 0 && !pending(lb), "installed: the name localhost resolves to loopback and is refused for B (the resolved address is what is checked)");
    struct sockaddr_in ab = {.sin_family = AF_INET}; inet_pton(AF_INET, "127.0.0.1", &ab.sin_addr);
    errno = 0;
    int s4 = socket_connect_addr((struct sockaddr *)&ab, (uint16_t)pb);
    CHECK(s4 < 0 && errno == EACCES && !pending(lb), "installed: socket_connect_addr to B is refused with EACCES");
    char up[] = "/tmp/hvng-XXXXXX"; int ufd = mkstemp(up); close(ufd); unlink(up);
    int us = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un ua = {.sun_family = AF_UNIX}; strncpy(ua.sun_path, up, sizeof ua.sun_path - 1);
    bind(us, (void *)&ua, sizeof ua); listen(us, 2);
    int s5 = socket_connect_unix(up);
    CHECK(s5 < 0, "installed: an unregistered unix socket is refused");
    hv_net_guard_allow_unix(up);
    int s6 = socket_connect_unix(up);
    CHECK(s6 >= 0, "installed: the registered unix socket path is allowed");
    if (s6 >= 0) close(s6);
    close(us); unlink(up);

    /* 4. the curl stand-in */
    CURL *c = curl_easy_init();
    char url[64]; snprintf(url, sizeof url, "http://127.0.0.1:%d/", pb);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    CHECK(rc != CURLE_OK && !pending(lb), "installed: a curl request to loopback port B is refused before connect() (rc %d), its server saw no connection", (int)rc);

    HvNetStats st; hv_net_guard_stats(&st);
    CHECK(st.denied >= 5 && st.allowed >= 2, "counters: %d attempts, %d allowed, %d refused", st.attempts, st.allowed, st.denied);
    hv_net_guard_uninstall();
    int s7 = socket_connect("127.0.0.1", (uint16_t)pb);
    CHECK(s7 >= 0 && pending(lb), "after uninstall: the libraries behave as before");
    if (s7 >= 0) close(s7);

    printf("%s (%d failed)\n", g_fail ? "NETGUARD FAILED" : "NETGUARD OK", g_fail);
    return g_fail ? 1 : 0;
}
