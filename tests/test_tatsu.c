/* Drives the real libtatsu (tss_request_send) through host/curl_mbedtls.c. usage: test_tatsu URL
   Prints "RESULT: null" or "RESULT: plist <keys>" so tests/test_tatsu.py can judge it. */
#include <plist/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtatsu/tss.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    tss_set_debug_level(1); /* libtatsu prints its errors only at level >= 1 */
    plist_t req = plist_new_dict();
    plist_dict_set_item(req, "@ApImg4Ticket", plist_new_bool(1));
    plist_dict_set_item(req, "ApECID", plist_new_uint(0x2d2a8d94f32ba4d2ULL));
    plist_dict_set_item(req, "ApChipID", plist_new_uint(0xFE00));
    plist_t resp = tss_request_send(req, argv[1]);
    if (!resp) { printf("RESULT: null\n"); return 1; }
    plist_dict_iter it = NULL;
    plist_dict_new_iter(resp, &it);
    printf("RESULT: plist");
    for (;;) {
        char *key = NULL;
        plist_t n = NULL;
        plist_dict_next_item(resp, it, &key, &n);
        if (!n) break;
        printf(" %s", key);
        free(key);
    }
    free(it);
    printf("\n");
    return 0;
}
