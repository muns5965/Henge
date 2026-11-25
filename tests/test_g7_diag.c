/* host/g7_diag.h: the diagnostic lines of the G7 clone runner, from synthetic procfs text plus one live read of this test's own /proc. Nothing is run on a device. */
#include <stdio.h>
#include <string.h>
#include "../host/g7_diag.h"

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    const char vm[] = "nr_free_pages 123\npswpin 4000\npswpout 52000\npgmajfault 777\nxpswpin 9\n";
    CHECK(hv_g7d_vmstat_field(vm, "pswpin") == 4000 && hv_g7d_vmstat_field(vm, "pswpout") == 52000 && hv_g7d_vmstat_field(vm, "pgmajfault") == 777, "vmstat keys are read");
    CHECK(hv_g7d_vmstat_field(vm, "nope") == HV_G7D_UNKNOWN && hv_g7d_vmstat_field("", "pswpin") == HV_G7D_UNKNOWN && hv_g7d_vmstat_field(NULL, "pswpin") == HV_G7D_UNKNOWN, "a missing key or text is unknown, not 0");
    CHECK(hv_g7d_vmstat_field("xpswpin 5\n", "pswpin") == HV_G7D_UNKNOWN, "a key matches only at the start of a line");

    const char psi[] = "some avg10=1.25 avg60=0.50 avg300=0.10 total=123456\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=789\n";
    char a[16]; uint64_t t;
    CHECK(hv_g7d_psi(psi, "some", a, sizeof a, &t) == 0 && !strcmp(a, "1.25") && t == 123456, "psi some");
    CHECK(hv_g7d_psi(psi, "full", a, sizeof a, &t) == 0 && !strcmp(a, "0.00") && t == 789, "psi full");
    CHECK(hv_g7d_psi("", "some", a, sizeof a, &t) == -1 && !strcmp(a, "?") && t == HV_G7D_UNKNOWN, "no psi text: ? and unknown");
    CHECK(hv_g7d_psi("some avg10=abc total=5\n", "some", a, sizeof a, &t) == -1 && !strcmp(a, "?"), "a malformed avg10 is ?");

    HvG7DThr th;
    const char st[] = "4242 (vcpu 0) ) S 1 4242 4242 0 -1 4194304 1500 0 33 0 250 50 0 0 20 0 12 0 100 1000 1000 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 3 0 0 0 0 0\n";
    CHECK(hv_g7d_parse_thread_stat(st, &th) == 0 && th.tid == 4242 && th.minflt == 1500 && th.majflt == 33 && th.utime == 250 && th.stime == 50, "thread stat fields (a comm with a space and a ')' is handled)");
    CHECK(!strchr(th.comm, ' ') && !strchr(th.comm, '|'), "the thread name has no space and no separator: %s", th.comm);
    CHECK(hv_g7d_parse_thread_stat("garbage", &th) == -1 && hv_g7d_parse_thread_stat("1 (x) S 1 2", &th) == -1 && hv_g7d_parse_thread_stat(NULL, &th) == -1, "short or broken text is refused");

    char sw[120];
    hv_g7d_swaps("Filename Type Size Used Priority\n/dev/block/zram0 partition 4194300 1843200 -2\n", sw, sizeof sw);
    CHECK(!strcmp(sw, "zram0:1800/4095MiB"), "one swap device, base name only: %s", sw);
    hv_g7d_swaps("Filename Type Size Used Priority\n", sw, sizeof sw); CHECK(!strcmp(sw, "none"), "no device: none");
    hv_g7d_swaps(NULL, sw, sizeof sw); CHECK(!strcmp(sw, "?"), "unreadable: ?");

    char d[48];
    hv_g7d_delta(d, sizeof d, 10, 4); CHECK(!strcmp(d, "10(+6)"), "delta");
    hv_g7d_delta(d, sizeof d, 10, HV_G7D_UNKNOWN); CHECK(!strcmp(d, "10(+?)"), "no previous: +?");
    hv_g7d_delta(d, sizeof d, 3, 9); CHECK(!strcmp(d, "3(+?)"), "a counter that went down: +?");
    hv_g7d_delta(d, sizeof d, HV_G7D_UNKNOWN, 9); CHECK(!strcmp(d, "?"), "unreadable now: ?");

    /* live: two samples of this process; the lines have the fixed shape, end in one newline, and unreadable values are ?, never an invented 0 */
    HvG7DPrev pv; hv_g7d_prev_init(&pv);
    char l1[1024], l2[2048];
    hv_g7d_sample_lines(&pv, 10, l1, sizeof l1, l2, sizeof l2);
    CHECK(!strncmp(l1, "g7-diag: t=10s vm pswpin=", 25) && l1[strlen(l1) - 1] == '\n' && strchr(l1, '\n') == l1 + strlen(l1) - 1, "line 1 shape: %s", l1);
    CHECK(!strncmp(l2, "g7-diag-thr: t=10s", 18) && l2[strlen(l2) - 1] == '\n' && strchr(l2, '\n') == l2 + strlen(l2) - 1, "line 2 shape: %s", l2);
    CHECK(strstr(l1, "(+?)") || strstr(l1, "pswpin=?"), "the first sample has no previous value (+? or ?)");
    hv_g7d_sample_lines(&pv, 20, l1, sizeof l1, l2, sizeof l2);
    CHECK(strstr(l2, "cpu_ms=") && (strstr(l2, "(+0)") || strstr(l2, "(+")), "the second sample carries deltas");

    printf(g_fail ? "G7DIAG FAILED (%d)\n" : "G7DIAG OK\n", g_fail);
    return g_fail != 0;
}
