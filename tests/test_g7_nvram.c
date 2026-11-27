/* host/g7_nvram.h: the S0 aux-traffic log (recognition of v3 entries, cap, dedup, summary) and the S1 pre-seed (only into a valid "3VVN" store, never otherwise). Linux, temporary files, no guest. */
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
#include "../host/g7_nvram.h"

int hv_knob[HV_K_COUNT];
static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* An independent builder (test side): store header + entries, rest 0xFF. */
static size_t put_entry(uint8_t *p, const char *name, const char *val, const uint8_t *guid, uint8_t state) {
    size_t nl = strlen(name) + 1, vl = strlen(val);
    p[0] = 0xAA; p[1] = 0x55; p[2] = state; p[3] = 0;
    memset(p + 4, 0, 4);
    uint32_t a = (uint32_t)nl, b = (uint32_t)vl;
    memcpy(p + 8, &a, 4); memcpy(p + 12, &b, 4);   /* little-endian host (x86/arm64) */
    memcpy(p + 16, guid, 16);
    uint32_t c = g7nv_crc32((const uint8_t *)val, vl);
    memcpy(p + 32, &c, 4);
    memcpy(p + 36, name, nl); memcpy(p + 36 + nl, val, vl);
    return 36 + nl + vl;
}
static void build_store(uint8_t *part, uint32_t gen, int with_autoboot_false) {
    memset(part, 0xFF, G7NV_PART);
    memcpy(part, "3VVN", 4);
    uint32_t sz = G7NV_PART, sys = 0x4000, com = 0x4000;
    memcpy(part + 4, &sz, 4); memcpy(part + 8, &gen, 4);
    part[12] = 0xFF; part[13] = 0xFF; part[14] = 1; part[15] = 0;
    memcpy(part + 16, &sys, 4); memcpy(part + 20, &com, 4);
    size_t at = 24;
    at += put_entry(part + at, "boot-args", "-v", g7nv_guid_common, 0x7F);
    if (with_autoboot_false) at += put_entry(part + at, "auto-boot", "false", g7nv_guid_system, 0x7F);
}
static int mkfile(char *path, uint64_t size) { int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600); if (fd < 0) return -1; int r = ftruncate(fd, (off_t)size); close(fd); return r; }
static unsigned long fsum(const char *path) { FILE *f = fopen(path, "rb"); unsigned long h = 1469598103u; int c; while ((c = fgetc(f)) != EOF) h = (h ^ (unsigned)c) * 16777619u; fclose(f); return h; }
static char *slurp(FILE *f) { fflush(f); long n = ftell(f); rewind(f); char *b = calloc(1, (size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) b[0] = 0; return b; }
static int count_str(const char *h, const char *n) { int c = 0; for (const char *p = h; (p = strstr(p, n)); p++) c++; return c; }

int main(void) {
    CHECK(g7nv_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc32 test vector");

    /* spec parsing */
    G7NvSeed it[G7NV_SEED_MAX]; char err[96] = "";
    CHECK(g7nv_parse_spec(NULL, it, 4, err, sizeof err) == 0 && g7nv_parse_spec("", it, 4, err, sizeof err) == 0 && g7nv_parse_spec("0", it, 4, err, sizeof err) == 0 && g7nv_parse_spec("off", it, 4, err, sizeof err) == 0, "empty/0/off = nothing requested");
    CHECK(g7nv_parse_spec("1", it, 4, err, sizeof err) == 1 && !strcmp(it[0].name, "auto-boot") && it[0].vlen == 4 && !memcmp(it[0].val, "true", 4), "\"1\" = auto-boot=true only");
    CHECK(g7nv_parse_spec("auto-boot=true;boot-volume=AAAA:BBBB:CCCC", it, 4, err, sizeof err) == 2 && !strcmp(it[1].name, "boot-volume") && it[1].vlen == 14, "boot-volume only from an explicit string (value kept verbatim)");
    CHECK(g7nv_parse_spec("auto-boot=true\nboot-args=-v", it, 4, err, sizeof err) == 2, "newline separator");
    CHECK(g7nv_parse_spec("boot-volume", it, 4, err, sizeof err) == -1, "name without a value is refused (no default boot-volume)");
    CHECK(g7nv_parse_spec("a=1;a=2", it, 4, err, sizeof err) == -1 && g7nv_parse_spec("a b=1", it, 4, err, sizeof err) == -1 && g7nv_parse_spec("a=\x01", it, 4, err, sizeof err) == -1, "duplicate, bad name, non-printable value refused");
    char sp[64];
    CHECK(!g7nv_spec_source(NULL, NULL, NULL, sp, sizeof sp), "default: nothing requested");
    CHECK(g7nv_spec_source("0", "1", "1", sp, sizeof sp) && !strcmp(sp, "0"), "an environment value wins, even when it says off");
    CHECK(g7nv_spec_source(NULL, "1", "x", sp, sizeof sp) && !strcmp(sp, "1") && g7nv_spec_source(NULL, NULL, "auto-boot=true", sp, sizeof sp) && !strcmp(sp, "auto-boot=true"), "HENGE_ env, then marker file");

    /* S0: recognition, dedup, summary */
    char dir[] = "/tmp/g7nv-XXXXXX"; if (!mkdtemp(dir)) return 2;
    uint8_t *part = malloc(G7NV_PART); build_store(part, 5, 1);
    FILE *lg = tmpfile(); G7Nv nv; g7nv_init(&nv, lg, 1);
    g7nv_trace(&nv, HV_DISK_TRACE_WRITE, 0x40000, part, G7NV_PART, 0);
    g7nv_trace(&nv, HV_DISK_TRACE_WRITE, 0x40000, part, G7NV_PART, 0);   /* the same bytes again: entries are deduplicated, the op line is not */
    g7nv_trace(&nv, HV_DISK_TRACE_FLUSH, 0, NULL, 0, 0);
    g7nv_trace(&nv, HV_DISK_TRACE_READ, 0x40000, part, 512, 0);
    g7nv_summary(&nv); g7nv_summary(&nv);
    char *txt = slurp(lg);
    CHECK(strstr(txt, "set name=auto-boot guid=system state=0x7f size=5 val32=66616c7365 txt=\"false\""), "write of the store: auto-boot=false with the system GUID recognised");
    CHECK(strstr(txt, "set name=boot-args guid=common") != NULL, "boot-args with the common GUID");
    CHECK(count_str(txt, "set name=auto-boot") == 1, "second identical write: entry line not repeated (dedup)");
    CHECK(strstr(txt, "store-header aux_off=0x40000 size=65536 generation=5") != NULL, "store header seen");
    CHECK(strstr(txt, "F aux flush rc=0") != NULL && strstr(txt, " W aux_off=0x40000 len=65536 rc=0 head32=33") != NULL, "flush and write op lines (head32 starts with '3VVN' = 33 56 56 4e)");
    CHECK(count_str(txt, "g7-nvram: summary") == 1 && strstr(txt, "auto-boot=last:get/state=0x7f/size=5/guid=system") && strstr(txt, "boot-volume=never-seen"), "exactly one summary line; auto-boot tracked, boot-volume never seen");
    CHECK(strstr(txt, "aux_writes=2 (131072 B, failed 0) aux_flushes=1") != NULL, "summary counters");
    free(txt); fclose(lg);

    /* delete state and a cut-off request */
    uint8_t one[256]; size_t el = put_entry(one, "boot-volume", "X:Y", g7nv_guid_system, 0x7C);
    lg = tmpfile(); g7nv_init(&nv, lg, 1);
    g7nv_trace(&nv, HV_DISK_TRACE_WRITE, 0x1000, one, el, 0);
    g7nv_trace(&nv, HV_DISK_TRACE_WRITE, 0x2000, part + 24 + 48, 48, 0);   /* auto-boot entry cut after 2 of its 5 value bytes: shown as cut off (system GUID known) */
    txt = slurp(lg);
    CHECK(strstr(txt, "delete name=boot-volume guid=system state=0x7c"), "state 0x7c = delete");
    CHECK(strstr(txt, "cut off by the request end") != NULL, "cut-off entry with a known GUID is shown as cut off");
    free(txt); fclose(lg);
    one[el - 1] ^= 1;   /* corrupt the value: CRC mismatch */
    lg = tmpfile(); g7nv_init(&nv, lg, 1);
    g7nv_trace(&nv, HV_DISK_TRACE_WRITE, 0x1000, one, el, 0);
    txt = slurp(lg); CHECK(!strstr(txt, "name=boot-volume"), "an entry with a wrong CRC is not reported"); free(txt); fclose(lg);

    /* cap */
    lg = tmpfile(); g7nv_init(&nv, lg, 1);
    for (int i = 0; i < 1000; i++) { uint8_t b[512]; memset(b, i, sizeof b); g7nv_trace(&nv, HV_DISK_TRACE_WRITE, (uint64_t)i * 512, b, sizeof b, 0); }
    txt = slurp(lg);
    CHECK(count_str(txt, "g7-nvram: n=") <= G7NV_LINE_CAP && count_str(txt, "g7-nvram: n=") == G7NV_OP_LINES, "1000 plain writes: capped (%d op lines)", count_str(txt, "g7-nvram: n="));
    g7nv_summary(&nv); free(txt); txt = slurp(lg);
    CHECK(strstr(txt, "aux_writes=1000") && strstr(txt, "suppressed=900"), "summary counts all writes and the suppressed lines"); free(txt); fclose(lg);
    lg = tmpfile(); g7nv_init(&nv, lg, 0); g7nv_trace(&nv, HV_DISK_TRACE_WRITE, 0, part, 512, 0); txt = slurp(lg); CHECK(!*txt, "log off: nothing printed by the observer"); free(txt); fclose(lg);

    /* observer wired through hv_disk (same path as bdif and virtio-blk) */
    char aux[200]; snprintf(aux, sizeof aux, "%s/aux.img", dir);
    uint64_t hidden = 16384, vis = 4u << 20;
    CHECK(mkfile(aux, hidden + vis) == 0, "aux file");
    HvDisk d; CHECK(hv_disk_open(&d, aux, hidden, 0) == 0, "open aux with the hidden offset");
    lg = tmpfile(); g7nv_init(&nv, lg, 1); hv_disk_set_trace(&d, g7nv_trace, &nv);
    CHECK(hv_disk_write(&d, 0x30000, part, G7NV_PART) == 0, "write through hv_disk");
    uint8_t *rb = malloc(G7NV_PART); CHECK(hv_disk_read(&d, 0x30000, rb, G7NV_PART) == 0 && !memcmp(rb, part, G7NV_PART), "read back unchanged (observer is read-only)");
    hv_disk_flush(&d);
    txt = slurp(lg); CHECK(strstr(txt, "W aux_off=0x30000") && strstr(txt, "R store-header aux_off=0x30000") && strstr(txt, "F aux flush"), "observer saw write, read and flush"); free(txt); fclose(lg);
    hv_disk_set_trace(&d, NULL, NULL);

    /* S1: no store -> nothing written */
    hv_disk_close(&d);
    unsigned long before = fsum(aux);
    hv_disk_open(&d, aux, hidden, 0);
    FILE *plg = tmpfile();
    mkfile(aux, hidden + vis);   /* all zeros again */
    before = fsum(aux);
    G7NvSeedResult r = g7nv_preseed(&d, (G7NvSeed[]){{"auto-boot", "true", 4}}, 1, plg);
    txt = slurp(plg);
    CHECK(!r.store_found && r.seeded == 0 && fsum(aux) == before && strstr(txt, "NO valid v3 \"3VVN\" store") && strstr(txt, "NOTHING WRITTEN"), "no store: nothing written, file unchanged, said so"); free(txt); fclose(plg);

    /* a bare "3VVN" without a consistent entry chain is not a store */
    uint8_t junk[512]; memset(junk, 0xAA, sizeof junk); memcpy(junk, "3VVN", 4); junk[14] = 1; uint32_t s64k = G7NV_PART; memcpy(junk + 4, &s64k, 4);
    hv_disk_write(&d, 0x10000, junk, sizeof junk); before = fsum(aux);
    plg = tmpfile(); r = g7nv_preseed(&d, (G7NvSeed[]){{"auto-boot", "true", 4}}, 1, plg); txt = slurp(plg);
    CHECK(!r.store_found && fsum(aux) == before, "a lookalike header with garbage entries is rejected"); free(txt); fclose(plg);

    /* S1: with a store (two banks, the newer one is generation 7) */
    hv_disk_close(&d); mkfile(aux, hidden + vis); hv_disk_open(&d, aux, hidden, 0);
    build_store(part, 6, 1); hv_disk_write(&d, 0x20000, part, G7NV_PART);
    build_store(part, 7, 1); hv_disk_write(&d, 0x30000, part, G7NV_PART);
    plg = tmpfile();
    r = g7nv_preseed(&d, (G7NvSeed[]){{"auto-boot", "true", 4}, {"boot-volume", "AAAA:BBBB", 9}}, 2, plg);
    txt = slurp(plg);
    CHECK(r.store_found && r.store_off == 0x30000 && r.generation == 7 && r.seeded == 2, "newest bank (generation 7) chosen, 2 variables seeded: %s", r.seeded == 2 ? "ok" : txt);
    CHECK(strstr(txt, "auto-boot=true written (guid=system") && strstr(txt, "boot-volume=AAAA:BBBB written (guid=common") && strstr(txt, "1 older copy/copies marked deleted"), "result lines: system GUID for auto-boot, common for boot-volume, old auto-boot copy invalidated");
    free(txt); fclose(plg);
    uint8_t *after = malloc(G7NV_PART); hv_disk_read(&d, 0x30000, after, G7NV_PART);
    uint32_t sz; int ne; size_t end; G7NvPEnt *ents = malloc(sizeof(G7NvPEnt) * 512);
    CHECK(g7nv_walk(after, G7NV_PART, &sz, ents, 512, &ne, &end) && ne == 4, "the store still walks cleanly (CRCs valid), 4 entries now");
    int act_ab = 0, old_ab = 0, bv = 0;
    for (int i = 0; i < ne; i++) { if (!strcmp(ents[i].name, "auto-boot") && ents[i].state == 0x7F && ents[i].dsz == 4 && !memcmp(ents[i].val, "true", 4)) act_ab++; if (!strcmp(ents[i].name, "auto-boot") && ents[i].state == 0x7C) old_ab++; if (!strcmp(ents[i].name, "boot-volume") && ents[i].state == 0x7F) bv++; }
    CHECK(act_ab == 1 && old_ab == 1 && bv == 1, "one active auto-boot=true, the old false copy is state 0x7c, boot-volume added");
    uint8_t *other = malloc(G7NV_PART); hv_disk_read(&d, 0x20000, other, G7NV_PART); build_store(part, 6, 1);
    CHECK(!memcmp(other, part, G7NV_PART), "the older bank is untouched");
    /* idempotent */
    plg = tmpfile(); r = g7nv_preseed(&d, (G7NvSeed[]){{"auto-boot", "true", 4}}, 1, plg); txt = slurp(plg);
    CHECK(r.already == 1 && r.seeded == 0 && strstr(txt, "already has the requested value"), "second run: unchanged"); free(txt); fclose(plg);
    /* bytes after the used area are not erased: refuse */
    uint8_t *full = malloc(G7NV_PART);
    hv_disk_close(&d); mkfile(aux, hidden + vis); hv_disk_open(&d, aux, hidden, 0);
    build_store(full, 9, 1);
    memset(full + 123, 0x00, 36);      /* the chain ends here (used end = 24 + 48 + 51 = 123) ... */
    memset(full + 159, 0x11, 1000);    /* ... but what follows is not erased */
    hv_disk_write(&d, 0x10000, full, G7NV_PART); before = fsum(aux);
    plg = tmpfile(); r = g7nv_preseed(&d, (G7NvSeed[]){{"auto-boot", "true", 4}}, 1, plg); txt = slurp(plg);
    CHECK(r.store_found && r.seeded == 0 && r.failed == 1 && fsum(aux) == before && strstr(txt, "NOT written"), "bytes after the used area not erased: refuses, file unchanged"); free(txt); fclose(plg);
    hv_disk_close(&d);
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    printf(g_fail ? "FAILED %d\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
