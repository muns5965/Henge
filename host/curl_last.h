#ifndef HENGEVM_CURL_LAST_H
#define HENGEVM_CURL_LAST_H
/* What the curl stand-in (curl_mbedtls.c) saw during the LAST curl_easy_perform on the calling thread. The record is cleared at the start of every perform, so
   it describes that request only (all its redirect hops), never an earlier one, and it is read on the same thread right after the call returns.
   It exists because one return code cannot say everything: a connection refused by the policy hook is returned as CURLE_UNSUPPORTED_PROTOCOL (libtatsu does not
   retry that code), and policy_denied tells it apart from a real unsupported protocol. */
typedef struct {
    int policy_denied;      /* the request ended because EVERY address of a hop was refused by the connect hook (so no connect() was made for that hop) */
    int addrs_denied;       /* addresses refused by the hook during this request */
    int addrs_tried;        /* addresses the hook allowed, i.e. connect() was called */
    int connected;          /* a TCP connection was established */
    int tls_done;           /* the TLS handshake completed (certificate verified) */
    int request_sent;       /* the request, body included, was written to the connection */
    int http_status;        /* status line received (0 = none) */
    int too_large;          /* stopped by the body or header size cap */
    long long body_to_cb;   /* body bytes handed to the write callback */
    size_t max_buf;         /* largest capacity any internal line/request buffer reached */
} HvCurlLast;

void hv_curl_last(HvCurlLast *out);
/* Thread-local cap on the DECODED response body (after chunked framing is removed; this client does no content decompression). 0 = none (the default). Header
   bytes have their own fixed cap. An over-long body ends the request with CURLE_FILESIZE_EXCEEDED before the bytes reach the write callback. */
void hv_curl_set_body_cap(long long bytes);

#endif
