#define _GNU_SOURCE 1
#include "tss_stage.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "curl/curl.h"
#include "libtatsu/tss.h"

#ifndef HV_TSS_HTTPS_ONLY
#error "tss_stage.c needs libtatsu built with HV_TSS_HTTPS_ONLY"
#endif
const char *tss_last_diag(void);

const char *hv_tss_code_name(HvTssCode c) {
    static const char *const n[] = {"OK", "POLICY_DENIED", "NET_DNS", "NET_CONNECT", "NET_TIMEOUT", "NET_IO", "TLS_FAILED", "HTTP_ERROR", "SERVER_REFUSED", "BAD_REPLY",
                                    "REPLY_TOO_LARGE", "CANCELLED", "NOT_SENT", "TRANSPORT_OTHER"};
    return (unsigned)c < sizeof n / sizeof *n ? n[c] : "?";
}

static int num_after(const char *s, const char *key, int *v) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    p += strlen(key);
    char *end;
    long l = strtol(p, &end, 10);
    if (end == p) return 0;
    *v = (int)l;
    return 1;
}

static int tls_code(int c) {
    switch (c) {
    case CURLE_SSL_CONNECT_ERROR: case CURLE_PEER_FAILED_VERIFICATION: case CURLE_SSL_CACERT_BADFILE:
    case 51: case 53: case 54: case 58: case 59: case 64: case 66: case 80: case 83: case 90: case 91: return 1;
    default: return 0;
    }
}

/* The stage the request reached decides the class: a transport code means no complete reply came back (even if a status line was seen); a complete HTTP reply with
   an error status is an HTTP error whatever its body says; only then does the TSS STATUS count. */
void hv_tss_classify(const char *diag, const HvCurlLast *last, int got_plist, HvTssResult *o) {
    memset(o, 0, sizeof *o);
    o->tss_status = -1;
    if (last) { o->http = last->http_status; o->addrs_denied = last->addrs_denied; o->addrs_tried = last->addrs_tried; o->body_to_lib = last->body_to_cb; o->max_buf = last->max_buf; }
    int v;
    if (diag && num_after(diag, "TSS_ATTEMPTS: ", &v)) o->attempts = v;
    if (got_plist) { o->code = HV_TSS_OK; if (diag && num_after(diag, "HTTP: ", &v)) o->http = v; o->tss_status = 0; return; }
    if (!diag || !diag[0]) { o->code = HV_TSS_NOT_SENT; return; }
    int curl = 0;
    if (strstr(diag, "TRANSPORT: curl ") && num_after(diag, "TRANSPORT: curl ", &curl)) {
        o->curl_code = curl;
        if (curl == CURLE_UNSUPPORTED_PROTOCOL) o->code = (last && last->policy_denied) ? HV_TSS_POLICY_DENIED : HV_TSS_TRANSPORT_OTHER;
        else if (curl == CURLE_FILESIZE_EXCEEDED) o->code = HV_TSS_REPLY_TOO_LARGE;
        else if (curl == CURLE_ABORTED_BY_CALLBACK) o->code = HV_TSS_CANCELLED;
        else if (curl == CURLE_OPERATION_TIMEDOUT) o->code = HV_TSS_NET_TIMEOUT;
        else if (curl == CURLE_COULDNT_RESOLVE_HOST) o->code = HV_TSS_NET_DNS;
        else if (curl == CURLE_COULDNT_CONNECT) o->code = HV_TSS_NET_CONNECT;
        else if (tls_code(curl)) o->code = HV_TSS_TLS_FAILED;
        else if (curl == CURLE_GOT_NOTHING || curl == CURLE_SEND_ERROR || curl == CURLE_RECV_ERROR) o->code = HV_TSS_NET_IO;
        else o->code = HV_TSS_TRANSPORT_OTHER;
        return;
    }
    int http = 0;
    if (num_after(diag, "HTTP: ", &http)) o->http = http;
    if (strstr(diag, "SUCCESS_BUT_NOT_A_PLIST")) { o->code = HV_TSS_BAD_REPLY; return; }
    if (http >= 300) { o->code = HV_TSS_HTTP_ERROR; num_after(diag, "TSS_STATUS: ", &o->tss_status); return; }
    if (strstr(diag, "NOT_A_TSS_REPLY")) { o->code = HV_TSS_BAD_REPLY; return; }
    if (strstr(diag, "TSS_STATUS: ")) {
        if (num_after(diag, "TSS_STATUS: ", &v)) o->tss_status = v;
        o->code = HV_TSS_SERVER_REFUSED;
        return;
    }
    o->code = HV_TSS_BAD_REPLY;
}

plist_t hv_tss_send(plist_t request, const char *url, HvTssResult *out) {
    hv_curl_set_body_cap(HV_TSS_BODY_CAP);
    plist_t r = tss_request_send(request, url);
    hv_curl_set_body_cap(0);
    HvCurlLast last;
    hv_curl_last(&last);
    hv_tss_classify(tss_last_diag(), &last, r != NULL, out);
    return r;
}

int hv_tss_run_requests(plist_t *reqs, int n, const char *url, HvTssResult *results, int *stopped_at) {
    int sent = 0;
    if (stopped_at) *stopped_at = -1;
    for (int i = 0; i < n; i++) {
        plist_t r = hv_tss_send(reqs[i], url, &results[i]);
        sent++;
        if (r) plist_free(r);
        if (results[i].code != HV_TSS_OK) { if (stopped_at) *stopped_at = i; break; }
    }
    return sent;
}
