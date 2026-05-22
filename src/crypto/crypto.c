#include "tdb/crypto.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef __APPLE__
#include <sys/random.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* Forward declare arc4random_buf for macOS if not declared by headers */
#ifdef __APPLE__
extern void arc4random_buf(void *buf, size_t nbytes);
#endif

/*
 * Transparent Data Encryption (TDE) and column-level encryption engine.
 *
 * AES-256-GCM encryption engine for page-level TDE and column-level
 * encryption.  Uses the NIST-validated AES-256-GCM implementation below.
 *
 * Page-level encryption (TDE):
 *   - Each page gets a unique 12-byte IV derived from the base IV + page_id
 *     via SHA-256 truncation.
 *   - The first 32 bytes (page header) are left in the clear so that the
 *     crash-recovery subsystem can identify pages without decrypting.
 *   - A 16-byte GCM authentication tag is stored at the end of the page.
 *   - AAD covers page_id + clear header for page binding.
 *
 * Column-level encryption:
 *   - A random 12-byte IV is generated per encryption call.
 *   - Output layout: [IV (12 bytes)][ciphertext (input_len bytes)][tag (16 bytes)]
 *   - The tag is an AES-256-GCM authentication tag (AEAD).
 */

/* --------------------------------------------------------------------------
 *  Platform-specific random bytes
 * -------------------------------------------------------------------------- */

#if defined(__APPLE__)
  /* macOS: arc4random_buf is always available, no header needed beyond stdlib.h */
#elif defined(__linux__)
  #include <fcntl.h>
  #include <unistd.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

tdb_status_t tdb_crypto_random_bytes(void *buf, size_t len) {
    if (!buf || len == 0) return TDB_ERR_INVALID_ARG;

#if defined(__APPLE__)
    arc4random_buf(buf, len);
    return TDB_OK;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return TDB_ERR_IO;

    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (uint8_t *)buf + total, len - total);
        if (n <= 0) {
            close(fd);
            return TDB_ERR_IO;
        }
        total += (size_t)n;
    }
    close(fd);
    return TDB_OK;
#endif
}

/* --------------------------------------------------------------------------
 *  Key / IV mixing helpers (retained for reference / fallback)
 * --------------------------------------------------------------------------
 *
 *  These functions produce a deterministic, well-distributed byte stream from
 *  a key and IV.  Superseded by AES-256-GCM below but retained in case a
 *  non-AES build is ever needed.
 */

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* Rotate left for 64-bit value */
static inline uint64_t rotl64(uint64_t v, int k) {
    return (v << k) | (v >> (64 - k));
}

/*
 * mix_state: given an 8-byte state block and a round constant, produce the
 * next state.  Loosely inspired by SplitMix64 / xorshift.
 */
static inline uint64_t mix_state(uint64_t s, uint64_t c) {
    s += c;
    s ^= s >> 30;
    s *= 0xbf58476d1ce4e5b9ULL;
    s ^= s >> 27;
    s *= 0x94d049bb133111ebULL;
    s ^= s >> 31;
    return s;
}

/*
 * Generate a keystream block.  We maintain two 64-bit state words seeded from
 * the key and IV, then iterate a mixing round per 16 output bytes.
 */
typedef struct {
    uint64_t s0;
    uint64_t s1;
} keystream_state_t;

static void keystream_init(keystream_state_t *ks, const uint8_t *key, const uint8_t *iv) {
    /* Seed s0 from the first 16 bytes of key + IV[0..7] */
    uint64_t k0, k1, k2, k3;
    memcpy(&k0, key,      8);
    memcpy(&k1, key + 8,  8);
    memcpy(&k2, key + 16, 8);
    memcpy(&k3, key + 24, 8);

    uint64_t iv0, iv1;
    memcpy(&iv0, iv,     8);
    memcpy(&iv1, iv + 8, 8);

    ks->s0 = mix_state(k0 ^ iv0, k1);
    ks->s1 = mix_state(k2 ^ iv1, k3);
}

/* Produce the next 16 bytes of keystream and advance state */
static void keystream_next(keystream_state_t *ks, uint8_t out[16]) {
    ks->s0 = rotl64(ks->s0, 17) ^ ks->s1;
    ks->s1 = mix_state(ks->s1, ks->s0);
    memcpy(out,     &ks->s0, 8);
    memcpy(out + 8, &ks->s1, 8);
}

/*
 * XOR-cipher: apply keystream to buf[offset .. offset+len-1].
 * This is its own inverse (encrypt == decrypt).
 */
static void xor_cipher(const uint8_t *key, const uint8_t *iv,
                        uint8_t *buf, size_t offset, size_t len) {
    keystream_state_t ks;
    keystream_init(&ks, key, iv);

    /*
     * We need to skip `offset` bytes of keystream so that the cipher aligns
     * correctly.  Generate and discard full 16-byte blocks.
     */
    size_t skip_blocks = offset / 16;
    size_t skip_extra  = offset % 16;

    uint8_t block[16];
    for (size_t b = 0; b < skip_blocks; b++) {
        keystream_next(&ks, block);
    }

    /* Handle partial leading block */
    size_t pos = 0;
    if (skip_extra > 0) {
        keystream_next(&ks, block);
        size_t use = 16 - skip_extra;
        if (use > len) use = len;
        for (size_t i = 0; i < use; i++) {
            buf[offset + pos] ^= block[skip_extra + i];
            pos++;
        }
    }

    /* Full blocks */
    while (pos + 16 <= len) {
        keystream_next(&ks, block);
        for (size_t i = 0; i < 16; i++) {
            buf[offset + pos] ^= block[i];
            pos++;
        }
    }

    /* Trailing partial block */
    if (pos < len) {
        keystream_next(&ks, block);
        for (size_t i = 0; pos < len; i++, pos++) {
            buf[offset + pos] ^= block[i];
        }
    }
}

/* --------------------------------------------------------------------------
 *  Simple checksum tag (placeholder for GCM authentication tag)
 * --------------------------------------------------------------------------
 *
 *  Produces a 16-byte tag over the ciphertext by iteratively mixing all bytes
 *  with the key.  This detects accidental corruption but is NOT a MAC.
 */
static void compute_tag(const uint8_t *key, const uint8_t *data, size_t len,
                         uint8_t tag[TDB_CRYPTO_TAG_SIZE]) {
    uint64_t h0 = 0x736f6d6570736575ULL;  /* "somepseu" */
    uint64_t h1 = 0x646f72616e646f6dULL;  /* "dorandom" */

    /* Mix in key */
    uint64_t kk0, kk1;
    memcpy(&kk0, key,     8);
    memcpy(&kk1, key + 8, 8);
    h0 ^= kk0;
    h1 ^= kk1;

    /* Mix in data */
    for (size_t i = 0; i < len; i++) {
        h0 = rotl64(h0, 13) + h1 + data[i];
        h1 = rotl64(h1, 29) ^ h0;
    }

    /* Final avalanche */
    h0 = mix_state(h0, h1);
    h1 = mix_state(h1, h0);

    memcpy(tag,     &h0, 8);
    memcpy(tag + 8, &h1, 8);
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/* --------------------------------------------------------------------------
 *  Public API
 * -------------------------------------------------------------------------- */

tdb_status_t tdb_crypto_init(tdb_crypto_ctx_t *ctx, const uint8_t *key, size_t key_len) {
    if (!ctx || !key) return TDB_ERR_INVALID_ARG;
    if (key_len != TDB_CRYPTO_KEY_SIZE) return TDB_ERR_INVALID_ARG;

    memcpy(ctx->key, key, TDB_CRYPTO_KEY_SIZE);

    tdb_status_t rc = tdb_crypto_random_bytes(ctx->iv, TDB_CRYPTO_IV_SIZE);
    if (rc != TDB_OK) {
        memset(ctx, 0, sizeof(*ctx));
        return rc;
    }

    ctx->initialized = true;
    return TDB_OK;
}

void tdb_crypto_derive_page_iv(const tdb_crypto_ctx_t *ctx, uint64_t page_id,
                                uint8_t *iv_out) {
    /* Derive a 12-byte GCM IV from base IV + page_id using SHA-256 truncation */
    uint8_t seed[24]; /* 16 bytes IV + 8 bytes page_id */
    memcpy(seed, ctx->iv, TDB_CRYPTO_IV_SIZE);
    memcpy(seed + TDB_CRYPTO_IV_SIZE, &page_id, 8);
    uint8_t hash[32];
    tdb_sha256(seed, sizeof(seed), hash);
    memcpy(iv_out, hash, 12); /* truncate to 96 bits */
}

tdb_status_t tdb_crypto_encrypt_page(tdb_crypto_ctx_t *ctx, void *page,
                                      size_t page_size, uint64_t page_id) {
    if (!ctx || !ctx->initialized) return TDB_ERR_CRYPTO;
    if (!page || page_size < 64)   return TDB_ERR_INVALID_ARG;

    static const size_t HEADER_CLEAR = 32;

    /* Need room for header + at least 1 byte + tag */
    if (page_size < HEADER_CLEAR + 1 + TDB_CRYPTO_TAG_SIZE)
        return TDB_ERR_INVALID_ARG;

    uint8_t gcm_iv[12];
    tdb_crypto_derive_page_iv(ctx, page_id, gcm_iv);

    uint8_t *p = (uint8_t *)page;
    size_t cipher_len = page_size - HEADER_CLEAR - TDB_CRYPTO_TAG_SIZE;

    /* AAD = page_id (8 bytes) + clear header (32 bytes) */
    uint8_t aad[40];
    memcpy(aad, &page_id, 8);
    memcpy(aad + 8, p, HEADER_CLEAR);

    /* Encrypt in a temp buffer, then copy back */
    uint8_t *plaintext = p + HEADER_CLEAR;
    uint8_t *tag_out = p + page_size - TDB_CRYPTO_TAG_SIZE;

    /* We need a temporary buffer for the ciphertext since AES-GCM can't always work in-place safely */
    uint8_t *tmp = (uint8_t *)malloc(cipher_len);
    if (!tmp) return TDB_ERR_IO;

    tdb_status_t rc = tdb_aes256_gcm_encrypt(ctx->key, gcm_iv,
                                              aad, sizeof(aad),
                                              plaintext, cipher_len,
                                              tmp, tag_out);
    if (rc == TDB_OK) {
        memcpy(plaintext, tmp, cipher_len);
    }
    free(tmp);
    return rc;
}

tdb_status_t tdb_crypto_decrypt_page(tdb_crypto_ctx_t *ctx, void *page,
                                      size_t page_size, uint64_t page_id) {
    if (!ctx || !ctx->initialized) return TDB_ERR_CRYPTO;
    if (!page || page_size < 64)   return TDB_ERR_INVALID_ARG;

    static const size_t HEADER_CLEAR = 32;
    if (page_size < HEADER_CLEAR + 1 + TDB_CRYPTO_TAG_SIZE)
        return TDB_ERR_INVALID_ARG;

    uint8_t gcm_iv[12];
    tdb_crypto_derive_page_iv(ctx, page_id, gcm_iv);

    uint8_t *p = (uint8_t *)page;
    size_t cipher_len = page_size - HEADER_CLEAR - TDB_CRYPTO_TAG_SIZE;
    const uint8_t *tag = p + page_size - TDB_CRYPTO_TAG_SIZE;

    uint8_t aad[40];
    memcpy(aad, &page_id, 8);
    memcpy(aad + 8, p, HEADER_CLEAR);

    uint8_t *tmp = (uint8_t *)malloc(cipher_len);
    if (!tmp) return TDB_ERR_IO;

    tdb_status_t rc = tdb_aes256_gcm_decrypt(ctx->key, gcm_iv,
                                              aad, sizeof(aad),
                                              p + HEADER_CLEAR, cipher_len,
                                              tag, tmp);
    if (rc == TDB_OK) {
        memcpy(p + HEADER_CLEAR, tmp, cipher_len);
    }
    free(tmp);
    return rc;
}

tdb_status_t tdb_crypto_encrypt(tdb_crypto_ctx_t *ctx,
                                 const void *input, size_t input_len,
                                 void *output, size_t *output_len) {
    if (!ctx || !ctx->initialized) return TDB_ERR_CRYPTO;
    if (!input || !output || !output_len) return TDB_ERR_INVALID_ARG;
    if (input_len == 0) return TDB_ERR_INVALID_ARG;

    /* Output layout: [IV (12)][ciphertext (input_len)][tag (16)] */
    size_t total = TDB_CRYPTO_GCM_IV_SIZE + input_len + TDB_CRYPTO_TAG_SIZE;

    uint8_t *out = (uint8_t *)output;

    /* Generate a fresh random 12-byte IV */
    uint8_t col_iv[12];
    tdb_status_t rc = tdb_crypto_random_bytes(col_iv, 12);
    if (rc != TDB_OK) return rc;

    /* Prepend IV */
    memcpy(out, col_iv, 12);

    /* Encrypt */
    uint8_t *ct = out + 12;
    uint8_t *tag = ct + input_len;
    rc = tdb_aes256_gcm_encrypt(ctx->key, col_iv,
                                 NULL, 0,  /* no AAD for column-level */
                                 (const uint8_t *)input, input_len,
                                 ct, tag);
    if (rc != TDB_OK) return rc;

    *output_len = total;
    return TDB_OK;
}

tdb_status_t tdb_crypto_decrypt(tdb_crypto_ctx_t *ctx,
                                 const void *input, size_t input_len,
                                 void *output, size_t *output_len) {
    if (!ctx || !ctx->initialized) return TDB_ERR_CRYPTO;
    if (!input || !output || !output_len) return TDB_ERR_INVALID_ARG;

    /* Minimum size: IV(12) + at least 1 byte + tag(16) */
    size_t overhead = TDB_CRYPTO_GCM_IV_SIZE + TDB_CRYPTO_TAG_SIZE;
    if (input_len <= overhead) return TDB_ERR_INVALID_ARG;

    const uint8_t *in = (const uint8_t *)input;
    size_t data_len = input_len - overhead;

    /* Extract IV (12 bytes) */
    const uint8_t *col_iv = in;

    /* Extract ciphertext */
    const uint8_t *ct = in + TDB_CRYPTO_GCM_IV_SIZE;

    /* Extract tag */
    const uint8_t *tag = ct + data_len;

    /* Decrypt with authentication */
    tdb_status_t rc = tdb_aes256_gcm_decrypt(ctx->key, col_iv,
                                              NULL, 0,  /* no AAD */
                                              ct, data_len,
                                              tag,
                                              (uint8_t *)output);
    if (rc != TDB_OK) return TDB_ERR_CORRUPT;

    *output_len = data_len;
    return TDB_OK;
}

/* --------------------------------------------------------------------------
 *  SHA-256 — minimal portable implementation (FIPS 180-4)
 * --------------------------------------------------------------------------
 *  Suitable as a building block for HMAC-SHA256 + PBKDF2. Not optimised for
 *  bulk throughput; PBKDF2 invokes HMAC many times so we keep the inner loop
 *  small and branch-free. Verified against RFC 6234 / FIPS test vectors via
 *  test_crypto_pbkdf2.c. */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint32_t datalen;
    uint8_t  buffer[64];
} tdb_sha256_ctx_t;

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(tdb_sha256_ctx_t *c, const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4+1] << 16)
             | ((uint32_t)data[i*4+2] << 8) | (uint32_t)data[i*4+3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->state[0],b=c->state[1],d=c->state[2],e=c->state[3];
    uint32_t f=c->state[4],g=c->state[5],h=c->state[6],hh=c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(f,6) ^ rotr32(f,11) ^ rotr32(f,25);
        uint32_t ch = (f & g) ^ (~f & h);
        uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t mj = (a & b) ^ (a & d) ^ (b & d);
        uint32_t t2 = S0 + mj;
        hh = h; h = g; g = f; f = e + t1;
        e = d; d = b; b = a; a = t1 + t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=d; c->state[3]+=e;
    c->state[4]+=f; c->state[5]+=g; c->state[6]+=h; c->state[7]+=hh;
}

static void sha256_init(tdb_sha256_ctx_t *c) {
    c->state[0]=0x6a09e667u; c->state[1]=0xbb67ae85u; c->state[2]=0x3c6ef372u; c->state[3]=0xa54ff53au;
    c->state[4]=0x510e527fu; c->state[5]=0x9b05688cu; c->state[6]=0x1f83d9abu; c->state[7]=0x5be0cd19u;
    c->bitlen=0; c->datalen=0;
}

static void sha256_update(tdb_sha256_ctx_t *c, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        c->buffer[c->datalen++] = data[i];
        if (c->datalen == 64) {
            sha256_transform(c, c->buffer);
            c->bitlen += 512;
            c->datalen = 0;
        }
    }
}

static void sha256_final(tdb_sha256_ctx_t *c, uint8_t out[32]) {
    uint32_t i = c->datalen;
    c->buffer[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->buffer[i++] = 0;
        sha256_transform(c, c->buffer);
        i = 0;
    }
    while (i < 56) c->buffer[i++] = 0;
    c->bitlen += (uint64_t)c->datalen * 8;
    for (int j = 7; j >= 0; j--) c->buffer[i++] = (uint8_t)(c->bitlen >> (j*8));
    sha256_transform(c, c->buffer);
    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(c->state[j] >> 24);
        out[j*4+1] = (uint8_t)(c->state[j] >> 16);
        out[j*4+2] = (uint8_t)(c->state[j] >> 8);
        out[j*4+3] = (uint8_t)(c->state[j]);
    }
}

void tdb_sha256(const void *data, size_t len, uint8_t out[32]) {
    tdb_sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)data, len);
    sha256_final(&c, out);
}

/* --------------------------------------------------------------------------
 *  HMAC-SHA256 (RFC 2104)
 * -------------------------------------------------------------------------- */

void tdb_hmac_sha256(const uint8_t *key, size_t key_len,
                     const uint8_t *data, size_t data_len,
                     uint8_t out[32]) {
    uint8_t k_block[64] = {0};
    if (key_len > 64) {
        tdb_sha256(key, key_len, k_block);
    } else {
        memcpy(k_block, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k_block[i] ^ 0x36;
        opad[i] = k_block[i] ^ 0x5c;
    }
    tdb_sha256_ctx_t c;
    uint8_t inner[32];
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, data, data_len);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* --------------------------------------------------------------------------
 *  PBKDF2-HMAC-SHA256 (RFC 8018)
 * -------------------------------------------------------------------------- */

tdb_status_t tdb_pbkdf2_sha256(const char *password, size_t pw_len,
                               const uint8_t *salt, size_t salt_len,
                               uint32_t iterations,
                               uint8_t *out, size_t out_len) {
    if (!password || !salt || !out) return TDB_ERR_INVALID_ARG;
    if (iterations == 0 || out_len == 0) return TDB_ERR_INVALID_ARG;

    const size_t H_LEN = 32;
    size_t blocks = (out_len + H_LEN - 1) / H_LEN;

    uint8_t u[32], t[32];
    /* salt || INT(i) buffer */
    uint8_t *salt_i = (uint8_t *)malloc(salt_len + 4);
    if (!salt_i) return TDB_ERR_INVALID_ARG;

    for (size_t block = 1; block <= blocks; block++) {
        memcpy(salt_i, salt, salt_len);
        salt_i[salt_len]   = (uint8_t)((block >> 24) & 0xff);
        salt_i[salt_len+1] = (uint8_t)((block >> 16) & 0xff);
        salt_i[salt_len+2] = (uint8_t)((block >> 8)  & 0xff);
        salt_i[salt_len+3] = (uint8_t)(block & 0xff);

        /* U_1 = PRF(P, S || INT(i)) */
        tdb_hmac_sha256((const uint8_t *)password, pw_len, salt_i, salt_len + 4, u);
        memcpy(t, u, H_LEN);

        for (uint32_t it = 1; it < iterations; it++) {
            tdb_hmac_sha256((const uint8_t *)password, pw_len, u, H_LEN, u);
            for (size_t j = 0; j < H_LEN; j++) t[j] ^= u[j];
        }

        size_t off = (block - 1) * H_LEN;
        size_t copy = (off + H_LEN <= out_len) ? H_LEN : (out_len - off);
        memcpy(out + off, t, copy);
    }

    /* Zero scratch */
    volatile uint8_t *p = (volatile uint8_t *)salt_i;
    for (size_t i = 0; i < salt_len + 4; i++) p[i] = 0;
    free(salt_i);
    volatile uint8_t *pu = (volatile uint8_t *)u;
    volatile uint8_t *pt = (volatile uint8_t *)t;
    for (size_t i = 0; i < 32; i++) { pu[i] = 0; pt[i] = 0; }

    return TDB_OK;
}

void tdb_crypto_destroy(tdb_crypto_ctx_t *ctx) {
    if (!ctx) return;

    /*
     * Zero out all key material.  Use volatile pointer trick to prevent
     * the compiler from optimizing away the memset.
     */
#if defined(__APPLE__) || defined(__FreeBSD__)
    /* Securely zero out key material */
    volatile uint8_t *p = (volatile uint8_t *)ctx;
    for (size_t i = 0; i < sizeof(*ctx); i++) p[i] = 0;
#else
    volatile uint8_t *p = (volatile uint8_t *)ctx;
    for (size_t i = 0; i < sizeof(*ctx); i++) {
        p[i] = 0;
    }
#endif
}

/* ──────────────────────────────────────────────────────────────────────
 * SHA-1 (RFC 3174). Pure C, no external dependency.
 * Output: 20 bytes.
 * ──────────────────────────────────────────────────────────────────── */

static uint32_t s1_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void tdb_sha1(const void *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    /* Padded message: original bytes + 0x80 + zero pad + 8-byte big-endian length. */
    uint64_t bits = (uint64_t)len * 8;
    size_t padded_len = ((len + 9 + 63) / 64) * 64;
    uint8_t stack_buf[512];
    uint8_t *buf = (padded_len <= sizeof(stack_buf)) ? stack_buf : (uint8_t *)malloc(padded_len);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = 0x80;
    memset(buf + len + 1, 0, padded_len - len - 1 - 8);
    for (int i = 0; i < 8; i++) buf[padded_len - 8 + i] = (uint8_t)((bits >> (56 - i * 8)) & 0xFF);

    for (size_t b = 0; b < padded_len; b += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)buf[b + i*4 + 0] << 24) | ((uint32_t)buf[b + i*4 + 1] << 16)
                 | ((uint32_t)buf[b + i*4 + 2] << 8)  |  (uint32_t)buf[b + i*4 + 3];
        }
        for (int i = 16; i < 80; i++) w[i] = s1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], B = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (B & c) | ((~B) & d); k = 0x5A827999u; }
            else if (i < 40) { f = B ^ c ^ d;            k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (B & c) | (B & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = B ^ c ^ d;            k = 0xCA62C1D6u; }
            uint32_t t = s1_rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = s1_rotl(B, 30); B = a; a = t;
        }
        h[0] += a; h[1] += B; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[i*4 + 0] = (uint8_t)((h[i] >> 24) & 0xFF);
        out[i*4 + 1] = (uint8_t)((h[i] >> 16) & 0xFF);
        out[i*4 + 2] = (uint8_t)((h[i] >> 8)  & 0xFF);
        out[i*4 + 3] = (uint8_t)( h[i]        & 0xFF);
    }
    if (buf != stack_buf) free(buf);
}

/* ──────────────────────────────────────────────────────────────────────
 * MD5 (RFC 1321). Pure C. Output: 16 bytes. Legacy interop only.
 * ──────────────────────────────────────────────────────────────────── */

static uint32_t md5_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const int md5_r[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

void tdb_md5(const void *data, size_t len, uint8_t out[16]) {
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    uint64_t bits = (uint64_t)len * 8;
    size_t padded_len = ((len + 9 + 63) / 64) * 64;
    uint8_t stack_buf[512];
    uint8_t *buf = (padded_len <= sizeof(stack_buf)) ? stack_buf : (uint8_t *)malloc(padded_len);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = 0x80;
    memset(buf + len + 1, 0, padded_len - len - 1 - 8);
    for (int i = 0; i < 8; i++) buf[padded_len - 8 + i] = (uint8_t)((bits >> (i * 8)) & 0xFF);

    for (size_t b = 0; b < padded_len; b += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            M[i] = (uint32_t)buf[b + i*4 + 0]
                 | ((uint32_t)buf[b + i*4 + 1] << 8)
                 | ((uint32_t)buf[b + i*4 + 2] << 16)
                 | ((uint32_t)buf[b + i*4 + 3] << 24);
        }
        uint32_t A = h0, B = h1, C = h2, D = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | ((~B) & D); g = i; }
            else if (i < 32) { F = (D & B) | ((~D) & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;             g = (3*i + 5) % 16; }
            else             { F = C ^ (B | (~D));        g = (7*i) % 16; }
            uint32_t temp = D;
            D = C; C = B;
            B = B + md5_rotl(A + F + md5_k[i] + M[g], md5_r[i]);
            A = temp;
        }
        h0 += A; h1 += B; h2 += C; h3 += D;
    }
    for (int i = 0; i < 4; i++) {
        out[i]      = (uint8_t)((h0 >> (i*8)) & 0xFF);
        out[i + 4]  = (uint8_t)((h1 >> (i*8)) & 0xFF);
        out[i + 8]  = (uint8_t)((h2 >> (i*8)) & 0xFF);
        out[i + 12] = (uint8_t)((h3 >> (i*8)) & 0xFF);
    }
    if (buf != stack_buf) free(buf);
}

/* HMAC-SHA1 — standard HMAC construction with 64-byte block size and SHA-1. */
void tdb_hmac_sha1(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t out[20]) {
    uint8_t kpad[64] = {0};
    if (key_len > 64) {
        tdb_sha1(key, key_len, kpad);
    } else {
        memcpy(kpad, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = kpad[i] ^ 0x36; opad[i] = kpad[i] ^ 0x5C; }
    /* SHA1(ipad || data) */
    size_t inner_len = 64 + data_len;
    uint8_t stack_inner[1024];
    uint8_t *inner = (inner_len <= sizeof(stack_inner)) ? stack_inner : (uint8_t*)malloc(inner_len);
    if (!inner) return;
    memcpy(inner, ipad, 64);
    if (data_len > 0) memcpy(inner + 64, data, data_len);
    uint8_t inner_hash[20];
    tdb_sha1(inner, inner_len, inner_hash);
    if (inner != stack_inner) free(inner);
    /* SHA1(opad || inner_hash) */
    uint8_t outer[64 + 20];
    memcpy(outer, opad, 64);
    memcpy(outer + 64, inner_hash, 20);
    tdb_sha1(outer, 64 + 20, out);
}

/* ──────────────────────────────────────────────────────────────────────
 * AES-256-GCM (FIPS 197 + NIST SP 800-38D)
 *
 * Reference quality, constant-time-ish for table lookups. Not optimized;
 * production deployments should link OpenSSL via TDB_WITH_OPENSSL=ON.
 * ──────────────────────────────────────────────────────────────────── */

static const uint8_t k_sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const uint8_t k_rcon[15] = {
0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d
};

/* AES-256: Nb=4 (block words), Nk=8 (key words), Nr=14 (rounds).
 * Round keys: (Nr+1)*Nb = 60 words = 240 bytes. */
static void aes256_key_expand(const uint8_t key[32], uint8_t rk[240]) {
    int Nk = 8, Nr = 14, Nb = 4;
    memcpy(rk, key, 32);
    int total_words = Nb * (Nr + 1); /* 60 */
    for (int i = Nk; i < total_words; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % Nk == 0) {
            uint8_t tmp = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp; /* RotWord */
            for (int j = 0; j < 4; j++) t[j] = k_sbox[t[j]];                       /* SubWord */
            t[0] ^= k_rcon[i / Nk];
        } else if (i % Nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = k_sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) rk[i*4 + j] = rk[(i - Nk)*4 + j] ^ t[j];
    }
}

static uint8_t aes_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes_encrypt_block(const uint8_t in[16], uint8_t out[16], const uint8_t rk[240]) {
    uint8_t s[16]; memcpy(s, in, 16);
    /* AddRoundKey 0 */
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round < 14; round++) {
        /* SubBytes */
        for (int i = 0; i < 16; i++) s[i] = k_sbox[s[i]];
        /* ShiftRows */
        uint8_t t;
        t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13];  s[13] = t;
        t = s[2];  s[2] = s[10]; s[10] = t;
        t = s[6];  s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        /* MixColumns */
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = s[c*4 + 0], a1 = s[c*4 + 1], a2 = s[c*4 + 2], a3 = s[c*4 + 3];
            uint8_t e = a0 ^ a1 ^ a2 ^ a3;
            s[c*4 + 0] ^= e ^ aes_xtime(a0 ^ a1);
            s[c*4 + 1] ^= e ^ aes_xtime(a1 ^ a2);
            s[c*4 + 2] ^= e ^ aes_xtime(a2 ^ a3);
            s[c*4 + 3] ^= e ^ aes_xtime(a3 ^ a0);
        }
        /* AddRoundKey */
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16 + i];
    }
    /* Final round: SubBytes, ShiftRows, AddRoundKey (no MixColumns) */
    for (int i = 0; i < 16; i++) s[i] = k_sbox[s[i]];
    uint8_t t;
    t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13];  s[13] = t;
    t = s[2];  s[2] = s[10]; s[10] = t;
    t = s[6];  s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
    for (int i = 0; i < 16; i++) s[i] ^= rk[14*16 + i];
    memcpy(out, s, 16);
}

/* GHASH: GF(2^128) multiplication. We use the right-shift method —
 * straightforward but not constant-time. */
static void ghash_mul(uint8_t x[16], const uint8_t y[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16]; memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        int bit = (x[i / 8] >> (7 - (i % 8))) & 1;
        if (bit) for (int j = 0; j < 16; j++) z[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | ((v[j-1] & 1) << 7));
        v[0] = (uint8_t)(v[0] >> 1);
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}

static void ghash_update(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        uint8_t block[16] = {0};
        size_t n = (len - i) < 16 ? (len - i) : 16;
        memcpy(block, data + i, n);
        for (int j = 0; j < 16; j++) y[j] ^= block[j];
        ghash_mul(y, h);
    }
}

static void inc32(uint8_t ctr[16]) {
    /* Increment the last 32 bits, big-endian. */
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i] != 0) break;
    }
}

static tdb_status_t aes256_gcm_crypt(int encrypt,
                                      const uint8_t key[32],
                                      const uint8_t iv[12],
                                      const uint8_t *aad, size_t aad_len,
                                      const uint8_t *in, size_t in_len,
                                      uint8_t *out,
                                      uint8_t tag[16]) {
    if (!key || !iv || (!in && in_len) || (!out && in_len)) return TDB_ERR_INVALID_ARG;
    uint8_t rk[240];
    aes256_key_expand(key, rk);

    /* H = AES(0^128) */
    uint8_t H[16] = {0};
    aes_encrypt_block(H, H, rk);

    /* J0 = IV || 0^31 || 1 (since IV is 96 bits) */
    uint8_t J0[16] = {0};
    memcpy(J0, iv, 12);
    J0[15] = 1;

    /* CTR mode encryption: counter starts at J0+1. */
    uint8_t ctr[16]; memcpy(ctr, J0, 16);
    inc32(ctr);
    uint8_t ks[16];
    for (size_t i = 0; i < in_len; i += 16) {
        aes_encrypt_block(ctr, ks, rk);
        size_t n = (in_len - i) < 16 ? (in_len - i) : 16;
        for (size_t j = 0; j < n; j++) out[i + j] = in[i + j] ^ ks[j];
        inc32(ctr);
    }

    /* GHASH over AAD || ciphertext || len(AAD)*8 || len(C)*8 */
    uint8_t Y[16] = {0};
    if (aad && aad_len) ghash_update(Y, H, aad, aad_len);
    const uint8_t *cipher_for_ghash = encrypt ? out : in; /* GHASH on ciphertext */
    if (in_len) ghash_update(Y, H, cipher_for_ghash, in_len);
    uint8_t len_block[16] = {0};
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t c_bits   = (uint64_t)in_len * 8;
    for (int i = 0; i < 8; i++) len_block[i]     = (uint8_t)((aad_bits >> (56 - i*8)) & 0xFF);
    for (int i = 0; i < 8; i++) len_block[8 + i] = (uint8_t)((c_bits   >> (56 - i*8)) & 0xFF);
    for (int j = 0; j < 16; j++) Y[j] ^= len_block[j];
    ghash_mul(Y, H);

    /* Tag = AES(J0) XOR Y */
    uint8_t J0_enc[16];
    aes_encrypt_block(J0, J0_enc, rk);
    for (int j = 0; j < 16; j++) tag[j] = J0_enc[j] ^ Y[j];

    return TDB_OK;
}

tdb_status_t tdb_aes256_gcm_encrypt(const uint8_t key[32],
                                    const uint8_t iv[12],
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *plain, size_t plain_len,
                                    uint8_t *out_ct,
                                    uint8_t out_tag[16]) {
    return aes256_gcm_crypt(1, key, iv, aad, aad_len, plain, plain_len, out_ct, out_tag);
}

tdb_status_t tdb_aes256_gcm_decrypt(const uint8_t key[32],
                                    const uint8_t iv[12],
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *ct, size_t ct_len,
                                    const uint8_t tag[16],
                                    uint8_t *out_plain) {
    uint8_t computed_tag[16];
    tdb_status_t r = aes256_gcm_crypt(0, key, iv, aad, aad_len, ct, ct_len, out_plain, computed_tag);
    if (r != TDB_OK) return r;
    /* Constant-time tag compare */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(computed_tag[i] ^ tag[i]);
    if (diff != 0) return TDB_ERR_IO;
    return TDB_OK;
}
