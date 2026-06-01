/*
** tdb_crypto.c — cryptography SQL functions, registered through the public
** plugin API. Hashes/CRC/hex are computed in-tree (no external dependency);
** AES and a CSPRNG are layered on when built against OpenSSL.
*/
#include "tdb_crypto.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef TDB_HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#endif

/* ============================ SHA-256 ================================= */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_ctx;

static uint32_t rotr32(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }

static const uint32_t SHA256_K[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void sha256_init(sha256_ctx *c) {
  static const uint32_t iv[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                                 0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
  memcpy(c->h, iv, sizeof(iv));
  c->len = 0; c->n = 0;
}

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
           (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i-15],7) ^ rotr32(w[i-15],18) ^ (w[i-15] >> 3);
    uint32_t s1 = rotr32(w[i-2],17) ^ rotr32(w[i-2],19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
    uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
  }
  c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
  c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_update(sha256_ctx *c, const uint8_t *p, size_t n) {
  c->len += (uint64_t)n;
  while (n) {
    size_t take = 64 - c->n; if (take > n) take = n;
    memcpy(c->buf + c->n, p, take);
    c->n += take; p += take; n -= take;
    if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
  }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->n != 56) sha256_update(c, &zero, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i*8));
  sha256_update(c, lb, 8);
  for (int i = 0; i < 8; i++) {
    out[i*4]   = (uint8_t)(c->h[i] >> 24);
    out[i*4+1] = (uint8_t)(c->h[i] >> 16);
    out[i*4+2] = (uint8_t)(c->h[i] >> 8);
    out[i*4+3] = (uint8_t)(c->h[i]);
  }
}

static void sha256(const uint8_t *p, size_t n, uint8_t out[32]) {
  sha256_ctx c; sha256_init(&c); sha256_update(&c, p, n); sha256_final(&c, out);
}

/* HMAC-SHA256 (RFC 2104) */
static void hmac_sha256(const uint8_t *key, size_t klen,
                        const uint8_t *msg, size_t mlen, uint8_t out[32]) {
  uint8_t k[64]; memset(k, 0, sizeof(k));
  if (klen > 64) { sha256(key, klen, k); }
  else memcpy(k, key, klen);
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) { ipad[i] = (uint8_t)(k[i] ^ 0x36); opad[i] = (uint8_t)(k[i] ^ 0x5c); }
  uint8_t inner[32];
  sha256_ctx c; sha256_init(&c);
  sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
  sha256_init(&c);
  sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

/* ============================== MD5 =================================== */

typedef struct { uint32_t a,b,c,d; uint64_t len; uint8_t buf[64]; size_t n; } md5_ctx;

static uint32_t rotl32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static void md5_block(md5_ctx *ctx, const uint8_t *p) {
  static const uint32_t S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
  static const uint32_t K[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u};
  uint32_t m[16];
  for (int i = 0; i < 16; i++)
    m[i] = (uint32_t)p[i*4] | (uint32_t)p[i*4+1] << 8 | (uint32_t)p[i*4+2] << 16 | (uint32_t)p[i*4+3] << 24;
  uint32_t a=ctx->a,b=ctx->b,c=ctx->c,d=ctx->d;
  for (int i = 0; i < 64; i++) {
    uint32_t f; int g;
    if (i < 16)      { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5*i+1) & 15; }
    else if (i < 48) { f = b ^ c ^ d;          g = (3*i+5) & 15; }
    else             { f = c ^ (b | ~d);       g = (7*i) & 15; }
    f = f + a + K[i] + m[g];
    a = d; d = c; c = b; b = b + rotl32(f, (int)S[i]);
  }
  ctx->a+=a; ctx->b+=b; ctx->c+=c; ctx->d+=d;
}

static void md5(const uint8_t *p, size_t n, uint8_t out[16]) {
  md5_ctx ctx; ctx.a=0x67452301u; ctx.b=0xefcdab89u; ctx.c=0x98badcfeu; ctx.d=0x10325476u;
  ctx.len = (uint64_t)n; ctx.n = 0;
  size_t full = n / 64;
  for (size_t i = 0; i < full; i++) md5_block(&ctx, p + i*64);
  uint8_t tail[128]; size_t r = n - full*64;
  memcpy(tail, p + full*64, r);
  tail[r++] = 0x80;
  size_t padto = (r <= 56) ? 56 : 120;
  while (r < padto) tail[r++] = 0;
  uint64_t bits = (uint64_t)n * 8;
  for (int i = 0; i < 8; i++) tail[r++] = (uint8_t)(bits >> (i*8));
  for (size_t i = 0; i < r; i += 64) md5_block(&ctx, tail + i);
  uint32_t v[4] = {ctx.a, ctx.b, ctx.c, ctx.d};
  for (int i = 0; i < 4; i++) {
    out[i*4]   = (uint8_t)(v[i]);
    out[i*4+1] = (uint8_t)(v[i] >> 8);
    out[i*4+2] = (uint8_t)(v[i] >> 16);
    out[i*4+3] = (uint8_t)(v[i] >> 24);
  }
}

/* =============================== SHA-1 ================================ */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static void sha1_block(sha1_ctx *c, const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
           (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
  for (int i = 16; i < 80; i++) {
    uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
    w[i] = (v << 1) | (v >> 31);
  }
  uint32_t a=c->h[0], b=c->h[1], cc=c->h[2], d=c->h[3], e=c->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5a827999u; }
    else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ed9eba1u; }
    else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);k = 0x8f1bbcdcu; }
    else             { f = b ^ cc ^ d;                   k = 0xca62c1d6u; }
    uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
    e = d; d = cc; cc = (b << 30) | (b >> 2); b = a; a = t;
  }
  c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e;
}

static void sha1(const uint8_t *p, size_t n, uint8_t out[20]) {
  sha1_ctx c;
  c.h[0]=0x67452301u; c.h[1]=0xefcdab89u; c.h[2]=0x98badcfeu;
  c.h[3]=0x10325476u; c.h[4]=0xc3d2e1f0u;
  c.len = (uint64_t)n; c.n = 0;
  size_t full = n / 64;
  for (size_t i = 0; i < full; i++) sha1_block(&c, p + i*64);
  uint8_t tail[128]; size_t r = n - full*64;
  memcpy(tail, p + full*64, r);
  tail[r++] = 0x80;
  size_t padto = (r <= 56) ? 56 : 120;
  while (r < padto) tail[r++] = 0;
  uint64_t bits = (uint64_t)n * 8;
  for (int i = 0; i < 8; i++) tail[r++] = (uint8_t)(bits >> (56 - i*8));
  for (size_t i = 0; i < r; i += 64) sha1_block(&c, tail + i);
  for (int i = 0; i < 5; i++) {
    out[i*4]   = (uint8_t)(c.h[i] >> 24);
    out[i*4+1] = (uint8_t)(c.h[i] >> 16);
    out[i*4+2] = (uint8_t)(c.h[i] >> 8);
    out[i*4+3] = (uint8_t)(c.h[i]);
  }
}

/* ============================== CRC32 ================================= */

static uint32_t crc32_bytes(const uint8_t *p, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return crc ^ 0xffffffffu;
}

/* ============================ helpers ================================= */

static const unsigned char *arg_bytes(tdb_value *v, int *n) {
  if (tdb_value_type(v) == TDB_BLOB) {
    *n = tdb_value_bytes(v);
    const void *b = tdb_value_blob(v);
    return (const unsigned char *)(b ? b : (const void *)"");
  }
  const char *s = tdb_value_text(v);
  *n = s ? (int)strlen(s) : 0;
  return (const unsigned char *)(s ? s : "");
}

static void result_hex(tdb_context *ctx, const uint8_t *p, int n) {
  static const char hexd[] = "0123456789abcdef";
  char stackbuf[129];
  char *buf = (n * 2 + 1 <= (int)sizeof(stackbuf)) ? stackbuf : (char *)malloc((size_t)n * 2 + 1);
  if (!buf) { tdb_result_error(ctx, "out of memory"); return; }
  for (int i = 0; i < n; i++) {
    buf[i*2]   = hexd[(p[i] >> 4) & 0xf];
    buf[i*2+1] = hexd[p[i] & 0xf];
  }
  buf[n*2] = '\0';
  tdb_result_text(ctx, buf, n * 2);
  if (buf != stackbuf) free(buf);
}

/* ============================ functions =============================== */

static void fn_sha1(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc; int n; const unsigned char *p = arg_bytes(argv[0], &n);
  uint8_t d[20]; sha1(p, (size_t)n, d); result_hex(ctx, d, 20);
}

static void fn_sha256(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc; int n; const unsigned char *p = arg_bytes(argv[0], &n);
  uint8_t d[32]; sha256(p, (size_t)n, d); result_hex(ctx, d, 32);
}

static void fn_md5(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc; int n; const unsigned char *p = arg_bytes(argv[0], &n);
  uint8_t d[16]; md5(p, (size_t)n, d); result_hex(ctx, d, 16);
}

static void fn_hmac_sha256(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  int kn, mn;
  const unsigned char *key = arg_bytes(argv[0], &kn);
  const unsigned char *msg = arg_bytes(argv[1], &mn);
  uint8_t d[32]; hmac_sha256(key, (size_t)kn, msg, (size_t)mn, d);
  result_hex(ctx, d, 32);
}

static void fn_crc32(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc; int n; const unsigned char *p = arg_bytes(argv[0], &n);
  tdb_result_int64(ctx, (int64_t)crc32_bytes(p, (size_t)n));
}

static void fn_hex(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc; int n; const unsigned char *p = arg_bytes(argv[0], &n);
  result_hex(ctx, p, n);
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void fn_unhex(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  const char *s = tdb_value_text(argv[0]);
  if (!s) { tdb_result_null(ctx); return; }
  size_t len = strlen(s);
  if (len % 2) { tdb_result_error(ctx, "unhex: odd-length string"); return; }
  uint8_t *out = (uint8_t *)malloc(len / 2 + 1);
  if (!out) { tdb_result_error(ctx, "out of memory"); return; }
  for (size_t i = 0; i < len; i += 2) {
    int hi = hexval((unsigned char)s[i]), lo = hexval((unsigned char)s[i+1]);
    if (hi < 0 || lo < 0) { free(out); tdb_result_error(ctx, "unhex: invalid hex digit"); return; }
    out[i/2] = (uint8_t)(hi << 4 | lo);
  }
  tdb_result_blob(ctx, out, (int)(len / 2));
  free(out);
}

/* =============================== Base64 =============================== */

static const char k_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static void fn_base64_encode(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  int n; const unsigned char *p = arg_bytes(argv[0], &n);
  size_t olen = (size_t)((n + 2) / 3) * 4;
  char *out = (char *)malloc(olen + 1);
  if (!out) { tdb_result_error(ctx, "out of memory"); return; }
  size_t i = 0, j = 0;
  while ((int)i + 3 <= n) {
    uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i+1] << 8) | (uint32_t)p[i+2];
    out[j++] = k_b64[(v >> 18) & 63];
    out[j++] = k_b64[(v >> 12) & 63];
    out[j++] = k_b64[(v >> 6) & 63];
    out[j++] = k_b64[v & 63];
    i += 3;
  }
  if ((int)i < n) {
    uint32_t v = (uint32_t)p[i] << 16;
    if ((int)i + 1 < n) v |= (uint32_t)p[i+1] << 8;
    out[j++] = k_b64[(v >> 18) & 63];
    out[j++] = k_b64[(v >> 12) & 63];
    out[j++] = ((int)i + 1 < n) ? k_b64[(v >> 6) & 63] : '=';
    out[j++] = '=';
  }
  out[j] = '\0';
  tdb_result_text(ctx, out, (int)j);
  free(out);
}

static void fn_base64_decode(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  const char *s = tdb_value_text(argv[0]);
  if (!s) { tdb_result_null(ctx); return; }
  size_t n = strlen(s);
  uint8_t *out = (uint8_t *)malloc(n + 1);
  if (!out) { tdb_result_error(ctx, "out of memory"); return; }
  size_t j = 0; uint32_t v = 0; int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int c = (unsigned char)s[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int d = b64val(c);
    if (d < 0) { free(out); tdb_result_error(ctx, "base64: invalid character"); return; }
    v = (v << 6) | (uint32_t)d;
    bits += 6;
    if (bits >= 8) { bits -= 8; out[j++] = (uint8_t)((v >> bits) & 0xff); }
  }
  tdb_result_blob(ctx, out, (int)j);
  free(out);
}

static void fn_randomblob(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  int64_t n = tdb_value_int64(argv[0]);
  if (n <= 0 || n > (1 << 24)) { tdb_result_error(ctx, "randomblob: bad length"); return; }
  uint8_t *out = (uint8_t *)malloc((size_t)n);
  if (!out) { tdb_result_error(ctx, "out of memory"); return; }
#ifdef TDB_HAVE_OPENSSL
  if (RAND_bytes(out, (int)n) != 1) { free(out); tdb_result_error(ctx, "RAND_bytes failed"); return; }
#else
  /* non-cryptographic fallback PRNG (xorshift64*) seeded from the clock */
  static uint64_t st = 0;
  if (!st) st = (uint64_t)time(NULL) ^ 0x9e3779b97f4a7c15ull;
  for (int64_t i = 0; i < n; i++) {
    st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
    out[i] = (uint8_t)((st * 0x2545f4914f6cdd1dull) >> 56);
  }
#endif
  tdb_result_blob(ctx, out, (int)n);
  free(out);
}

#ifdef TDB_HAVE_OPENSSL
/* aes_encrypt(plaintext, key) / aes_decrypt(ciphertext, key) — AES-256-CBC with
** a zero IV prepended-key-derived scheme is intentionally simple: the 32-byte
** key is SHA-256(key) and the IV is the first 16 bytes of SHA-256(key||1). */
static void aes_keyiv(tdb_value *kv, uint8_t key[32], uint8_t iv[16]) {
  int kn; const unsigned char *k = arg_bytes(kv, &kn);
  sha256(k, (size_t)kn, key);
  uint8_t tmp[64]; memcpy(tmp, key, 32); tmp[32] = 1;
  uint8_t d2[32]; sha256(tmp, 33, d2); memcpy(iv, d2, 16);
}

static void fn_aes(tdb_context *ctx, int enc, tdb_value **argv) {
  int dn; const unsigned char *data = arg_bytes(argv[0], &dn);
  uint8_t key[32], iv[16]; aes_keyiv(argv[1], key, iv);
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  if (!c) { tdb_result_error(ctx, "EVP_CIPHER_CTX_new failed"); return; }
  uint8_t *out = (uint8_t *)malloc((size_t)dn + 32);
  int outl = 0, finl = 0, ok = 1;
  if (out &&
      EVP_CipherInit_ex(c, EVP_aes_256_cbc(), NULL, key, iv, enc) == 1 &&
      EVP_CipherUpdate(c, out, &outl, data, dn) == 1 &&
      EVP_CipherFinal_ex(c, out + outl, &finl) == 1) {
    tdb_result_blob(ctx, out, outl + finl);
  } else ok = 0;
  EVP_CIPHER_CTX_free(c);
  free(out);
  if (!ok) tdb_result_error(ctx, enc ? "aes_encrypt failed" : "aes_decrypt failed");
}

static void fn_aes_encrypt(tdb_context *ctx, int argc, tdb_value **argv) { (void)argc; fn_aes(ctx, 1, argv); }
static void fn_aes_decrypt(tdb_context *ctx, int argc, tdb_value **argv) { (void)argc; fn_aes(ctx, 0, argv); }

/* sha512 / sha384 — hex-encoded digest, OpenSSL-backed. */
static void fn_sha_evp(tdb_context *ctx, int argc, tdb_value **argv, const EVP_MD *md, int dlen) {
  (void)argc;
  int n; const unsigned char *p = arg_bytes(argv[0], &n);
  unsigned char d[64]; unsigned int outl = 0;
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  if (!c || EVP_DigestInit_ex(c, md, NULL) != 1 ||
      EVP_DigestUpdate(c, p, (size_t)n) != 1 ||
      EVP_DigestFinal_ex(c, d, &outl) != 1) {
    if (c) EVP_MD_CTX_free(c);
    tdb_result_error(ctx, "EVP digest failed");
    return;
  }
  EVP_MD_CTX_free(c);
  result_hex(ctx, d, (int)outl < dlen ? (int)outl : dlen);
}
static void fn_sha512(tdb_context *ctx, int argc, tdb_value **argv) { fn_sha_evp(ctx, argc, argv, EVP_sha512(), 64); }
static void fn_sha384(tdb_context *ctx, int argc, tdb_value **argv) { fn_sha_evp(ctx, argc, argv, EVP_sha384(), 48); }

/* AES-256-GCM authenticated encryption. The 12-byte IV is randomly generated
** and prepended to the ciphertext; the 16-byte tag is appended. Key is
** SHA-256(passphrase). aes_gcm_encrypt(plaintext, key) returns iv||ct||tag;
** aes_gcm_decrypt(blob, key) reverses it (or returns NULL on auth failure). */
static void fn_aes_gcm_encrypt(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  int dn; const unsigned char *data = arg_bytes(argv[0], &dn);
  int kn; const unsigned char *kp = arg_bytes(argv[1], &kn);
  uint8_t key[32]; sha256(kp, (size_t)kn, key);
  uint8_t iv[12]; if (RAND_bytes(iv, 12) != 1) { tdb_result_error(ctx, "RAND_bytes failed"); return; }
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  uint8_t *out = (uint8_t *)malloc((size_t)dn + 12 + 16);
  int outl = 0, finl = 0, ok = 0;
  if (c && out &&
      EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
      EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
      EVP_EncryptInit_ex(c, NULL, NULL, key, iv) == 1 &&
      EVP_EncryptUpdate(c, out + 12, &outl, data, dn) == 1 &&
      EVP_EncryptFinal_ex(c, out + 12 + outl, &finl) == 1 &&
      EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + 12 + outl + finl) == 1) {
    memcpy(out, iv, 12);
    tdb_result_blob(ctx, out, 12 + outl + finl + 16);
    ok = 1;
  }
  if (c) EVP_CIPHER_CTX_free(c);
  free(out);
  if (!ok) tdb_result_error(ctx, "aes_gcm_encrypt failed");
}

static void fn_aes_gcm_decrypt(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  int dn; const unsigned char *data = arg_bytes(argv[0], &dn);
  int kn; const unsigned char *kp = arg_bytes(argv[1], &kn);
  if (dn < 12 + 16) { tdb_result_error(ctx, "aes_gcm_decrypt: input too short"); return; }
  uint8_t key[32]; sha256(kp, (size_t)kn, key);
  int ctlen = dn - 12 - 16;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  uint8_t *out = (uint8_t *)malloc((size_t)ctlen + 16);
  int outl = 0, finl = 0, ok = 0;
  if (c && out &&
      EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
      EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
      EVP_DecryptInit_ex(c, NULL, NULL, key, data /* iv */) == 1 &&
      EVP_DecryptUpdate(c, out, &outl, data + 12, ctlen) == 1 &&
      EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void *)(data + 12 + ctlen)) == 1 &&
      EVP_DecryptFinal_ex(c, out + outl, &finl) == 1) {
    tdb_result_blob(ctx, out, outl + finl);
    ok = 1;
  }
  if (c) EVP_CIPHER_CTX_free(c);
  free(out);
  if (!ok) tdb_result_null(ctx);   /* authentication failure -> NULL */
}

/* PBKDF2-HMAC-SHA256(password, salt, iters, dklen) -> derived key blob. */
static void fn_pbkdf2(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  const char *pw = tdb_value_text(argv[0]); if (!pw) pw = "";
  int sn; const unsigned char *salt = arg_bytes(argv[1], &sn);
  int64_t iters = tdb_value_int64(argv[2]);
  int64_t dklen = tdb_value_int64(argv[3]);
  if (iters <= 0 || iters > 10000000 || dklen <= 0 || dklen > 4096) {
    tdb_result_error(ctx, "pbkdf2: bad iters/dklen"); return;
  }
  unsigned char *out = (unsigned char *)malloc((size_t)dklen);
  if (!out) { tdb_result_error(ctx, "out of memory"); return; }
  if (PKCS5_PBKDF2_HMAC(pw, (int)strlen(pw), salt, sn, (int)iters,
                        EVP_sha256(), (int)dklen, out) != 1) {
    free(out); tdb_result_error(ctx, "PKCS5_PBKDF2_HMAC failed"); return;
  }
  tdb_result_blob(ctx, out, (int)dklen);
  free(out);
}
#endif

int tdb_register_crypto(tdb_db *db) {
  tdb_create_function(db, "sha1", 1, fn_sha1, NULL);
  tdb_create_function(db, "sha256", 1, fn_sha256, NULL);
  tdb_create_function(db, "md5", 1, fn_md5, NULL);
  tdb_create_function(db, "hmac_sha256", 2, fn_hmac_sha256, NULL);
  tdb_create_function(db, "crc32", 1, fn_crc32, NULL);
  tdb_create_function(db, "hex", 1, fn_hex, NULL);
  tdb_create_function(db, "unhex", 1, fn_unhex, NULL);
  tdb_create_function(db, "base64_encode", 1, fn_base64_encode, NULL);
  tdb_create_function(db, "base64_decode", 1, fn_base64_decode, NULL);
  tdb_create_function(db, "randomblob", 1, fn_randomblob, NULL);
#ifdef TDB_HAVE_OPENSSL
  tdb_create_function(db, "sha384", 1, fn_sha384, NULL);
  tdb_create_function(db, "sha512", 1, fn_sha512, NULL);
  tdb_create_function(db, "aes_encrypt", 2, fn_aes_encrypt, NULL);
  tdb_create_function(db, "aes_decrypt", 2, fn_aes_decrypt, NULL);
  tdb_create_function(db, "aes_gcm_encrypt", 2, fn_aes_gcm_encrypt, NULL);
  tdb_create_function(db, "aes_gcm_decrypt", 2, fn_aes_gcm_decrypt, NULL);
  tdb_create_function(db, "pbkdf2", 4, fn_pbkdf2, NULL);
#endif
  return TDB_OK;
}
