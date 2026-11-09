/* Unpacks the AVPBooter ROM from the piece of Apple's restore image that holds it (ios/Henge/BootROM.swift does the same
   on iOS): the app downloads bytes 5349700747..5350560594 of the image (859,848 bytes, HTTP Range, status 206) into a file;
   this program turns that file into AVPBooter.vmapple2.bin and checks it. Steps and exit codes:
     2 bad arguments or unreadable input     3 the input is not 859,848 bytes     4 LZFSE decoding failed
     5 the resource fork inside is malformed 6 an LZVN piece failed to decode     7 size or SHA-256 differs from the known ROM
     8 cannot write the output
   The output file is written only when every check passes. Usage: romextract IN OUT [--size N --sha256 HEX --block N --fork-offset N --fork-length N]
   (the options exist for tests with synthetic data; the defaults are the real values). liblzfse (third_party/lzfse) does the
   LZFSE and LZVN decoding. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto_portable.h"
#include "lzfse.h"
#include "lzvn_decode_base.h"

static const char *ROM_SHA256 = "fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91";

static uint32_t word(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* One LZVN piece into dst (exactly `want` bytes), or -1. */
static int lzvn_piece(const uint8_t *src, size_t n, uint8_t *dst, size_t want) {
    lzvn_decoder_state st;
    memset(&st, 0, sizeof st);
    st.src = src;
    st.src_end = src + n;
    st.dst_begin = dst;
    st.dst = dst;
    st.dst_end = dst + want;
    lzvn_decode(&st);
    return (size_t)(st.dst - dst) == want ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: romextract IN OUT\n");
        return 2;
    }
    size_t block_len = 859848, unpacked = 1048576, fork_off = 876544, fork_len = 116335, rom_len = 214904;
    const char *sha = ROM_SHA256;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--size")) rom_len = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--sha256")) sha = argv[i + 1];
        else if (!strcmp(argv[i], "--block")) block_len = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--fork-offset")) fork_off = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--fork-length")) fork_len = strtoull(argv[i + 1], NULL, 0);
        else { fprintf(stderr, "romextract: unknown option %s\n", argv[i]); return 2; }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "romextract: cannot read %s\n", argv[1]); return 2; }
    uint8_t *in = malloc(block_len + 1);
    size_t got = in ? fread(in, 1, block_len + 1, f) : 0;
    fclose(f);
    if (got != block_len) { fprintf(stderr, "romextract: input is %zu bytes, expected %zu\n", got, block_len); return 3; }
    uint8_t *whole = malloc(unpacked);
    size_t n = whole ? lzfse_decode_buffer(whole, unpacked, in, block_len, NULL) : 0;
    if (n != unpacked) { fprintf(stderr, "romextract: LZFSE decoding gave %zu bytes, expected %zu\n", n, unpacked); return 4; }
    if (fork_off + fork_len > unpacked || fork_len < 4) { fprintf(stderr, "romextract: resource fork out of range\n"); return 5; }
    const uint8_t *fork = whole + fork_off;
    uint32_t table = word(fork);
    if (table < 8 || table % 4 || table > fork_len) { fprintf(stderr, "romextract: bad piece table length %u\n", table); return 5; }
    uint8_t *rom = malloc(rom_len + 65536);
    if (!rom) return 2;
    size_t pieces = table / 4 - 1, done = 0;
    for (size_t p = 0; p < pieces; p++) {
        uint32_t start = word(fork + p * 4), end = word(fork + (p + 1) * 4);
        if (start >= end || end > fork_len) { fprintf(stderr, "romextract: piece %zu has bad bounds %u..%u\n", p, start, end); return 5; }
        if (p * 65536 >= rom_len) { fprintf(stderr, "romextract: more pieces than the ROM needs\n"); return 5; }
        size_t want = rom_len - p * 65536 < 65536 ? rom_len - p * 65536 : 65536;
        if (fork[start] == 0x06) { /* stored as it is */
            if (end - start - 1 != want) { fprintf(stderr, "romextract: stored piece %zu is %u bytes, expected %zu\n", p, end - start - 1, want); return 5; }
            memcpy(rom + done, fork + start + 1, want);
        } else if (lzvn_piece(fork + start, end - start, rom + done, want) != 0) {
            fprintf(stderr, "romextract: LZVN piece %zu did not decode to %zu bytes\n", p, want);
            return 6;
        }
        done += want;
    }
    if (done != rom_len) { fprintf(stderr, "romextract: ROM is %zu bytes, expected %zu\n", done, rom_len); return 7; }
    uint8_t d[32];
    hv_sha256(rom, rom_len, d);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    if (strcmp(hex, sha)) { fprintf(stderr, "romextract: SHA-256 is %s, expected %s\n", hex, sha); return 7; }
    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(rom, 1, rom_len, o) != rom_len || fclose(o) != 0) { fprintf(stderr, "romextract: cannot write %s\n", argv[2]); return 8; }
    printf("romextract: ok, %zu bytes, sha256 %s\n", rom_len, hex);
    return 0;
}
