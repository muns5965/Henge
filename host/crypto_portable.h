#ifndef HENGEVM_CRYPTO_PORTABLE_H
#define HENGEVM_CRYPTO_PORTABLE_H

/* AES and SHA-256 for hosts without CommonCrypto (Android, Linux). Apple builds do not compile this file. */

#include <stddef.h>
#include <stdint.h>

/* AES-128/192/256 without padding. cbc: CBC with iv (16 bytes, not modified), else ECB. len must be a multiple of 16.
   Returns 0, or -1 for a bad key length or length. in and out may be the same buffer. */
int hv_aes_crypt(int encrypt, int cbc, const uint8_t *key, unsigned key_len, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len);

void hv_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
