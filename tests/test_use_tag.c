/* libusbmuxd use_tag: many threads issue requests at once against a tiny listening server that records the TAG field of every request header (4 x uint32 LE:
   length, version, message, tag). Checks that no two requests carry the same tag (the server answers nothing and closes; the client call fails, which is irrelevant here).
   Under TSan the unprotected ++use_tag is reported as a data race. Usage: test_use_tag [threads] [calls per thread] */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <usbmuxd.h>

static int g_lfd, g_stop;
static uint32_t *g_tags; static size_t g_ntags, g_cap;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void *server(void *a) {
    (void)a;
    while (!__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) {
        int c = accept(g_lfd, NULL, NULL); if (c < 0) break;
        uint32_t h[4]; size_t got = 0;
        while (got < sizeof h) { ssize_t n = recv(c, (char *)h + got, sizeof h - got, 0); if (n <= 0) break; got += (size_t)n; }
        if (got == sizeof h) { pthread_mutex_lock(&g_mu); if (g_ntags < g_cap) g_tags[g_ntags++] = h[3]; pthread_mutex_unlock(&g_mu); }
        close(c);
    }
    return NULL;
}
static int g_calls;
static void *client(void *a) {
    (void)a;
    for (int i = 0; i < g_calls; i++) { usbmuxd_device_info_t *l = NULL; int n = usbmuxd_get_device_list(&l); if (n > 0 && l) free(l); }
    return NULL;
}
static int cmp(const void *x, const void *y) { uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y; return a < b ? -1 : a > b; }
int main(int argc, char **argv) {
    int nt = argc > 1 ? atoi(argv[1]) : 8; g_calls = argc > 2 ? atoi(argv[2]) : 100;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    g_lfd = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(g_lfd, (struct sockaddr *)&sa, sizeof sa) || listen(g_lfd, 256)) { perror("listen"); return 2; }
    socklen_t sl = sizeof sa; getsockname(g_lfd, (struct sockaddr *)&sa, &sl);
    char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%d", ntohs(sa.sin_port)); setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);
    g_cap = (size_t)nt * g_calls * 2 + 16; g_tags = calloc(g_cap, sizeof *g_tags);
    pthread_t sv; pthread_create(&sv, NULL, server, NULL);
    pthread_t th[64]; if (nt > 64) nt = 64;
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, client, NULL);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    usleep(300000); __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST); shutdown(g_lfd, SHUT_RDWR); close(g_lfd); pthread_join(sv, NULL);
    qsort(g_tags, g_ntags, sizeof *g_tags, cmp);
    size_t dup = 0; for (size_t i = 1; i < g_ntags; i++) if (g_tags[i] == g_tags[i - 1]) dup++;
    printf("TAGS requests_seen=%zu expected=%d duplicates=%zu min=%u max=%u\n", g_ntags, nt * g_calls, dup, g_ntags ? g_tags[0] : 0, g_ntags ? g_tags[g_ntags - 1] : 0);
    printf("%s unique tags for concurrent requests\n", dup == 0 && g_ntags > 0 ? "PASS" : "FAIL");
    return dup == 0 && g_ntags > 0 ? 0 : 1;
}
