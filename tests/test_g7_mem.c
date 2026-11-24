/* host/g7_mem.h: the memory line of the G7 watchdog, from synthetic procfs text. Nothing is run on a device, nothing is connected. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "../host/g7_mem.h"

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static const char STATUS[] =
    "Name:\thengevm\nUmask:\t0077\nState:\tS (sleeping)\nTgid:\t4242\nVmPeak:\t 9999999 kB\nVmSize:\t 9000000 kB\nVmHWM:\t 3145728 kB\nVmRSS:\t 2097152 kB\n"
    "RssAnon:\t 1572864 kB\nRssFile:\t  524288 kB\nRssShmem:\t   65536 kB\nVmSwap:\t  102400 kB\nThreads:\t 12\n";
static const char MEMINFO[] = "MemTotal:       11800000 kB\nMemFree:          200000 kB\nMemAvailable:    1048576 kB\nBuffers:           10000 kB\nCached:          3145728 kB\nDirty:             20480 kB\nWriteback:          2048 kB\nSwapFree:         524288 kB\n";

int main(void) {
    HvG7Mem m; char l[400];
    hv_g7m_parse(&m, STATUS, MEMINFO);
    CHECK(m.rss_kb == 2097152 && m.peak_kb == 3145728 && m.anon_kb == 1572864 && m.file_kb == 524288 && m.swap_kb == 102400 && m.avail_kb == 1048576, "all six values are read (kB)");
    hv_g7m_line(l, sizeof l, 943, &m);
    CHECK(!strcmp(l, "g7-mem: t=943s rss=2048MiB peak=3072MiB anon=1536MiB file=512MiB shmem=64MiB swap=100MiB available=1024MiB cached=3072MiB dirty=20MiB writeback=2MiB swapfree=512MiB\n"), "the line has the fixed shape, in MiB: %s", l);
    CHECK(strlen(l) < 200 && l[strlen(l) - 1] == '\n' && strchr(l, '\n') == l + strlen(l) - 1, "one line that ends in a single newline, short enough for one write");

    /* a key only matches at the start of a line: VmRSS must not be found inside VmRSSX or in the middle of a line, "Rss" is not "RssAnon" */
    HvG7Mem n; hv_g7m_parse(&n, "xVmRSS:\t 5 kB\nVmRSSX:\t 6 kB\nRss:\t 7 kB\n", "XMemAvailable: 8 kB\n");
    CHECK(n.rss_kb == HV_G7M_UNKNOWN && n.anon_kb == HV_G7M_UNKNOWN && n.avail_kb == HV_G7M_UNKNOWN, "a key matches only a whole key at the start of a line");

    /* a kernel without RssAnon/RssFile/VmSwap (old procfs): unknown values print as ?, never as 0 */
    HvG7Mem o; hv_g7m_parse(&o, "VmHWM:\t 2048 kB\nVmRSS:\t 1024 kB\n", MEMINFO);
    hv_g7m_line(l, sizeof l, 30, &o);
    CHECK(strstr(l, "anon=? file=? shmem=? swap=?") && strstr(l, "rss=1MiB peak=2MiB") && strstr(l, "available=1024MiB"), "missing fields are printed as ?: %s", l);

    /* no text at all (a file that could not be read) */
    HvG7Mem z; hv_g7m_parse(&z, NULL, NULL);
    hv_g7m_line(l, sizeof l, 60, &z);
    CHECK(!strcmp(l, "g7-mem: t=60s rss=? peak=? anon=? file=? shmem=? swap=? available=? cached=? dirty=? writeback=? swapfree=?\n"), "unreadable files give all-unknown, not zeros: %s", l);

    /* a value that is a real zero stays 0MiB; a value without a number is unknown; a number that does not fit is unknown */
    HvG7Mem q; hv_g7m_parse(&q, "VmSwap:\t 0 kB\nVmRSS:\t kB\nVmHWM:\t 99999999999999999999999 kB\n", "");
    CHECK(q.swap_kb == 0 && q.rss_kb == HV_G7M_UNKNOWN && q.peak_kb == HV_G7M_UNKNOWN, "0 is a value, a missing or oversized number is unknown");
    hv_g7m_line(l, sizeof l, 90, &q);
    CHECK(strstr(l, "swap=0MiB") != NULL, "a real zero prints as 0MiB");

    /* a small buffer never overruns and still ends the string */
    char tiny[16]; memset(tiny, 'x', sizeof tiny); hv_g7m_line(tiny, sizeof tiny, 123456789, &m);
    CHECK(memchr(tiny, 0, sizeof tiny) != NULL, "a too small buffer is cut and terminated");

    /* the line contains only the fixed words and numbers: no path, no digit string that is not a number of the line */
    int bad = 0; for (const char *p = l; *p; p++) if (!(isalnum((unsigned char)*p) || *p == ' ' || *p == ':' || *p == '=' || *p == '-' || *p == '?' || *p == '\n')) bad++;
    CHECK(bad == 0, "only letters, digits and the fixed punctuation");

    /* the real procfs of the machine that runs the test (Linux): this process has a resident set, and the peak is not below it */
    HvG7Mem r; hv_g7m_sample(&r);
    CHECK(r.rss_kb != HV_G7M_UNKNOWN && r.rss_kb > 0 && r.peak_kb >= r.rss_kb, "the live sample reads this process (rss %llu kB, peak %llu kB)", (unsigned long long)r.rss_kb, (unsigned long long)r.peak_kb);
    CHECK(r.avail_kb != HV_G7M_UNKNOWN && r.avail_kb > 0, "MemAvailable is read from /proc/meminfo");
    char big[64]; CHECK(hv_g7m_read("/proc/this-file-does-not-exist", big, sizeof big) == -1 && big[0] == 0, "an unreadable file is reported and leaves an empty string");

    /* the stage line and the PSS line */
    HvG7Mem sm; hv_g7m_parse(&sm, STATUS, MEMINFO); char sl[200];
    hv_g7m_stage_line(sl, sizeof sl, "cpu_tables", &sm);
    CHECK(!strcmp(sl, "g7-mem-stage: cpu_tables rss=2048MiB anon=1536MiB shmem=64MiB available=1024MiB\n"), "the stage line has the fixed shape: %s", sl);
    char longname[200]; memset(longname, 'a', sizeof longname - 1); longname[sizeof longname - 1] = 0; hv_g7m_stage_line(sl, sizeof sl, longname, &sm);
    CHECK(strlen(sl) < 200 && sl[strlen(sl) - 1] == '\n', "a long stage name is cut, the line stays one line under 200 bytes");
    char pl[100]; hv_g7m_pss_line(pl, sizeof pl, 300, 3 * 1024 * 1024ull); CHECK(!strcmp(pl, "g7-mem-pss: t=300s pss=3072MiB\n"), "pss line: %s", pl);
    hv_g7m_pss_line(pl, sizeof pl, 300, HV_G7M_UNKNOWN); CHECK(!strcmp(pl, "g7-mem-pss: t=300s pss=?\n"), "an unreadable PSS is ?, not 0: %s", pl);
    uint64_t live = hv_g7m_pss_kb(); CHECK(live == HV_G7M_UNKNOWN || live > 0, "the live PSS is unknown or positive, never a fake 0");
    printf(g_fail ? "G7MEM FAILED (%d)\n" : "G7MEM OK\n", g_fail);
    return g_fail != 0;
}
