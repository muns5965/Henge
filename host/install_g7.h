#ifndef HENGEVM_INSTALL_G7_H
#define HENGEVM_INSTALL_G7_H
/* The G7 network allow-list for the restore (compiled only into the G7 restore stack: tools/restore-g7-build.sh builds host/install.c with -DHV_INSTALL_G7; no other build does).
   host/install.c's restore thread calls hv_g7_extra_flags() right after the restore session has begun (hv_restore_session_begin RESETS the lists, so anything registered before
   it would be wiped) and before the tool is entered (hv_restore_session_run_client). Only destinations that the SOURCE confirms are listed:
     gs.apple.com, gs.apple.com.akadns.net, gs.apple.com.v.aaplimg.com, port 443: the three https TSS hosts of third_party/libtatsu/src/tss.c (the plain-http ones are not built:
     HV_TSS_HTTPS_ONLY, and port 80 is not listed anyway), the same three G2..G6 already use.
     wkms-public.apple.com, port 443: the URLAsset host the device asked for in run g7_run_1795484190481 (refusal line `g7-net-deny: purpose=URL`), approved by the user on 2026-11-24 (docs/g7-new-destinations.md).
   NOT listed, on purpose, because their host names are chosen by the DEVICE at run time and no source names them (only what a run actually refused and the user approved is added, one exact host:port at a time): the URLAsset download (restore.c restore_send_url_asset), the
   image-decryption-key request (restore.c), an FDR target. They are refused by default; a refused named destination is written to the denied log (host name and port, never a URL),
   which the runner prints after the restore, so that an approved run can show exactly what was missing. No wildcard, no suffix match, no other port. */
#include <stddef.h>
#include <stdint.h>

typedef struct { const char *name; uint16_t port; } HvG7NetHost;
const HvG7NetHost *hv_g7_net_hosts(size_t *n);
/* Registers the list in net_guard (exact name + port). Returns the extra restore flags: none (0). A host that could not be registered stays unregistered, so its request is refused. */
int hv_g7_extra_flags(void);
/* 1 when every listed host was registered by the last hv_g7_extra_flags call. */
int hv_g7_net_registered_ok(void);
/* The gate install.c applies after hv_g7_extra_flags(): 0 = every listed host is registered, the tool may be entered; -1 = at least one is not, the tool must NOT be entered
   (the reason goes to stderr: names and ports only). The session is then closed and finalized on the ordinary path, the same as when "the connection policy is not in force". */
int hv_g7_net_gate(void);
#endif
