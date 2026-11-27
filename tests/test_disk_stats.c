/* host/disk.c counters (hv_disk_stats): writes, bytes, refused, failed, flush results; counts and file metadata only. Linux, temporary files. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../core/machine.h"
#include "../host/disk.h"

int hv_knob[HV_K_COUNT];   /* host/disk.c reads the flush knob; the machine that owns it is not linked here */
static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    char dir[] = "/tmp/hvdstat-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char root[200], aux[200], buf[4096];
    snprintf(root, sizeof root, "%s/root.img", dir); snprintf(aux, sizeof aux, "%s/aux.img", dir);
    memset(buf, 7, sizeof buf);
    int fd = open(root, O_CREAT | O_WRONLY, 0600); ftruncate(fd, 1 << 20); close(fd);
    fd = open(aux, O_CREAT | O_WRONLY, 0600); ftruncate(fd, 64 << 10); close(fd);
    HvDisk r, a; HvDiskStats s;
    CHECK(hv_disk_open(&r, root, 0, 0) == 0 && hv_disk_open(&a, aux, 16384, 0) == 0, "both disks open (aux with the 16 KB offset)");
    hv_disk_stats(&r, &s);
    CHECK(s.write_calls == 0 && s.write_bytes == 0 && s.flush_calls == 0 && s.flush_last == HV_FLUSH_NEVER && s.file_size == (1 << 20) && s.mtime_s > 0, "fresh disk: all counts 0, flush NEVER, file size and mtime reported");
    CHECK(hv_disk_write(&r, 0, buf, sizeof buf) == 0 && hv_disk_write(&r, 4096, buf, 100) == 0 && hv_disk_write(&a, 0, buf, 512) == 0, "three writes: two on root, one on aux");
    hv_disk_stats(&r, &s); HvDiskStats sa; hv_disk_stats(&a, &sa);
    CHECK(s.write_calls == 2 && s.write_bytes == 4196 && s.write_refused == 0 && s.write_failed == 0, "root: 2 writes, 4196 bytes");
    CHECK(sa.write_calls == 1 && sa.write_bytes == 512 && sa.file_size == (64 << 10), "aux: 1 write, 512 bytes, file size is the whole file (offset not subtracted)");
    CHECK(hv_disk_write(&r, (1 << 20) - 10, buf, 100) == -1, "a write past the end is refused");
    hv_disk_stats(&r, &s);
    CHECK(s.write_calls == 3 && s.write_refused == 1 && s.write_bytes == 4196, "refused is counted as asked and refused, adds no bytes");
    CHECK(hv_disk_flush(&r) == 0 && hv_disk_flush(&a) == 0, "flush both");
    hv_disk_stats(&r, &s); hv_disk_stats(&a, &sa);
    CHECK(s.flush_calls == 1 && s.flush_failed == 0 && s.flush_last == HV_FLUSH_OK && sa.flush_last == HV_FLUSH_OK, "flush result ok, per disk");
    hv_knob[HV_K_FLUSH] = 1; hv_disk_flush(&a); hv_knob[HV_K_FLUSH] = 0;
    hv_disk_stats(&a, &sa);
    CHECK(sa.flush_calls == 2 && sa.flush_last == HV_FLUSH_SKIPPED, "HV_FLUSH knob 1 is reported as skipped (no sync was done), not as ok");
    HvDisk ro; hv_disk_open(&ro, root, 0, 1);
    CHECK(hv_disk_write(&ro, 0, buf, 16) == -1 && hv_disk_flush(&ro) == 0, "read-only: write refused, flush returns 0");
    hv_disk_stats(&ro, &s);
    CHECK(s.write_calls == 1 && s.write_refused == 1 && s.flush_last == HV_FLUSH_SKIPPED, "read-only: refused counted, flush skipped");
    /* a failing pwrite: the descriptor is closed under the disk */
    close(a.fd);
    CHECK(hv_disk_write(&a, 0, buf, 16) == -1, "write on a dead descriptor fails");
    a.fd = -1;
    hv_disk_stats(&a, &sa);
    CHECK(sa.write_failed == 1 && sa.write_refused == 0 && sa.write_calls == 2, "a pwrite failure is counted as failed, not as refused");
    CHECK(hv_disk_flush(&a) != 0, "flush on the dead descriptor fails");
    hv_disk_stats(&a, &sa);
    CHECK(sa.flush_last == HV_FLUSH_FAILED && sa.flush_failed == 1 && sa.flush_calls == 3, "a failed sync is reported as FAILED and counted");
    hv_disk_close(&r); hv_disk_close(&ro);
    unlink(root); unlink(aux); rmdir(dir);
    printf(g_fail ? "DISK-STATS FAILED\n" : "DISK-STATS OK\n");
    return g_fail != 0;
}
