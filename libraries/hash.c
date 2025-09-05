// SPDX-License-Identifier: GPL-3.0-or-later
//
// hash.c — Hash/HMAC front-end for Vitte Light VM (C17, complet)
// Namespace: "hash"
//
// Build examples:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_OPENSSL -c hash.c
//   cc ... hash.o -lcrypto
//   # Optional BLAKE3 support (https://github.com/BLAKE3-team/BLAKE3)
//   cc -std=c17 -O2 -DVL_HAVE_BLAKE3 -I/path/to/blake3 -c hash.c
//   cc ... hash.o /path/to/blake3.o
//
// Model:
//   - Supports: MD5, SHA-1, SHA-224/256/384/512 via OpenSSL EVP.
//   - BLAKE3 via official blake3.h (if VL_HAVE_BLAKE3).
//   - One handle id per incremental hasher or HMAC/keyed BLAKE3.
//   - One-shot helpers: hash.hash(), hash.hmac().
//   - Helpers for sizes and hex encoding.
//
// API:
//   Introspection
//     hash.list() -> "md5 sha1 sha224 sha256 sha384 sha512 [blake3]"
//     hash.digest_size(alg) -> int | (nil,errmsg)
//     hash.block_size(alg)  -> int | (nil,errmsg)
//     hash.hex(bytes)       -> hex:string
//
//   One-shot
//     hash.hash(alg, data)              -> digest:string | (nil,errmsg)
//     hash.hmac(alg, key, data)         -> digest:string | (nil,errmsg)
//     hash.blake3_keyed(key32, data)    -> digest:string | (nil,"ENOSYS")
//
//   Incremental
//     h = hash.new(alg[, key])          -> id | (nil,errmsg)   // 'key' -> HMAC
//     or keyed BLAKE3 hash.update(h, bytes)             -> true | (nil,errmsg)
//     hash.final(h)                     -> digest:string | (nil,errmsg)
//     hash.reset(h)                     -> true | (nil,errmsg) // same
//     algorithm, same key hash.free(h)                      -> true
//
// Notes:
//   - Inputs must be NUL-free VM strings (binary limitation). Outputs use
//   vl_push_lstring and are binary-safe.
//   - Errors: "EINVAL", "ENOSYS", "ENOMEM".
//
// Deps: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#if defined(VL_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

#if defined(VL_HAVE_BLAKE3)
#include "blake3.h"
#endif

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *hs_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t hs_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static const char *hs_opt_str(VL_State *S, int idx, const char *defv) {
  if (!vl_get(S, idx) || !vl_isstring(S, idx)) return defv;
  return hs_check_str(S, idx);
}
static void push_err(VL_State *S, const char *e) {
  vl_push_nil(S);
  vl_push_string(S, e ? e : "EIO");
}

// ---------------------------------------------------------------------
// Algorithm registry
// ---------------------------------------------------------------------
typedef enum {
  ALG_NONE = 0,
  ALG_MD5,
  ALG_SHA1,
  ALG_SHA224,
  ALG_SHA256,
  ALG_SHA384,
  ALG_SHA512,
  ALG_BLAKE3
} AlgId;

typedef struct AlgInfo {
  const char *name;
  AlgId id;
  int dig;    // digest size
  int block;  // block size (for HMAC)
} AlgInfo;

static const AlgInfo *alg_find(const char *name) {
  if (!name) return NULL;
  if (strcmp(name, "md5") == 0)
    static const AlgInfo i = {"md5", ALG_MD5, 16, 64};
  else if (strcmp(name, "sha1") == 0)
    static const AlgInfo i = {"sha1", ALG_SHA1, 20, 64};
  else if (strcmp(name, "sha224") == 0)
    static const AlgInfo i = {"sha224", ALG_SHA224, 28, 64};
  else if (strcmp(name, "sha256") == 0)
    static const AlgInfo i = {"sha256", ALG_SHA256, 32, 64};
  else if (strcmp(name, "sha384") == 0)
    static const AlgInfo i = {"sha384", ALG_SHA384, 48, 128};
  else if (strcmp(name, "sha512") == 0)
    static const AlgInfo i = {"sha512", ALG_SHA512, 64, 128};
  else if (strcmp(name, "blake3") == 0)
    static const AlgInfo i = {"blake3", ALG_BLAKE3,
#if defined(VL_HAVE_BLAKE3)
                              BLAKE3_OUT_LEN
#else
                              32
#endif
                              ,
                              64};
  else
    return NULL;
  return &i;  // NB: returns pointer to a temporary static in each branch — OK
              // for immediate use
}

// ---------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------
typedef struct HCtx {
  int used;
  AlgId alg;
  int keyed;

#if defined(VL_HAVE_OPENSSL)
  const EVP_MD *md;
  EVP_MD_CTX *mdctx;
  HMAC_CTX *hctx;  // for HMAC
#endif

#if defined(VL_HAVE_BLAKE3)
  blake3_hasher b3;
  uint8_t key_b3[32];
  int keylen_b3;
#endif
} HCtx;

static HCtx *g_h = NULL;
static int g_cap = 0;

static int ensure_cap(int need) {
  if (need <= g_cap) return 1;
  int n = g_cap ? g_cap : 8;
  while (n < need) n <<= 1;
  HCtx *nh = (HCtx *)realloc(g_h, (size_t)n * sizeof *nh);
  if (!nh) return 0;
  for (int i = g_cap; i < n; i++) {
    memset(&nh[i], 0, sizeof nh[i]);
  }
  g_h = nh;
  g_cap = n;
  return 1;
}
static int alloc_h(void) {
  for (int i = 1; i < g_cap; i++)
    if (!g_h[i].used) return i;
  if (!ensure_cap(g_cap ? g_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_cap; i++)
    if (!g_h[i].used) return i;
  return 0;
}
static int chk_h(int id) { return id > 0 && id < g_cap && g_h[id].used; }

static void free_ctx(HCtx *h) {
  if (!h || !h->used) return;
#if defined(VL_HAVE_OPENSSL)
  if (h->mdctx) {
    EVP_MD_CTX_free(h->mdctx);
    h->mdctx = NULL;
  }
  if (h->hctx) {
    HMAC_CTX_free(h->hctx);
    h->hctx = NULL;
  }
#endif
  memset(h, 0, sizeof *h);
}

// ---------------------------------------------------------------------
// OpenSSL helpers
// ---------------------------------------------------------------------
#if defined(VL_HAVE_OPENSSL)
static const EVP_MD *evp_for_alg(AlgId a) {
  switch (a) {
    case ALG_MD5:
      return EVP_get_digestbyname("md5");
    case ALG_SHA1:
      return EVP_get_digestbyname("sha1");
    case ALG_SHA224:
      return EVP_get_digestbyname("sha224");
    case ALG_SHA256:
      return EVP_get_digestbyname("sha256");
    case ALG_SHA384:
      return EVP_get_digestbyname("sha384");
    case ALG_SHA512:
      return EVP_get_digestbyname("sha512");
    default:
      return NULL;
  }
}
#endif

// ---------------------------------------------------------------------
// Common ops
// ---------------------------------------------------------------------
static int ctx_init_alg(HCtx *h, const AlgInfo *ai, const char *key,
                        size_t klen, char **err) {
  *err = NULL;
  h->alg = ai->id;
  h->keyed = key ? 1 : 0;

  if (ai->id == ALG_BLAKE3) {
#if defined(VL_HAVE_BLAKE3)
    if (key) {
      if (klen != 32) {
        *err = "EINVAL";
        return 0;
      }  // keyed mode requires 32 bytes
      memcpy(h->key_b3, key, 32);
      h->keylen_b3 = 32;
      blake3_hasher_init_keyed(&h->b3, h->key_b3);
    } else {
      blake3_hasher_init(&h->b3);
    }
    return 1;
#else
    *err = "ENOSYS";
    return 0;
#endif
  }

#if defined(VL_HAVE_OPENSSL)
  h->md = evp_for_alg(ai->id);
  if (!h->md) {
    *err = "ENOSYS";
    return 0;
  }

  if (key) {
    h->hctx = HMAC_CTX_new();
    if (!h->hctx) {
      *err = "ENOMEM";
      return 0;
    }
    if (HMAC_Init_ex(h->hctx, key, (int)klen, h->md, NULL) != 1) {
      *err = "EIO";
      return 0;
    }
    return 1;
  } else {
    h->mdctx = EVP_MD_CTX_new();
    if (!h->mdctx) {
      *err = "ENOMEM";
      return 0;
    }
    if (EVP_DigestInit_ex(h->mdctx, h->md, NULL) != 1) {
      *err = "EIO";
      return 0;
    }
    return 1;
  }
#else
  (void)key;
  (void)klen;
  *err = "ENOSYS";
  return 0;
#endif
}

static int ctx_update(HCtx *h, const char *data, size_t n, char **err) {
  *err = NULL;
  if (!h || !h->used) {
    *err = "EINVAL";
    return 0;
  }
  if (h->alg == ALG_BLAKE3) {
#if defined(VL_HAVE_BLAKE3)
    blake3_hasher_update(&h->b3, data, n);
    return 1;
#else
    *err = "ENOSYS";
    return 0;
#endif
  }
#if defined(VL_HAVE_OPENSSL)
  if (h->hctx) {
    if (HMAC_Update(h->hctx, (const unsigned char *)data, n) != 1) {
      *err = "EIO";
      return 0;
    }
    return 1;
  }
  if (h->mdctx) {
    if (EVP_DigestUpdate(h->mdctx, data, n) != 1) {
      *err = "EIO";
      return 0;
    }
    return 1;
  }
  *err = "EINVAL";
  return 0;
#else
  (void)data;
  (void)n;
  *err = "ENOSYS";
  return 0;
#endif
}

static int ctx_final(HCtx *h, uint8_t *out, size_t outcap, size_t *outlen,
                     char **err) {
  *err = NULL;
  *outlen = 0;
  if (!h || !h->used) {
    *err = "EINVAL";
    return 0;
  }

  if (h->alg == ALG_BLAKE3) {
#if defined(VL_HAVE_BLAKE3)
    if (outcap < BLAKE3_OUT_LEN) {
      *err = "EINVAL";
      return 0;
    }
    blake3_hasher_finalize(&h->b3, out, BLAKE3_OUT_LEN);
    *outlen = BLAKE3_OUT_LEN;
    return 1;
#else
    *err = "ENOSYS";
    return 0;
#endif
  }

#if defined(VL_HAVE_OPENSSL)
  unsigned int len = 0;
  if (h->hctx) {
    if (HMAC_Final(h->hctx, out, &len) != 1) {
      *err = "EIO";
      return 0;
    }
    *outlen = len;
    return 1;
  }
  if (h->mdctx) {
    if (EVP_DigestFinal_ex(h->mdctx, out, &len) != 1) {
      *err = "EIO";
      return 0;
    }
    *outlen = len;
    return 1;
  }
  *err = "EINVAL";
  return 0;
#else
  (void)out;
  (void)outcap;
  *err = "ENOSYS";
  return 0;
#endif
}

static int ctx_reset(HCtx *h, char **err) {
  *err = NULL;
  if (!h || !h->used) {
    *err = "EINVAL";
    return 0;
  }

  if (h->alg == ALG_BLAKE3) {
#if defined(VL_HAVE_BLAKE3)
    if (h->keyed)
      blake3_hasher_init_keyed(&h->b3, h->key_b3);
    else
      blake3_hasher_init(&h->b3);
    return 1;
#else
    *err = "ENOSYS";
    return 0;
#endif
  }

#if defined(VL_HAVE_OPENSSL)
  if (h->hctx) {
    // Re-init HMAC with same key/md
    if (HMAC_Init_ex(h->hctx, NULL, 0, NULL, NULL) != 1) {
      *err = "EIO";
      return 0;
    }
    return 1;
  }
  if (h->mdctx) {
    if (EVP_DigestInit_ex(h->mdctx, h->md, NULL) != 1) {
      *err = "EIO";
      return 0;
    }
    return 1;
  }
  *err = "EINVAL";
  return 0;
#else
  *err = "ENOSYS";
  return 0;
#endif
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
static void to_hex(const uint8_t *b, size_t n, char *dst) {  // dst size >= 2n+1
  static const char hexd[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    dst[2 * i] = hexd[b[i] >> 4];
    dst[2 * i + 1] = hexd[b[i] & 0xF];
  }
  dst[2 * n] = 0;
}

static int alg_sizes(const AlgInfo *ai, int *dig, int *blk) {
  if (!ai) return 0;
#if defined(VL_HAVE_OPENSSL)
  if (ai->id != ALG_BLAKE3) {
    const EVP_MD *md = evp_for_alg(ai->id);
    if (!md) return 0;
    if (dig) *dig = EVP_MD_size(md);
    if (blk) *blk = EVP_MD_block_size(md);
    return 1;
  }
#endif
#if defined(VL_HAVE_BLAKE3)
  if (ai->id == ALG_BLAKE3) {
    if (dig) *dig = BLAKE3_OUT_LEN;
    if (blk) *blk = 64;
    return 1;
  }
#endif
  // Fallback to static table
  if (dig) *dig = ai->dig;
  if (blk) *blk = ai->block;
  return 1;
}

// ---------------------------------------------------------------------
// VM: list / sizes / hex
// ---------------------------------------------------------------------
static int vlh_list(VL_State *S) {
  const char *extra =
#if defined(VL_HAVE_BLAKE3)
      " blake3"
#else
      ""
#endif
      ;
#if defined(VL_HAVE_OPENSSL)
  vl_push_string(S, (char *)(("md5 sha1 sha224 sha256 sha384 sha512" extra)));
#else
  // only blake3 if present
  if (extra[0])
    vl_push_string(S, (char *)(("blake3")));
  else
    vl_push_string(S, "");
#endif
  return 1;
}
static int vlh_digest_size(VL_State *S) {
  const char *alg = hs_check_str(S, 1);
  const AlgInfo *ai = alg_find(alg);
  int dig = 0;
  if (!ai || !alg_sizes(ai, &dig, NULL)) {
    push_err(S, "ENOSYS");
    return 2;
  }
  vl_push_int(S, (int64_t)dig);
  return 1;
}
static int vlh_block_size(VL_State *S) {
  const char *alg = hs_check_str(S, 1);
  const AlgInfo *ai = alg_find(alg);
  int blk = 0;
  if (!ai || !alg_sizes(ai, NULL, &blk)) {
    push_err(S, "ENOSYS");
    return 2;
  }
  vl_push_int(S, (int64_t)blk);
  return 1;
}
static int vlh_hex(VL_State *S) {
  const char *bytes = hs_check_str(S, 1);
  size_t n = strlen(bytes);
  char *hx = (char *)malloc(n * 2 + 1);
  if (!hx) {
    push_err(S, "ENOMEM");
    return 2;
  }
  to_hex((const uint8_t *)bytes, n, hx);
  vl_push_string(S, hx);
  free(hx);
  return 1;
}

// ---------------------------------------------------------------------
// VM: incremental
// ---------------------------------------------------------------------
static int vlh_new(VL_State *S) {
  const char *alg = hs_check_str(S, 1);
  const char *key = vl_get(S, 2) ? hs_check_str(S, 2) : NULL;
  size_t klen = key ? strlen(key) : 0;

  const AlgInfo *ai = alg_find(alg);
  if (!ai) {
    push_err(S, "EINVAL");
    return 2;
  }

  int id = alloc_h();
  if (!id) {
    push_err(S, "ENOMEM");
    return 2;
  }
  g_h[id].used = 1;

  char *err = NULL;
  if (!ctx_init_alg(&g_h[id], ai, key, klen, &err)) {
    free_ctx(&g_h[id]);
    push_err(S, err);
    return 2;
  }

  vl_push_int(S, (int64_t)id);
  return 1;
}
static int vlh_update(VL_State *S) {
  int id = (int)hs_check_int(S, 1);
  const char *bytes = hs_check_str(S, 2);
  if (!chk_h(id)) {
    push_err(S, "EINVAL");
    return 2;
  }
  char *err = NULL;
  if (!ctx_update(&g_h[id], bytes, strlen(bytes), &err)) {
    push_err(S, err);
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}
static int vlh_final(VL_State *S) {
  int id = (int)hs_check_int(S, 1);
  if (!chk_h(id)) {
    push_err(S, "EINVAL");
    return 2;
  }
  uint8_t out[64];
  size_t outlen = 0;
  char *err = NULL;
  if (!ctx_final(&g_h[id], out, sizeof out, &outlen, &err)) {
    push_err(S, err);
    return 2;
  }
  vl_push_lstring(S, (const char *)out, (int)outlen);
  return 1;
}
static int vlh_reset(VL_State *S) {
  int id = (int)hs_check_int(S, 1);
  if (!chk_h(id)) {
    push_err(S, "EINVAL");
    return 2;
  }
  char *err = NULL;
  if (!ctx_reset(&g_h[id], &err)) {
    push_err(S, err);
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}
static int vlh_free(VL_State *S) {
  int id = (int)hs_check_int(S, 1);
  if (id > 0 && id < g_cap && g_h[id].used) {
    free_ctx(&g_h[id]);
    g_h[id].used = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ---------------------------------------------------------------------
// VM: one-shot
// ---------------------------------------------------------------------
static int vlh_hash(VL_State *S) {
  const char *alg = hs_check_str(S, 1);
  const char *data = hs_check_str(S, 2);
  const AlgInfo *ai = alg_find(alg);
  if (!ai) {
    push_err(S, "EINVAL");
    return 2;
  }

  // Fast path without allocating a handle
  uint8_t out[64];
  size_t outlen = 0;
  char *err = NULL;
  HCtx tmp;
  memset(&tmp, 0, sizeof tmp);
  tmp.used = 1;
  if (!ctx_init_alg(&tmp, ai, NULL, 0, &err)) {
    push_err(S, err);
    return 2;
  }
  if (!ctx_update(&tmp, data, strlen(data), &err)) {
    free_ctx(&tmp);
    push_err(S, err);
    return 2;
  }
  if (!ctx_final(&tmp, out, sizeof out, &outlen, &err)) {
    free_ctx(&tmp);
    push_err(S, err);
    return 2;
  }
  free_ctx(&tmp);

  vl_push_lstring(S, (const char *)out, (int)outlen);
  return 1;
}

static int vlh_hmac(VL_State *S) {
  const char *alg = hs_check_str(S, 1);
  const char *key = hs_check_str(S, 2);
  const char *data = hs_check_str(S, 3);
  const AlgInfo *ai = alg_find(alg);
  if (!ai) {
    push_err(S, "EINVAL");
    return 2;
  }

  // For blake3, map to keyed BLAKE3
  if (ai->id == ALG_BLAKE3) {
#if defined(VL_HAVE_BLAKE3)
    if (strlen(key) != 32) {
      push_err(S, "EINVAL");
      return 2;
    }
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher h;
    blake3_hasher_init_keyed(&h, (const uint8_t *)key);
    blake3_hasher_update(&h, data, strlen(data));
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    vl_push_lstring(S, (const char *)out, BLAKE3_OUT_LEN);
    return 1;
#else
    push_err(S, "ENOSYS");
    return 2;
#endif
  }

#if defined(VL_HAVE_OPENSSL)
  unsigned int len = 0;
  uint8_t out[EVP_MAX_MD_SIZE];
  const EVP_MD *md = (ai->id == ALG_MD5)      ? EVP_get_digestbyname("md5")
                     : (ai->id == ALG_SHA1)   ? EVP_get_digestbyname("sha1")
                     : (ai->id == ALG_SHA224) ? EVP_get_digestbyname("sha224")
                     : (ai->id == ALG_SHA256) ? EVP_get_digestbyname("sha256")
                     : (ai->id == ALG_SHA384) ? EVP_get_digestbyname("sha384")
                     : (ai->id == ALG_SHA512) ? EVP_get_digestbyname("sha512")
                                              : NULL;
  if (!md) {
    push_err(S, "ENOSYS");
    return 2;
  }
  if (!HMAC(md, key, (int)strlen(key), (const unsigned char *)data,
            strlen(data), out, &len)) {
    push_err(S, "EIO");
    return 2;
  }
  vl_push_lstring(S, (const char *)out, (int)len);
  return 1;
#else
  push_err(S, "ENOSYS");
  return 2;
#endif
}

static int vlh_b3_keyed(VL_State *S) {
  const char *key = hs_check_str(S, 1);
  const char *data = hs_check_str(S, 2);
#if defined(VL_HAVE_BLAKE3)
  if (strlen(key) != 32) {
    push_err(S, "EINVAL");
    return 2;
  }
  uint8_t out[BLAKE3_OUT_LEN];
  blake3_hasher h;
  blake3_hasher_init_keyed(&h, (const uint8_t *)key);
  blake3_hasher_update(&h, data, strlen(data));
  blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
  vl_push_lstring(S, (const char *)out, BLAKE3_OUT_LEN);
  return 1;
#else
  push_err(S, "ENOSYS");
  return 2;
#endif
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg hashlib[] = {{"list", vlh_list},
                                 {"digest_size", vlh_digest_size},
                                 {"block_size", vlh_block_size},
                                 {"hex", vlh_hex},

                                 {"new", vlh_new},
                                 {"update", vlh_update},
                                 {"final", vlh_final},
                                 {"reset", vlh_reset},
                                 {"free", vlh_free},

                                 {"hash", vlh_hash},
                                 {"hmac", vlh_hmac},
                                 {"blake3_keyed", vlh_b3_keyed},

                                 {NULL, NULL}};

void vl_open_hashlib(VL_State *S) { vl_register_lib(S, "hash", hashlib); }
