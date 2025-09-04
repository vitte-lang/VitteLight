// vitte-light/libraries/crypto.c
// Crypto primitives et natives pour VitteLight.
// Inclus: SHA-256, HMAC-SHA256, HKDF-SHA256, PBKDF2-HMAC-SHA256,
//         comparaison constante, génération d'octets aléatoires.
//
// Entrée publique: void vl_register_cryptolib(struct VL_Context *ctx);
// Natives:
//   crypto_sha256(data)                        -> str(32)
//   crypto_hmac_sha256(key, data)             -> str(32)
//   crypto_hkdf_sha256(ikm[,salt][,info], L)  -> str(L), L<=255*32
//   crypto_pbkdf2_sha256(pass, salt, iters, dklen) -> str(dklen)
//   crypto_rand(n)                            -> str(n)
//   crypto_secure_equal(a,b)                  -> bool
//
// Notes: Implémentations de référence minimalistes, sans SIMD. Pas d'AES/AEAD
// ici.
//        Utilisez crypto_rand pour générer des octets aléatoires; il tente
//        /dev/urandom ou CryptGenRandom. Eviter MD5/SHA-1.
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/crypto.c

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"
#include "mem.h"     // VL_Buffer
#include "string.h"  // VL_String, vl_make_strn
#include "tm.h"      // vl_mono_time_ns (fallback seed)

// Optionnel: fonction utilitaire d'aléa fournie par auxlib.c
int vl_rand_bytes(void *buf, size_t n);

// ───────────────────────── VM glue ─────────────────────────
#define RET_NIL()                \
  do {                           \
    if (ret) *(ret) = vlv_nil(); \
    return VL_OK;                \
  } while (0)
#define RET_INT(v)                           \
  do {                                       \
    if (ret) *(ret) = vlv_int((int64_t)(v)); \
    return VL_OK;                            \
  } while (0)
#define RET_BOOL(v)                       \
  do {                                    \
    if (ret) *(ret) = vlv_bool((v) != 0); \
    return VL_OK;                         \
  } while (0)
#define RET_STR_BYTES(p, n)                                             \
  do {                                                                  \
    VL_Value __s = vl_make_strn(ctx, (const char *)(p), (uint32_t)(n)); \
    if (__s.type != VT_STR) return VL_ERR_OOM;                          \
    if (ret) *ret = __s;                                                \
    return VL_OK;                                                       \
  } while (0)
static int need_str(const VL_Value *v) {
  return v && v->type == VT_STR && v->as.s;
}

// ───────────────────────── SHA-256 ─────────────────────────
// RFC 6234 / FIPS 180-4 style, portable.

typedef struct {
  uint32_t h[8];
  uint64_t bits;
  size_t len;
  uint8_t buf[64];
} sha256_ctx;

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}
static uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}
static uint32_t S0(uint32_t x) { return ror(x, 2) ^ ror(x, 13) ^ ror(x, 22); }
static uint32_t S1(uint32_t x) { return ror(x, 6) ^ ror(x, 11) ^ ror(x, 25); }
static uint32_t s0(uint32_t x) { return ror(x, 7) ^ ror(x, 18) ^ (x >> 3); }
static uint32_t s1(uint32_t x) { return ror(x, 17) ^ ror(x, 19) ^ (x >> 10); }

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_init(sha256_ctx *c) {
  static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, H0, sizeof(H0));
  c->bits = 0;
  c->len = 0;
}

static void sha256_compress(sha256_ctx *c, const uint8_t blk[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)blk[4 * i] << 24) | ((uint32_t)blk[4 * i + 1] << 16) |
           ((uint32_t)blk[4 * i + 2] << 8) | ((uint32_t)blk[4 * i + 3]);
  }
  for (int i = 16; i < 64; i++) {
    w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];
  }
  uint32_t a = c->h[0], b = c->h[1], c2 = c->h[2], d = c->h[3], e = c->h[4],
           f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t T1 = h + S1(e) + Ch(e, f, g) + K256[i] + w[i];
    uint32_t T2 = S0(a) + Maj(a, b, c2);
    h = g;
    g = f;
    f = e;
    e = d + T1;
    d = c2;
    c2 = b;
    b = a;
    a = T1 + T2;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += c2;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  c->bits += (uint64_t)n * 8ull;
  if (c->len) {
    size_t m = 64 - c->len;
    size_t k = (n < m) ? n : m;
    memcpy(c->buf + c->len, p, k);
    c->len += k;
    p += k;
    n -= k;
    if (c->len == 64) {
      sha256_compress(c, c->buf);
      c->len = 0;
    }
  }
  while (n >= 64) {
    sha256_compress(c, p);
    p += 64;
    n -= 64;
  }
  if (n) {
    memcpy(c->buf, p, n);
    c->len = n;
  }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
  uint8_t pad[64 + 8];
  size_t padn = 0;
  pad[padn++] = 0x80;
  size_t rem = (c->len + 1) % 64;
  size_t z = (rem <= 56) ? (56 - rem) : (56 + 64 - rem);
  memset(pad + padn, 0, z);
  padn += z;
  uint64_t bebits = ((uint64_t)(c->bits >> 56) & 0xFFull);
  // write 64-bit big-endian bit length
  uint8_t lenbe[8];
  for (int i = 0; i < 8; i++) {
    lenbe[7 - i] = (uint8_t)((c->bits >> (8 * i)) & 0xFFu);
  }
  sha256_update(c, pad, padn);
  sha256_update(c, lenbe, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)(c->h[i]);
  }
}

static void sha256(const void *data, size_t n, uint8_t out[32]) {
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, data, n);
  sha256_final(&c, out);
}

// ───────────────────────── HMAC / HKDF / PBKDF2 ─────────────────────────
static void hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *msg,
                        size_t msglen, uint8_t out[32]) {
  uint8_t k0[64];
  if (keylen > 64) {
    sha256(key, keylen, k0);
    memset(k0 + 32, 0, 32);
  } else {
    memcpy(k0, key, keylen);
    if (keylen < 64) memset(k0 + keylen, 0, 64 - keylen);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = k0[i] ^ 0x36;
    opad[i] = k0[i] ^ 0x5c;
  }
  sha256_ctx c;
  uint8_t ih[32];
  sha256_init(&c);
  sha256_update(&c, ipad, 64);
  sha256_update(&c, msg, msglen);
  sha256_final(&c, ih);
  sha256_init(&c);
  sha256_update(&c, opad, 64);
  sha256_update(&c, ih, 32);
  sha256_final(&c, out);
}

static void hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt,
                        size_t salt_len, const uint8_t *info, size_t info_len,
                        uint8_t *out, size_t L) {
  uint8_t prk[32];  // Extract
  uint8_t zerosalt[32];
  if (!salt) {
    memset(zerosalt, 0, sizeof(zerosalt));
    salt = zerosalt;
    salt_len = sizeof(zerosalt);
  }
  hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
  // Expand
  uint8_t T[32];
  size_t pos = 0;
  uint8_t ctr = 1;
  size_t nblocks = (L + 31) / 32;
  size_t tlen = 0;
  for (size_t i = 0; i < nblocks; i++) {
    // T(i) = HMAC(PRK, T(i-1) | info | counter)
    VL_Buffer b;
    vl_buf_init(&b);
    if (tlen) vl_buf_append(&b, T, tlen);
    if (info && info_len) vl_buf_append(&b, info, info_len);
    vl_buf_append(&b, &ctr, 1);
    hmac_sha256(prk, 32, b.d, b.n, T);
    vl_buf_free(&b);
    tlen = 32;
    size_t take = (pos + 32 <= L) ? 32 : (L - pos);
    memcpy(out + pos, T, take);
    pos += take;
    ctr++;
  }
}

static int pbkdf2_hmac_sha256(const uint8_t *pass, size_t passlen,
                              const uint8_t *salt, size_t saltlen,
                              uint32_t iters, uint8_t *out, size_t dklen) {
  if (iters == 0) return 0;
  uint32_t blocks = (uint32_t)((dklen + 31) / 32);
  if (blocks == 0) return 1;
  if (blocks > 0xFFFFFFFFu) return 0;  // spec limit
  uint8_t U[32], T[32];
  for (uint32_t i = 1; i <= blocks; i++) {
    // U1 = HMAC(p, salt || INT(i))
    uint8_t cnt[4] = {(uint8_t)(i >> 24), (uint8_t)(i >> 16), (uint8_t)(i >> 8),
                      (uint8_t)i};
    VL_Buffer b;
    vl_buf_init(&b);
    vl_buf_append(&b, salt, saltlen);
    vl_buf_append(&b, cnt, 4);
    hmac_sha256(pass, passlen, b.d, b.n, U);
    vl_buf_free(&b);
    memcpy(T, U, 32);
    for (uint32_t j = 2; j <= iters; j++) {
      hmac_sha256(pass, passlen, U, 32, U);
      for (int k = 0; k < 32; k++) T[k] ^= U[k];
    }
    size_t off = (size_t)(i - 1) * 32;
    size_t take = (off + 32 <= dklen) ? 32 : (dklen - off);
    memcpy(out + off, T, take);
  }
  return 1;
}

// ───────────────────────── Constant-time compare ─────────────────────────
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
  uint32_t d = 0;
  for (size_t i = 0; i < n; i++) {
    d |= (uint32_t)(a[i] ^ b[i]);
  }
  return d == 0;
}

// ───────────────────────── Random bytes ─────────────────────────
static int sys_rand_bytes(void *buf, size_t n) {
  if (vl_rand_bytes) {
    if (vl_rand_bytes(buf, n)) return 1;
  }
#if defined(_WIN32)
  // CryptGenRandom via advapi32; use BCryptGenRandom on modern Windows would be
  // better, but keep ANSI C
  HCRYPTPROV h = 0;
  if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL,
                           CRYPT_VERIFYCONTEXT)) {
    BOOL ok = CryptGenRandom(h, (DWORD)n, (BYTE *)buf);
    CryptReleaseContext(h, 0);
    return ok ? 1 : 0;
  }
  // fallback weak
  uint64_t seed = vl_mono_time_ns();
  for (size_t i = 0; i < n; i++) {
    seed ^= seed << 7;
    seed ^= seed >> 9;
    ((uint8_t *)buf)[i] = (uint8_t)seed;
  }
  return 1;
#else
  FILE *f = fopen("/dev/urandom", "rb");
  if (f) {
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    if (rd == n) return 1;
  }
  // fallback weak
  uint64_t seed = vl_mono_time_ns();
  for (size_t i = 0; i < n; i++) {
    seed ^= seed << 7;
    seed ^= seed >> 9;
    ((uint8_t *)buf)[i] = (uint8_t)seed;
  }
  return 1;
#endif
}

// ───────────────────────── Natives ─────────────────────────
static VL_Status nb_sha256(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  uint8_t d[32];
  sha256(a[0].as.s->data, a[0].as.s->len, d);
  RET_STR_BYTES(d, 32);
}

static VL_Status nb_hmac_sha256(struct VL_Context *ctx, const VL_Value *a,
                                uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 2 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  uint8_t d[32];
  hmac_sha256((const uint8_t *)a[0].as.s->data, a[0].as.s->len,
              (const uint8_t *)a[1].as.s->data, a[1].as.s->len, d);
  RET_STR_BYTES(d, 32);
}

static VL_Status nb_hkdf_sha256(struct VL_Context *ctx, const VL_Value *a,
                                uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 2 || !need_str(&a[0])) return VL_ERR_TYPE;
  const uint8_t *ikm = (const uint8_t *)a[0].as.s->data;
  size_t ikm_len = a[0].as.s->len;
  const uint8_t *salt = NULL, *info = NULL;
  size_t salt_len = 0, info_len = 0;
  int64_t L = 0;
  if (c == 2) {
    if (!vl_value_as_int(&a[1], &L) || L < 0) return VL_ERR_INVAL;
  } else if (c == 3) {
    if (!need_str(&a[1]) || !vl_value_as_int(&a[2], &L) || L < 0)
      return VL_ERR_INVAL;
    salt = (const uint8_t *)a[1].as.s->data;
    salt_len = a[1].as.s->len;
  } else {
    if ((c >= 2 && a[1].type != VT_NIL && !need_str(&a[1])) ||
        (c >= 3 && a[2].type != VT_NIL && !need_str(&a[2])) || (c < 4))
      return VL_ERR_INVAL;
    if (c >= 2 && a[1].type != VT_NIL) {
      salt = (const uint8_t *)a[1].as.s->data;
      salt_len = a[1].as.s->len;
    }
    if (c >= 3 && a[2].type != VT_NIL) {
      info = (const uint8_t *)a[2].as.s->data;
      info_len = a[2].as.s->len;
    }
    if (!vl_value_as_int(&a[3], &L) || L < 0) return VL_ERR_INVAL;
  }
  if (L > 255 * 32) return VL_ERR_INVAL;
  VL_Buffer b;
  vl_buf_init(&b);
  vl_buf_reserve(&b, (size_t)L);
  if (L) {
    b.n = L;
    hkdf_sha256(ikm, ikm_len, salt, salt_len, info, info_len, b.d, (size_t)L);
  }
  VL_Value s = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (s.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = s;
  return VL_OK;
}

static VL_Status nb_pbkdf2_sha256(struct VL_Context *ctx, const VL_Value *a,
                                  uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 4 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  int64_t it = 0, dklen = 0;
  if (!vl_value_as_int(&a[2], &it) || it <= 0) return VL_ERR_INVAL;
  if (!vl_value_as_int(&a[3], &dklen) || dklen < 0) return VL_ERR_INVAL;
  if (dklen == 0) {
    RET_STR_BYTES("", 0);
  }
  if ((uint64_t)dklen > (uint64_t)32 * 0xFFFFFFFFull) return VL_ERR_INVAL;
  VL_Buffer b;
  vl_buf_init(&b);
  vl_buf_reserve(&b, (size_t)dklen);
  b.n = dklen;
  if (!pbkdf2_hmac_sha256((const uint8_t *)a[0].as.s->data, a[0].as.s->len,
                          (const uint8_t *)a[1].as.s->data, a[1].as.s->len,
                          (uint32_t)it, (uint8_t *)b.d, (size_t)dklen)) {
    vl_buf_free(&b);
    return VL_ERR_INVAL;
  }
  VL_Value s = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (s.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = s;
  return VL_OK;
}

static VL_Status nb_rand(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *u) {
  (void)u;
  int64_t n = 0;
  if (c < 1 || !vl_value_as_int(&a[0], &n) || n < 0) return VL_ERR_INVAL;
  if (n == 0) {
    RET_STR_BYTES("", 0);
  }
  if ((uint64_t)n > (1ull << 26)) return VL_ERR_INVAL;
  VL_Buffer b;
  vl_buf_init(&b);
  vl_buf_reserve(&b, (size_t)n);
  b.n = (size_t)n;
  if (!sys_rand_bytes(b.d, b.n)) {
    vl_buf_free(&b);
    return VL_ERR_IO;
  }
  VL_Value s = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (s.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = s;
  return VL_OK;
}

static VL_Status nb_secure_equal(struct VL_Context *ctx, const VL_Value *a,
                                 uint8_t c, VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 2 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  const VL_String *A = a[0].as.s, *B = a[1].as.s;
  if (A->len != B->len) RET_BOOL(0);
  RET_BOOL(
      ct_equal((const uint8_t *)A->data, (const uint8_t *)B->data, A->len));
}

// ───────────────────────── Enregistrement ─────────────────────────
void vl_register_cryptolib(struct VL_Context *ctx) {
  if (!ctx) return;
  vl_register_native(ctx, "crypto_sha256", nb_sha256, NULL);
  vl_register_native(ctx, "crypto_hmac_sha256", nb_hmac_sha256, NULL);
  vl_register_native(ctx, "crypto_hkdf_sha256", nb_hkdf_sha256, NULL);
  vl_register_native(ctx, "crypto_pbkdf2_sha256", nb_pbkdf2_sha256, NULL);
  vl_register_native(ctx, "crypto_rand", nb_rand, NULL);
  vl_register_native(ctx, "crypto_secure_equal", nb_secure_equal, NULL);
}

// ───────────────────────── Self‑test (optionnel) ─────────────────────────
#ifdef VL_CRYPTO_TEST_MAIN
#include <assert.h>
static void hex(const uint8_t *p, size_t n) {
  static const char *H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    putchar(H[p[i] >> 4]);
    putchar(H[p[i] & 15]);
  }
  putchar('\n');
}
int main(void) {
  const char *m = "abc";
  uint8_t d[32];
  sha256(m, 3, d);
  hex(d, 32);
  uint8_t hm[32];
  hmac_sha256((const uint8_t *)"key", 3,
              (const uint8_t *)"The quick brown fox jumps over the lazy dog",
              43, hm);
  hex(hm, 32);
  uint8_t ok[42];
  hkdf_sha256((const uint8_t *)"ikm", 3, (const uint8_t *)"salt", 4,
              (const uint8_t *)"info", 4, ok, sizeof(ok));
  hex(ok, sizeof(ok));
  uint8_t dk[32];
  int r = pbkdf2_hmac_sha256((const uint8_t *)"password", 8,
                             (const uint8_t *)"salt", 4, 1, dk, 32);
  assert(r);
  hex(dk, 32);
  return 0;
}
#endif
