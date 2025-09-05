// SPDX-License-Identifier: GPL-3.0-or-later
//
// crypto.c — primitives crypto C17 portables pour Vitte Light
//
// Fournit :
// - SHA-256 (init/update/final) — implémentation sans dépendance
// - HMAC-SHA256
// - PBKDF2-HMAC-SHA256
// - Base16 (hex) encode/decode
// - Base64 encode/decode
// - Comparaison constante (ct_equal)
// - RNG: wrapper sur aux_rand_bytes()
// - Utilitaires: xor_inplace
//
// Correspond à l’en-tête: includes/crypto.h

#include "crypto.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"

// ============================================================
// Const-time compare
// ============================================================

int vl_crypto_ct_equal(const void *a, const void *b, size_t n) {
  const uint8_t *pa = (const uint8_t *)a;
  const uint8_t *pb = (const uint8_t *)b;
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i++) acc |= (uint32_t)(pa[i] ^ pb[i]);
  return acc == 0;
}

// ============================================================
// RNG
// ============================================================

int vl_crypto_random_bytes(void *out, size_t n) {
  return aux_rand_bytes(out, n) == AUX_OK ? 0 : -1;
}

// ============================================================
// SHA-256 (FIPS 180-4) — implé minimal, domaine public style
// ============================================================

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR((x), 2) ^ ROTR((x), 13) ^ ROTR((x), 22))
#define BSIG1(x) (ROTR((x), 6) ^ ROTR((x), 11) ^ ROTR((x), 25))
#define SSIG0(x) (ROTR((x), 7) ^ ROTR((x), 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR((x), 17) ^ ROTR((x), 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

void vl_sha256_init(vl_sha256_ctx *c) {
  c->h[0] = 0x6a09e667U;
  c->h[1] = 0xbb67ae85U;
  c->h[2] = 0x3c6ef372U;
  c->h[3] = 0xa54ff53aU;
  c->h[4] = 0x510e527fU;
  c->h[5] = 0x9b05688cU;
  c->h[6] = 0x1f83d9abU;
  c->h[7] = 0x5be0cd19U;
  c->len = 0;
  c->buf_len = 0;
}

static void sha256_compress(vl_sha256_ctx *c, const uint8_t block[64]) {
  uint32_t w[64], a, b, d, e, f, g, h;
  uint32_t t1, t2;
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
           ((uint32_t)block[4 * i + 2] << 8) | ((uint32_t)block[4 * i + 3]);
  }
  for (int i = 16; i < 64; i++)
    w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

  a = c->h[0];
  b = c->h[1];
  uint32_t ccc = c->h[2];
  d = c->h[3];
  e = c->h[4];
  f = c->h[5];
  g = c->h[6];
  h = c->h[7];

  for (int i = 0; i < 64; i++) {
    t1 = h + BSIG1(e) + CH(e, f, g) + K256[i] + w[i];
    t2 = BSIG0(a) + MAJ(a, b, ccc);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = ccc;
    ccc = b;
    b = a;
    a = t1 + t2;
  }

  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += ccc;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}

void vl_sha256_update(vl_sha256_ctx *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  c->len += (uint64_t)len;
  if (c->buf_len) {
    size_t need = 64 - c->buf_len;
    if (need > len) need = len;
    memcpy(c->buf + c->buf_len, p, need);
    c->buf_len += need;
    p += need;
    len -= need;
    if (c->buf_len == 64) {
      sha256_compress(c, c->buf);
      c->buf_len = 0;
    }
  }
  while (len >= 64) {
    sha256_compress(c, p);
    p += 64;
    len -= 64;
  }
  if (len) {
    memcpy(c->buf, p, len);
    c->buf_len = len;
  }
}

void vl_sha256_final(vl_sha256_ctx *c, uint8_t out[32]) {
  uint64_t bit_len = c->len * 8;
  // padding: 0x80 then zeros, then 64-bit big-endian length
  uint8_t pad[128] = {0x80};
  size_t padlen =
      (c->buf_len < 56) ? (56 - c->buf_len) : (56 + 64 - c->buf_len);
  vl_sha256_update(c, pad, padlen);
  uint8_t lenbe[8];
  for (int i = 0; i < 8; i++) lenbe[7 - i] = (uint8_t)(bit_len >> (8 * i));
  vl_sha256_update(c, lenbe, 8);

  for (int i = 0; i < 8; i++) {
    out[4 * i + 0] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)(c->h[i]);
  }
  // wipe context
  memset(c, 0, sizeof *c);
}

// One-shot
void vl_sha256(const void *msg, size_t len, uint8_t out[32]) {
  vl_sha256_ctx c;
  vl_sha256_init(&c);
  vl_sha256_update(&c, msg, len);
  vl_sha256_final(&c, out);
}

// ============================================================
// HMAC-SHA256 (RFC 2104)
// ============================================================

void vl_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                    size_t msg_len, uint8_t out[32]) {
  uint8_t k_ipad[64], k_opad[64], tk[32];

  if (key_len > 64) {
    vl_sha256(key, key_len, tk);
    key = tk;
    key_len = 32;
  }
  memset(k_ipad, 0x36, 64);
  memset(k_opad, 0x5c, 64);
  for (size_t i = 0; i < key_len; i++) {
    k_ipad[i] ^= key[i];
    k_opad[i] ^= key[i];
  }

  vl_sha256_ctx ctx;
  uint8_t inner[32];

  vl_sha256_init(&ctx);
  vl_sha256_update(&ctx, k_ipad, 64);
  vl_sha256_update(&ctx, msg, msg_len);
  vl_sha256_final(&ctx, inner);

  vl_sha256_init(&ctx);
  vl_sha256_update(&ctx, k_opad, 64);
  vl_sha256_update(&ctx, inner, 32);
  vl_sha256_final(&ctx, out);

  // wipe
  memset(k_ipad, 0, sizeof k_ipad);
  memset(k_opad, 0, sizeof k_opad);
  memset(tk, 0, sizeof tk);
  memset(inner, 0, sizeof inner);
}

// ============================================================
// PBKDF2-HMAC-SHA256 (RFC 8018)
// ============================================================

int vl_pbkdf2_hmac_sha256(const uint8_t *pw, size_t pwlen, const uint8_t *salt,
                          size_t saltlen, uint32_t iters, uint8_t *out,
                          size_t outlen) {
  if (iters == 0) return -1;
  uint32_t blocks = (uint32_t)((outlen + 31) / 32);
  uint8_t U[32], T[32];
  uint8_t *buf = NULL;
  size_t buflen = saltlen + 4;
  buf = (uint8_t *)malloc(buflen);
  if (!buf) return -1;
  memcpy(buf, salt, saltlen);

  for (uint32_t i = 1; i <= blocks; i++) {
    // INT(i) big-endian
    buf[saltlen + 0] = (uint8_t)(i >> 24);
    buf[saltlen + 1] = (uint8_t)(i >> 16);
    buf[saltlen + 2] = (uint8_t)(i >> 8);
    buf[saltlen + 3] = (uint8_t)(i);

    vl_hmac_sha256(pw, pwlen, buf, buflen, U);
    memcpy(T, U, 32);

    for (uint32_t j = 2; j <= iters; j++) {
      vl_hmac_sha256(pw, pwlen, U, 32, U);
      for (int k = 0; k < 32; k++) T[k] ^= U[k];
    }

    size_t off = (size_t)(i - 1) * 32;
    size_t take = (outlen - off >= 32) ? 32 : (outlen - off);
    memcpy(out + off, T, take);
  }

  memset(U, 0, sizeof U);
  memset(T, 0, sizeof T);
  if (buf) {
    memset(buf, 0, buflen);
    free(buf);
  }
  return 0;
}

// ============================================================
// HEX
// ============================================================

static inline int hex_val(unsigned char c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
  return -1;
}

void vl_hex_encode(const void *data, size_t n, char *out) {
  static const char *D = "0123456789abcdef";
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < n; i++) {
    out[2 * i + 0] = D[p[i] >> 4];
    out[2 * i + 1] = D[p[i] & 0x0F];
  }
  out[2 * n] = 0;
}

int vl_hex_decode(const char *hex, uint8_t *out, size_t *out_len) {
  size_t n = strlen(hex);
  if ((n & 1) != 0) return -1;
  size_t want = n / 2;
  if (out_len && *out_len < want) return -1;
  for (size_t i = 0; i < want; i++) {
    int hi = hex_val((unsigned char)hex[2 * i]);
    int lo = hex_val((unsigned char)hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  if (out_len) *out_len = want;
  return 0;
}

// ============================================================
// Base64 (RFC 4648) — sans sauts de ligne
// ============================================================

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t B64_DEC[256] = {
/* 0..255 table, 0xFF=invalid */
#define XX 0xFF
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, 62, XX, XX, XX, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    61, XX, XX, XX, 0, XX, XX, XX, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, XX, XX, XX, XX, XX, XX, 26,
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, XX, XX, XX, XX, XX,
    /* remaining all invalid */
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX
#undef XX
};

size_t vl_base64_encode_len(size_t n) { return 4 * ((n + 2) / 3); }

void vl_base64_encode(const void *src, size_t n, char *out) {
  const uint8_t *p = (const uint8_t *)src;
  size_t i = 0, o = 0;
  while (i + 3 <= n) {
    uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
    out[o++] = B64_ENC[(v >> 18) & 63];
    out[o++] = B64_ENC[(v >> 12) & 63];
    out[o++] = B64_ENC[(v >> 6) & 63];
    out[o++] = B64_ENC[v & 63];
    i += 3;
  }
  if (i + 1 == n) {
    uint32_t v = (uint32_t)p[i] << 16;
    out[o++] = B64_ENC[(v >> 18) & 63];
    out[o++] = B64_ENC[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (i + 2 == n) {
    uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
    out[o++] = B64_ENC[(v >> 18) & 63];
    out[o++] = B64_ENC[(v >> 12) & 63];
    out[o++] = B64_ENC[(v >> 6) & 63];
    out[o++] = '=';
  }
  out[o] = 0;
}

int vl_base64_decode(const char *in, uint8_t *out, size_t *out_len) {
  size_t n = strlen(in);
  if (n % 4 != 0) return -1;

  size_t outcap = out_len ? *out_len : 0;
  size_t o = 0;

  for (size_t i = 0; i < n; i += 4) {
    uint8_t a = B64_DEC[(unsigned char)in[i + 0]];
    uint8_t b = B64_DEC[(unsigned char)in[i + 1]];
    uint8_t c = (in[i + 2] == '=') ? 64 : B64_DEC[(unsigned char)in[i + 2]];
    uint8_t d = (in[i + 3] == '=') ? 64 : B64_DEC[(unsigned char)in[i + 3]];
    if (a == 0xFF || b == 0xFF || (c == 0xFF && in[i + 2] != '=') ||
        (d == 0xFF && in[i + 3] != '=')) {
      return -1;
    }
    uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                 ((c == 64 ? 0 : (uint32_t)c) << 6) |
                 (d == 64 ? 0 : (uint32_t)d);
    // byte 0
    if (out) {
      if (o >= outcap) return -1;
      out[o] = (uint8_t)((v >> 16) & 0xFF);
    }
    o++;
    // byte 1
    if (in[i + 2] != '=') {
      if (out) {
        if (o >= outcap) return -1;
        out[o] = (uint8_t)((v >> 8) & 0xFF);
      }
      o++;
    }
    // byte 2
    if (in[i + 3] != '=') {
      if (out) {
        if (o >= outcap) return -1;
        out[o] = (uint8_t)(v & 0xFF);
      }
      o++;
    }
  }

  if (out_len) *out_len = o;
  return 0;
}

// ============================================================
// XOR
// ============================================================

void vl_crypto_xor_inplace(uint8_t *dst, const uint8_t *src, size_t n) {
  for (size_t i = 0; i < n; i++) dst[i] ^= src[i];
}

// ============================================================
// Fin
// ============================================================
