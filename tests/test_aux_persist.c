/* The host half of "NVRAM survives a restart": the aux image is an ordinary file behind host/disk.c. Write -> flush -> close -> reopen (-> copy as a clone does) must
   return the same bytes, the hidden 16 KB header must stay untouched, and a read-only open must refuse writes. This says nothing about whether the GUEST stores anything in it. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../core/machine.h"
#include "../host/disk.h"

int hv_knob[HV_K_COUNT];
static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    char dir[] = "/tmp/hvaux-XXXXXX"; if (!mkdtemp(dir)) return 2;
    char aux[200], cl[200], cmd[500]; snprintf(aux, sizeof aux, "%s/aux.img", dir); snprintf(cl, sizeof cl, "%s/clone.img", dir);
    uint8_t hdr[16384], store[65536], back[65536];
    memset(hdr, 0xA5, sizeof hdr); memcpy(store, "3VVN", 4); for (int i = 4; i < 65536; i++) store[i] = (uint8_t)(i * 31 + 7);
    int fd = open(aux, O_CREAT | O_WRONLY, 0600); write(fd, hdr, sizeof hdr); ftruncate(fd, 16384 + (1 << 20)); close(fd);
    HvDisk d;
    CHECK(hv_disk_open(&d, aux, 16384, 0) == 0 && d.size == (1u << 20), "aux opens with the 16 KB hidden offset; guest-visible size 1 MiB");
    CHECK(hv_disk_write(&d, 0x30000, store, sizeof store) == 0 && hv_disk_flush(&d) == 0 && d.flush_last == HV_FLUSH_OK, "NVRAM-sized write at guest offset 0x30000 + flush ok");
    hv_disk_close(&d);
    HvDisk e; memset(back, 0, sizeof back);
    CHECK(hv_disk_open(&e, aux, 16384, 0) == 0 && hv_disk_read(&e, 0x30000, back, sizeof back) == 0 && !memcmp(back, store, sizeof store), "after close + reopen the same 64 KiB comes back (reload persistence, host side)");
    uint8_t raw[16384]; fd = open(aux, O_RDONLY); pread(fd, raw, sizeof raw, 0); close(fd);
    CHECK(!memcmp(raw, hdr, sizeof hdr), "the hidden 16 KB header is untouched");
    fd = open(aux, O_RDONLY); pread(fd, raw, 4, 16384 + 0x30000); close(fd);
    CHECK(!memcmp(raw, "3VVN", 4), "the store sits in the file at hidden offset + guest offset (16384 + 0x30000)");
    CHECK(hv_disk_write(&e, (1u << 20) - 100, store, 200) != 0 && e.write_refused == 1, "a write running past the end is refused and counted, not truncated silently");
    hv_disk_close(&e);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", aux, cl); (void)!system(cmd);
    HvDisk c; memset(back, 0, sizeof back);
    CHECK(hv_disk_open(&c, cl, 16384, 0) == 0 && hv_disk_read(&c, 0x30000, back, sizeof back) == 0 && !memcmp(back, store, sizeof store), "a file copy of the aux (a clone) carries the stored bytes");
    uint8_t z = 0xEE; hv_disk_write(&c, 0x30000, &z, 1); hv_disk_flush(&c); hv_disk_close(&c);
    HvDisk o; uint8_t chk;
    hv_disk_open(&o, aux, 16384, 0); hv_disk_read(&o, 0x30000, &chk, 1);
    CHECK(chk == '3', "writing the clone does not change the original aux");
    hv_disk_close(&o);
    HvDisk r; hv_disk_open(&r, aux, 16384, 1);
    CHECK(hv_disk_write(&r, 0, &z, 1) != 0 && r.write_refused == 1 && hv_disk_flush(&r) == 0 && r.flush_last == HV_FLUSH_SKIPPED, "read-only: write refused and counted, flush skipped (nothing would be saved)");
    hv_disk_close(&r);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); (void)!system(cmd);
    printf(g_fail ? "FAIL: %d\n" : "ALL PASS\n", g_fail);
    return g_fail != 0;
}
