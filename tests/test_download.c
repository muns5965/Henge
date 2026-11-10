/* The real idevicerestore download.c (download_to_file / download_to_buffer, the IPSW and firmware download paths) on top of host/curl_mbedtls.c.
   usage: test_download file URL OUT [quit-after-ms [register-flag]] | buffer URL
   Prints: RESULT <return value> SIZE <bytes of OUT or of the buffer> */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "download.h"
#include "curl/curl.h"

/* what download.c takes from the rest of idevicerestore */
int global_quit_flag = 0;
int log_level = 0;
void logger(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void register_progress(uint32_t tag, const char *label) { (void)tag, (void)label; }
void set_progress(uint32_t tag, double p) { (void)tag, (void)p; }
void finalize_progress(uint32_t tag) { (void)tag; }

static void *quitter(void *arg) { usleep((useconds_t)(long)arg * 1000); global_quit_flag = 1; return NULL; }

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "buffer")) {
        void *buf = NULL;
        size_t len = 0;
        int r = download_to_buffer(argv[2], &buf, &len);
        printf("RESULT %d SIZE %zu\n", r, len);
        free(buf);
        return 0;
    }
    if (argc < 4) return 2;
    if (getenv("HV_TEST_FSIZE")) { /* the disk fills up: writes beyond this many bytes fail with EFBIG */
        struct rlimit rl = {(rlim_t)atoll(getenv("HV_TEST_FSIZE")), (rlim_t)atoll(getenv("HV_TEST_FSIZE"))};
        signal(SIGXFSZ, SIG_IGN);
        setrlimit(RLIMIT_FSIZE, &rl);
    }
    if (argc > 4) {
        pthread_t t;
        pthread_create(&t, NULL, quitter, (void *)atol(argv[4]));
        if (argc > 5) hv_curl_set_cancel_flag((volatile int *)&global_quit_flag);
    }
    int r = download_to_file(argv[2], argv[3], 1);
    struct stat st;
    long long sz = stat(argv[3], &st) == 0 ? (long long)st.st_size : -1;
    printf("RESULT %d SIZE %lld\n", r, sz);
    return 0;
}
