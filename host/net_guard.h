#ifndef HENGEVM_NET_GUARD_H
#define HENGEVM_NET_GUARD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Default-deny policy for the connections the restore tool makes. Three places ask it, all before the connection exists:
     - libimobiledevice-glue's socket_connect / socket_connect_addr / socket_connect_unix (limd_glue_connect_guard): the FDR proxy, libusbmuxd's own link to the private
       usbmuxd, and any other user of that library. These calls see only the ADDRESS that is about to be connected (after name resolution).
     - the curl stand-in (hv_curl_connect_guard in curl_mbedtls.c): TSS and the URLAsset download. These also know the host NAME.
     - restore.c's URLAsset step (hv_urlasset_guard), which sees the whole URL before anything is opened.
   Rules, in this order:
     1. an exact endpoint registered with hv_net_guard_allow_local() (the private usbmuxd's listener) is allowed, for the DEFAULT purpose only (below). The match is by
        ADDRESS AND PORT; it does not know who the caller is, which is why FDR (device-chosen target) and curl are given other purposes;
     2. any other loopback, private, link-local, unspecified, multicast or unique-local address is refused, whatever the name or the allow list;
     3. a public address is allowed only when the caller gave a host NAME and (name, port) is on the list given to hv_net_guard_allow_host(); a bare address (the glue
        callers) is refused.
   The list starts EMPTY, so nothing outside the registered local endpoint is reachable until a caller adds an entry. Nothing here is a TLS or protocol check; FDR is
   plain TCP and the TLS rules of the curl stand-in are its own.
   What is logged is the numeric address and port and the reason, never a URL, a query string or an identifier. */

enum { HV_NET_ALLOW = 0, HV_NET_DENY = 1 };

/* Turn the guard on or off for all three hooks. Off (the default) leaves the libraries as they were. */
void hv_net_guard_install(void);
/* Refused (-1, the hooks stay) while an engine thread is registered: removing the hooks would turn "refuse" into "allow". 0 when removed. */
int hv_net_guard_uninstall(void);
int hv_net_guard_active(void);
int hv_net_guard_hooks_intact(void);   /* active, and all three hooks are this module's own */
/* An engine thread (anything that runs the restore tool) registers for as long as it runs. enter fails (-1) unless the hooks are intact. */
int hv_net_guard_engine_enter(void);
void hv_net_guard_engine_leave(void);
int hv_net_guard_engine_count(void);

void hv_net_guard_reset(void);                                    /* empty the lists and the counters */
void hv_net_guard_allow_local(const char *ip, uint16_t port);      /* an exact loopback endpoint, e.g. the private usbmuxd's */
void hv_net_guard_allow_unix(const char *path);                    /* an exact AF_UNIX path */
void hv_net_guard_allow_host(const char *name, uint16_t port);     /* a public host by name (approved requests only) */

/* Why a connection is made. DEFAULT: the library's own link to the private usbmuxd (the registered local endpoints apply). FDR: a host:port the DEVICE named.
   HTTP: curl (TSS, URLAsset). For FDR and HTTP the registered local endpoints and unix paths are NOT consulted: they cannot reach the private usbmuxd. */
enum { HV_NET_P_DEFAULT = 0, HV_NET_P_FDR = 1, HV_NET_P_HTTP = 2 };

/* The decision, for tests and the hooks. name may be NULL. why (if not NULL) receives a short reason. len is checked against the family's structure size first. */
int hv_net_guard_check_p(const struct sockaddr *a, socklen_t len, const char *name, int purpose, char *why, size_t cap);
int hv_net_guard_check(const struct sockaddr *a, socklen_t len, const char *name, char *why, size_t cap);
/* The URLAsset decision: https only, no user info, host name on the allow list for that port. */
int hv_net_guard_check_url(const char *url, char *why, size_t cap);

typedef struct { int attempts, allowed, denied, url_attempts, url_denied; } HvNetStats;
void hv_net_guard_stats(HvNetStats *s);
/* The last refused attempts, newest last, as "ip:port reason" text, one per line (at most 8). */
size_t hv_net_guard_denied_log(char *out, size_t cap);

#endif
