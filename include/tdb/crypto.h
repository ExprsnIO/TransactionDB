#ifndef TDB_CRYPTO_H
#define TDB_CRYPTO_H

#include "tdb/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TDB_CRYPTO_KEY_SIZE 32    /* AES-256 = 32 bytes */
#define TDB_CRYPTO_IV_SIZE  16    /* AES block size */
#define TDB_CRYPTO_GCM_IV_SIZE 12  /* GCM canonical 96-bit IV */
#define TDB_CRYPTO_TAG_SIZE 16    /* GCM auth tag */

typedef struct {
    uint8_t key[TDB_CRYPTO_KEY_SIZE];
    uint8_t iv[TDB_CRYPTO_IV_SIZE];
    bool initialized;
} tdb_crypto_ctx_t;

/* Initialize crypto context with a key (32 bytes for AES-256) */
tdb_status_t tdb_crypto_init(tdb_crypto_ctx_t *ctx, const uint8_t *key, size_t key_len);

/* Encrypt a page-sized buffer in-place (TDE) */
tdb_status_t tdb_crypto_encrypt_page(tdb_crypto_ctx_t *ctx, void *page, size_t page_size, uint64_t page_id);

/* Decrypt a page-sized buffer in-place (TDE) */
tdb_status_t tdb_crypto_decrypt_page(tdb_crypto_ctx_t *ctx, void *page, size_t page_size, uint64_t page_id);

/* Encrypt arbitrary data (column-level). Output buffer must be input_len + TDB_CRYPTO_TAG_SIZE + TDB_CRYPTO_IV_SIZE bytes */
tdb_status_t tdb_crypto_encrypt(tdb_crypto_ctx_t *ctx, const void *input, size_t input_len, void *output, size_t *output_len);

/* Decrypt arbitrary data (column-level) */
tdb_status_t tdb_crypto_decrypt(tdb_crypto_ctx_t *ctx, const void *input, size_t input_len, void *output, size_t *output_len);

/* Derive a page-specific IV from page_id (for TDE - each page gets unique IV) */
void tdb_crypto_derive_page_iv(const tdb_crypto_ctx_t *ctx, uint64_t page_id, uint8_t *iv_out);

/* Generate random bytes (for IV generation) */
tdb_status_t tdb_crypto_random_bytes(void *buf, size_t len);

void tdb_crypto_destroy(tdb_crypto_ctx_t *ctx);

/* ─── Password KDF ────────────────────────────────────────────────────── */

#define TDB_PBKDF2_SALT_SIZE 16
#define TDB_PBKDF2_HASH_SIZE 32   /* SHA-256 output */

/* PBKDF2-HMAC-SHA256.
 * password/pw_len:     UTF-8 password bytes (not NUL-terminated required).
 * salt/salt_len:       random salt (recommended 16 bytes).
 * iterations:          OWASP recommends 600k+ for SHA-256; we default to 100k.
 * out/out_len:         derived key buffer (typically 32 bytes).
 * Returns TDB_OK or TDB_ERR_INVALID_ARG. */
tdb_status_t tdb_pbkdf2_sha256(const char *password, size_t pw_len,
                               const uint8_t *salt, size_t salt_len,
                               uint32_t iterations,
                               uint8_t *out, size_t out_len);

/* SHA-256 single-shot. Output is 32 bytes. */
void tdb_sha256(const void *data, size_t len, uint8_t out[32]);

/* HMAC-SHA256. Output is 32 bytes. */
void tdb_hmac_sha256(const uint8_t *key, size_t key_len,
                     const uint8_t *data, size_t data_len,
                     uint8_t out[32]);

/* SHA-1 single-shot. Output is 20 bytes. RFC 3174 — for hashing identifiers
 * only; do NOT use for new password hashes (PBKDF2 or argon2 instead). */
void tdb_sha1(const void *data, size_t len, uint8_t out[20]);

/* MD5 single-shot. Output is 16 bytes. RFC 1321 — for legacy interop only. */
void tdb_md5(const void *data, size_t len, uint8_t out[16]);

/* HMAC-SHA1. Output is 20 bytes. */
void tdb_hmac_sha1(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t out[20]);

/* ─── AES-256-GCM (P5) ────────────────────────────────────────────────
 *
 * Pure-C reference implementation following FIPS 197 (AES) and NIST
 * SP 800-38D (GCM). Validated against the NIST AES-256-GCM test vector
 * with 96-bit IV — see test_deferrals2 in the test suite.
 *
 * key:    32 bytes
 * iv:     12 bytes (96-bit) — the canonical GCM IV length
 * aad:    optional additional authenticated data
 * out_ct: caller-provided buffer of at least plain_len bytes
 * out_tag: 16-byte authentication tag
 *
 * Returns TDB_OK on success. Decrypt returns TDB_ERR_IO if tag verification
 * fails (constant-time compare). */
tdb_status_t tdb_aes256_gcm_encrypt(const uint8_t key[32],
                                    const uint8_t iv[12],
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *plain, size_t plain_len,
                                    uint8_t *out_ct,
                                    uint8_t out_tag[16]);

tdb_status_t tdb_aes256_gcm_decrypt(const uint8_t key[32],
                                    const uint8_t iv[12],
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *ct, size_t ct_len,
                                    const uint8_t tag[16],
                                    uint8_t *out_plain);

#ifdef __cplusplus
}
#endif

#endif /* TDB_CRYPTO_H */
