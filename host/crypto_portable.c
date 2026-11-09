#include "crypto_portable.h"

#include <string.h>

/* ---- AES (FIPS 197), tables computed at first use ---- */

static uint8_t sbox[256], inv_sbox[256];
static int tables_ready;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

static void make_tables(void) {
    if (tables_ready) return;
    /* p walks the multiplicative group by 3, q by its inverse (1/3), so q is always 1/p. */
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    tables_ready = 1;
}

typedef struct {
    uint8_t rk[15 * 16];
    int rounds;
} AesKey;

static int expand_key(AesKey *k, const uint8_t *key, unsigned len) {
    if (len != 16 && len != 24 && len != 32) return -1;
    make_tables();
    unsigned nk = len / 4;
    k->rounds = (int)nk + 6;
    unsigned total = 4 * (unsigned)(k->rounds + 1);
    memcpy(k->rk, key, len);
    uint8_t rcon = 1;
    for (unsigned i = nk; i < total; i++) {
        uint8_t t[4];
        memcpy(t, k->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[u];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) k->rk[4 * i + j] = (uint8_t)(k->rk[4 * (i - nk) + j] ^ t[j]);
    }
    return 0;
}

static void add_round_key(uint8_t *s, const uint8_t *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void encrypt_block(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, k->rk);
    for (int r = 1; r <= k->rounds; r++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        /* ShiftRows: state is column-major, byte (row, col) at 4 * col + row. */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[4 * c + row] = s[4 * ((c + row) & 3) + row];
        if (r != k->rounds) {
            for (int c = 0; c < 4; c++) {
                const uint8_t *a = t + 4 * c;
                s[4 * c + 0] = (uint8_t)(xtime(a[0]) ^ (xtime(a[1]) ^ a[1]) ^ a[2] ^ a[3]);
                s[4 * c + 1] = (uint8_t)(a[0] ^ xtime(a[1]) ^ (xtime(a[2]) ^ a[2]) ^ a[3]);
                s[4 * c + 2] = (uint8_t)(a[0] ^ a[1] ^ xtime(a[2]) ^ (xtime(a[3]) ^ a[3]));
                s[4 * c + 3] = (uint8_t)((xtime(a[0]) ^ a[0]) ^ a[1] ^ a[2] ^ xtime(a[3]));
            }
        } else {
            memcpy(s, t, 16);
        }
        add_round_key(s, k->rk + 16 * r);
    }
    memcpy(out, s, 16);
}

static void decrypt_block(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, k->rk + 16 * k->rounds);
    for (int r = k->rounds - 1; r >= 0; r--) {
        /* InvShiftRows, InvSubBytes */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[4 * ((c + row) & 3) + row] = s[4 * c + row];
        for (int i = 0; i < 16; i++) t[i] = inv_sbox[t[i]];
        add_round_key(t, k->rk + 16 * r);
        if (r != 0) {
            for (int c = 0; c < 4; c++) {
                const uint8_t *a = t + 4 * c;
                s[4 * c + 0] = (uint8_t)(gmul(a[0], 14) ^ gmul(a[1], 11) ^ gmul(a[2], 13) ^ gmul(a[3], 9));
                s[4 * c + 1] = (uint8_t)(gmul(a[0], 9) ^ gmul(a[1], 14) ^ gmul(a[2], 11) ^ gmul(a[3], 13));
                s[4 * c + 2] = (uint8_t)(gmul(a[0], 13) ^ gmul(a[1], 9) ^ gmul(a[2], 14) ^ gmul(a[3], 11));
                s[4 * c + 3] = (uint8_t)(gmul(a[0], 11) ^ gmul(a[1], 13) ^ gmul(a[2], 9) ^ gmul(a[3], 14));
            }
        } else {
            memcpy(s, t, 16);
        }
    }
    memcpy(out, s, 16);
}

int hv_aes_crypt(int encrypt, int cbc, const uint8_t *key, unsigned key_len, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    AesKey k;
    if (len % 16 || expand_key(&k, key, key_len) != 0) return -1;
    uint8_t chain[16], blk[16], next[16];
    if (cbc) {
        if (!iv) return -1;
        memcpy(chain, iv, 16);
    }
    for (size_t off = 0; off < len; off += 16) {
        if (encrypt) {
            for (int i = 0; i < 16; i++) blk[i] = (uint8_t)(in[off + i] ^ (cbc ? chain[i] : 0));
            encrypt_block(&k, blk, out + off);
            if (cbc) memcpy(chain, out + off, 16);
        } else {
            memcpy(next, in + off, 16);
            decrypt_block(&k, in + off, blk);
            for (int i = 0; i < 16; i++) out[off + i] = (uint8_t)(blk[i] ^ (cbc ? chain[i] : 0));
            memcpy(chain, next, 16);
        }
    }
    return 0;
}

/* ---- SHA-256 (FIPS 180-4) ---- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void hv_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t off = 0;
    for (; off + 64 <= len; off += 64) sha_block(h, data + off);
    uint8_t tail[128] = {0};
    size_t rest = len - off;
    memcpy(tail, data + off, rest);
    tail[rest] = 0x80;
    size_t total = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t o = 0; o < total; o += 64) sha_block(h, tail + o);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}
