#ifndef HENGEVM_TSS_STAGE_H
#define HENGEVM_TSS_STAGE_H
/* Stop B: sending a TSS request and saying, per request, how it ended. Built on libtatsu (HV_TSS_HTTPS_ONLY flavour: https only, scrubbed diagnosis) and the curl
   stand-in. Nothing here decides to send anything: the caller does, one request at a time, and the first request that does not succeed ends a sequence.
   The result carries numbers and fixed words only: never the request, the reply text, the server's MESSAGE, an ECID, a nonce or a ticket. */
#include <plist/plist.h>
#include "curl_last.h"

typedef enum {
    HV_TSS_OK = 0,
    HV_TSS_POLICY_DENIED,    /* every address was refused by the connection policy; no connect() was made */
    HV_TSS_NET_DNS,          /* name lookup failed */
    HV_TSS_NET_CONNECT,      /* a connection attempt that the policy allowed failed (also when other addresses were refused) */
    HV_TSS_NET_TIMEOUT,      /* a connect, read or total timeout */
    HV_TSS_NET_IO,           /* the connection broke while sending or receiving, or the server sent nothing */
    HV_TSS_TLS_FAILED,       /* TLS handshake or certificate check failed; the request was not sent */
    HV_TSS_HTTP_ERROR,       /* a complete HTTP reply with status >= 300 */
    HV_TSS_SERVER_REFUSED,   /* HTTP 2xx with a TSS STATUS other than 0 */
    HV_TSS_BAD_REPLY,        /* a reply that is not a TSS reply, or success text without a usable plist */
    HV_TSS_REPLY_TOO_LARGE,  /* body or headers over the cap; stopped before the bytes reached libtatsu */
    HV_TSS_CANCELLED,
    HV_TSS_NOT_SENT,         /* libtatsu refused before any attempt (e.g. a URL that is not https) */
    HV_TSS_TRANSPORT_OTHER   /* any other curl failure, including a real unsupported protocol */
} HvTssCode;

/* What each field covers. libtatsu may retry, and the curl stand-in's record describes only the LAST curl_easy_perform:
     attempts                          libtatsu's count of attempts for the whole request (from its diagnosis)
     tss_status                        the TSS STATUS in libtatsu's final diagnosis (-1 = none); not a count over attempts
     http, curl_code                   the last attempt (http: its status line, curl_code: its failure, 0 = the transport worked)
     addrs_denied, addrs_tried         connection statistics of the last curl run only
     body_to_lib, max_buf              body bytes handed to libtatsu and the largest internal buffer, last curl run only */
typedef struct {
    HvTssCode code;
    int attempts;            /* libtatsu's attempts, as observed (0 if unknown) */
    int http;                /* HTTP status of the last attempt, 0 if none */
    int tss_status;          /* TSS STATUS number when the reply had a plain one, else -1 */
    int curl_code;           /* curl code of the last attempt, 0 if the transport worked */
    int addrs_denied;        /* addresses the policy refused during the last attempt */
    int addrs_tried;         /* connect() calls made during the last attempt */
    long long body_to_lib;   /* body bytes handed to libtatsu in the last attempt */
    size_t max_buf;          /* largest internal buffer of the curl stand-in in the last attempt */
} HvTssResult;

#define HV_TSS_BODY_CAP (1024 * 1024)

const char *hv_tss_code_name(HvTssCode c);
/* Pure: from libtatsu's scrubbed diagnosis, the curl stand-in's record of the last attempt, and whether libtatsu returned a plist. */
void hv_tss_classify(const char *diag, const HvCurlLast *last, int got_plist, HvTssResult *out);
/* One request. Returns libtatsu's plist (caller frees) or NULL; *out always filled. */
plist_t hv_tss_send(plist_t request, const char *url, HvTssResult *out);
/* Requests in order; stops at the first one that is not OK. Returns how many were sent; *stopped_at = index of the failing request or -1. */
int hv_tss_run_requests(plist_t *reqs, int n, const char *url, HvTssResult *results, int *stopped_at);

#endif
