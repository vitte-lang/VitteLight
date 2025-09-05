/* ============================================================================
   includes/auxlib.h — utilitaires C17 transverses et portables
   Modules: string_view, buffer dynamique, arène mémoire, hash, random,
            base64/hex, endian, temps, fichiers/chemins, env, UTF-8.
   Design:  zéro dépendance hors libc. Thread-safety côté appelant.
   Config:
     - Definir AUX_API pour l’export (ex: __declspec(dllexport))
     - Definir AUX_HEADER_ONLY pour une implémentation header-only (inline)
       Sinon, fournir auxlib.c avec les mêmes implémentations.
   Licence: MIT.
   ============================================================================
 */
#ifndef AUXLIB_H
#define AUXLIB_H
#pragma once

/* ------------------------------------------------------------------------- */
/* Exports + C++                                                             */
/* ------------------------------------------------------------------------- */
#ifndef AUX_API
#define AUX_API extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Includes de base                                                          */
/* ------------------------------------------------------------------------- */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------- */
/* Attributs/inline                                                          */
/* ------------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define AUX_INLINE static __inline__ __attribute__((always_inline))
#define AUX_PURE __attribute__((pure))
#define AUX_CONST __attribute__((const))
#define AUX_LIKELY(x) __builtin_expect(!!(x), 1)
#define AUX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define AUX_INLINE static inline
#define AUX_PURE
#define AUX_CONST
#define AUX_LIKELY(x) (x)
#define AUX_UNLIKELY(x) (x)
#endif

/* ------------------------------------------------------------------------- */
/* Plateforme                                                                */
/* ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
#define AUX_OS_WINDOWS 1
#include <direct.h>
#include <io.h>
#include <windows.h>
#define AUX_PATH_SEP '\\'
#define AUX_PATH_SEP_STR "\\"
#else
#define AUX_OS_POSIX 1
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define AUX_PATH_SEP '/'
#define AUX_PATH_SEP_STR "/"
#endif

/* ------------------------------------------------------------------------- */
/* String view                                                               */
/* ------------------------------------------------------------------------- */
typedef struct {
  const char* p;
  size_t n;
} aux_sv;

AUX_API aux_sv aux_sv_from_cstr(const char* s);
AUX_API aux_sv aux_sv_make(const char* s, size_t n);
AUX_API int aux_sv_eq(aux_sv a, aux_sv b);   /* 1/0 */
AUX_API int aux_sv_ieq(aux_sv a, aux_sv b);  /* ASCII case-insensitive */
AUX_API int aux_sv_cmp(aux_sv a, aux_sv b);  /* strcmp-like */
AUX_API int aux_sv_icmp(aux_sv a, aux_sv b); /* ASCII insensitive */
AUX_API int aux_sv_starts(aux_sv s, aux_sv pref);
AUX_API int aux_sv_ends(aux_sv s, aux_sv suf);
AUX_API size_t aux_sv_find(aux_sv s, aux_sv needle); /* pos or npos */
AUX_API size_t aux_sv_rfind(aux_sv s, aux_sv needle);
AUX_API aux_sv aux_sv_trim(aux_sv s);
AUX_API aux_sv aux_sv_ltrim(aux_sv s);
AUX_API aux_sv aux_sv_rtrim(aux_sv s);
AUX_API aux_sv aux_sv_take_prefix(aux_sv* s, size_t n); /* advance input */
AUX_API aux_sv aux_sv_split_once(aux_sv s, char sep, aux_sv* left,
                                 aux_sv* right);

/* ------------------------------------------------------------------------- */
/* Buffer dynamique (octets)                                                 */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint8_t* data;
  size_t len;
  size_t cap;
} aux_buf;

AUX_API void aux_buf_init(aux_buf* b);
AUX_API void aux_buf_free(aux_buf* b);
AUX_API int aux_buf_reserve(aux_buf* b, size_t need); /* 0 ok */
AUX_API int aux_buf_resize(aux_buf* b, size_t n);     /* grow with zero-fill */
AUX_API int aux_buf_clear(aux_buf* b);                /* len=0 */
AUX_API int aux_buf_push(aux_buf* b, const void* src, size_t n);
AUX_API int aux_buf_push_cstr(aux_buf* b, const char* s);
AUX_API int aux_buf_push_u8(aux_buf* b, uint8_t v);

/* ------------------------------------------------------------------------- */
/* Arène mémoire                                                             */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint8_t* base;
  size_t cap;
  size_t off;
} aux_arena;

AUX_API int aux_arena_init(aux_arena* a, size_t cap);
AUX_API void aux_arena_free(aux_arena* a);
AUX_API void aux_arena_reset(aux_arena* a);
AUX_API void* aux_arena_alloc(aux_arena* a, size_t n,
                              size_t align); /* NULL if OOM */

/* ------------------------------------------------------------------------- */
/* Hash (FNV-1a)                                                             */
/* ------------------------------------------------------------------------- */
AUX_API uint32_t aux_hash32(const void* data, size_t n);
AUX_API uint64_t aux_hash64(const void* data, size_t n);
AUX_API uint64_t aux_hash_sv(aux_sv s);

/* ------------------------------------------------------------------------- */
/* RNG (xoshiro256**)                                                        */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint64_t s[4];
} aux_rng;

AUX_API void aux_rng_seed(aux_rng* r, uint64_t seed);
AUX_API uint64_t aux_rng_next_u64(aux_rng* r);
AUX_API uint32_t aux_rng_next_u32(aux_rng* r);
AUX_API double aux_rng_next_f64(aux_rng* r); /* [0,1) */
AUX_API uint64_t aux_rng_range_u64(aux_rng* r, uint64_t lo,
                                   uint64_t hi); /* inclusive lo..hi */

/* ------------------------------------------------------------------------- */
/* Base64 / Hex                                                              */
/* ------------------------------------------------------------------------- */
AUX_API int aux_base64_encode(const void* src, size_t n,
                              aux_buf* out);            /* 0 ok */
AUX_API int aux_base64_decode(aux_sv in, aux_buf* out); /* 0 ok */
AUX_API int aux_hex_encode(const void* src, size_t n, aux_buf* out, int upper);
AUX_API int aux_hex_decode(aux_sv in, aux_buf* out);

/* ------------------------------------------------------------------------- */
/* Endian helpers                                                            */
/* ------------------------------------------------------------------------- */
AUX_INLINE uint16_t aux_bswap16(uint16_t x) {
  return (uint16_t)((x >> 8) | (x << 8));
}
AUX_INLINE uint32_t aux_bswap32(uint32_t x) {
  return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) |
         (x << 24);
}
AUX_INLINE uint64_t aux_bswap64(uint64_t x) {
  return ((uint64_t)aux_bswap32((uint32_t)x) << 32) |
         aux_bswap32((uint32_t)(x >> 32));
}
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define AUX_LE16(x) aux_bswap16((uint16_t)(x))
#define AUX_LE32(x) aux_bswap32((uint32_t)(x))
#define AUX_LE64(x) aux_bswap64((uint64_t)(x))
#else
#define AUX_LE16(x) (x)
#define AUX_LE32(x) (x)
#define AUX_LE64(x) (x)
#endif

/* ------------------------------------------------------------------------- */
/* Temps                                                                     */
/* ------------------------------------------------------------------------- */
AUX_API uint64_t aux_time_unix_ns(void); /* epoch nanoseconds */
AUX_API uint64_t aux_time_mono_ns(void); /* monotonic ns         */
AUX_API double aux_time_mono_sec(void);  /* monotonic seconds    */
AUX_API int aux_time_iso8601(char* dst, size_t dstsz, time_t t,
                             int utc); /* 0 ok */

/* ------------------------------------------------------------------------- */
/* Fichiers / Chemins / Env                                                  */
/* ------------------------------------------------------------------------- */
AUX_API int aux_file_exists(const char* path);                 /* 1/0 */
AUX_API int aux_file_size(const char* path, int64_t* sz);      /* 0 ok */
AUX_API int aux_file_read_all(const char* path, aux_buf* out); /* 0 ok */
AUX_API int aux_file_write_all(const char* path, const void* data,
                               size_t n);            /* 0 ok */
AUX_API int aux_mkdirs(const char* path);            /* 0 ok, idempotent */
AUX_API const char* aux_getenv_str(const char* key); /* NULL if unset */

AUX_API int aux_path_is_abs(const char* p);
AUX_API int aux_path_join2(const char* a, const char* b,
                           aux_buf* out); /* uses sep */
AUX_API void aux_path_normalize(
    char* p); /* in-place: collapses // and \ → sep */

/* ------------------------------------------------------------------------- */
/* UTF-8 helpers                                                              */
/* ------------------------------------------------------------------------- */
AUX_API int aux_utf8_decode(const char* s, size_t n,
                            uint32_t* cp); /* returns bytes used or -1 */
AUX_API int aux_utf8_encode(uint32_t cp, char out[4]); /* returns bytes or -1 */
AUX_API int aux_utf8_valid(const char* s, size_t n);   /* 1/0 */

/* ------------------------------------------------------------------------- */
/* Implémentation header-only                                                */
/* ------------------------------------------------------------------------- */
#ifdef AUX_HEADER_ONLY

/* ---------- utils locaux ---------- */
AUX_INLINE size_t aux__minsz(size_t a, size_t b) { return a < b ? a : b; }
AUX_INLINE size_t aux__maxsz(size_t a, size_t b) { return a > b ? a : b; }
AUX_INLINE int aux__is_space(unsigned c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}
AUX_INLINE int aux__tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

/* ---------- string view ---------- */
AUX_INLINE aux_sv aux_sv_from_cstr(const char* s) {
  aux_sv v = {s, s ? strlen(s) : 0};
  return v;
}
AUX_INLINE aux_sv aux_sv_make(const char* s, size_t n) {
  aux_sv v = {s, n};
  return v;
}
AUX_INLINE int aux_sv_eq(aux_sv a, aux_sv b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}
AUX_INLINE int aux_sv_cmp(aux_sv a, aux_sv b) {
  const size_t n = aux__minsz(a.n, b.n);
  int r = (n ? memcmp(a.p, b.p, n) : 0);
  return r ? r : (a.n < b.n ? -1 : a.n > b.n ? 1 : 0);
}
AUX_INLINE int aux_sv_ieq(aux_sv a, aux_sv b) {
  if (a.n != b.n) return 0;
  for (size_t i = 0; i < a.n; i++)
    if (aux__tolower((unsigned)a.p[i]) != aux__tolower((unsigned)b.p[i]))
      return 0;
  return 1;
}
AUX_INLINE int aux_sv_icmp(aux_sv a, aux_sv b) {
  const size_t n = aux__minsz(a.n, b.n);
  for (size_t i = 0; i < n; i++) {
    int ca = aux__tolower((unsigned)a.p[i]),
        cb = aux__tolower((unsigned)b.p[i]);
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}
AUX_INLINE int aux_sv_starts(aux_sv s, aux_sv pref) {
  return s.n >= pref.n && memcmp(s.p, pref.p, pref.n) == 0;
}
AUX_INLINE int aux_sv_ends(aux_sv s, aux_sv suf) {
  return s.n >= suf.n && memcmp(s.p + s.n - suf.n, suf.p, suf.n) == 0;
}
AUX_INLINE aux_sv aux_sv_ltrim(aux_sv s) {
  size_t i = 0;
  while (i < s.n && aux__is_space((unsigned)s.p[i])) i++;
  s.p += i;
  s.n -= i;
  return s;
}
AUX_INLINE aux_sv aux_sv_rtrim(aux_sv s) {
  size_t n = s.n;
  while (n > 0 && aux__is_space((unsigned)s.p[n - 1])) n--;
  s.n = n;
  return s;
}
AUX_INLINE aux_sv aux_sv_trim(aux_sv s) {
  return aux_sv_rtrim(aux_sv_ltrim(s));
}
AUX_INLINE aux_sv aux_sv_take_prefix(aux_sv* s, size_t n) {
  n = aux__minsz(n, s->n);
  aux_sv out = {s->p, n};
  s->p += n;
  s->n -= n;
  return out;
}
AUX_INLINE aux_sv aux_sv_split_once(aux_sv s, char sep, aux_sv* left,
                                    aux_sv* right) {
  size_t i = 0;
  for (; i < s.n; i++)
    if (s.p[i] == sep) break;
  if (left) *left = (aux_sv){s.p, i};
  if (right)
    *right =
        (i < s.n) ? (aux_sv){s.p + i + 1, s.n - i - 1} : (aux_sv){s.p + s.n, 0};
  return (aux_sv){s.p + i, (i < s.n) ? 1 : 0};
}
AUX_INLINE size_t aux_sv_find(aux_sv s, aux_sv needle) {
  if (needle.n == 0) return 0;
  if (needle.n > s.n) return (size_t)-1;
  for (size_t i = 0; i + needle.n <= s.n; i++)
    if (memcmp(s.p + i, needle.p, needle.n) == 0) return i;
  return (size_t)-1;
}
AUX_INLINE size_t aux_sv_rfind(aux_sv s, aux_sv needle) {
  if (needle.n == 0) return s.n;
  if (needle.n > s.n) return (size_t)-1;
  for (size_t i = s.n - needle.n + 1; i-- > 0;) {
    if (memcmp(s.p + i, needle.p, needle.n) == 0) return i;
  }
  return (size_t)-1;
}

/* ---------- buffer ---------- */
AUX_INLINE void aux_buf_init(aux_buf* b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}
AUX_INLINE void aux_buf_free(aux_buf* b) {
  if (b->data) free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}
AUX_INLINE int aux_buf_reserve(aux_buf* b, size_t need) {
  if (need <= b->cap) return 0;
  size_t cap = aux__maxsz(16, b->cap ? b->cap * 2 : 256);
  while (cap < need) cap = cap < (SIZE_MAX / 2) ? cap * 2 : need;
  void* p = realloc(b->data, cap);
  if (!p) return -1;
  b->data = (uint8_t*)p;
  b->cap = cap;
  return 0;
}
AUX_INLINE int aux_buf_resize(aux_buf* b, size_t n) {
  if (aux_buf_reserve(b, n) != 0) return -1;
  if (n > b->len) memset(b->data + b->len, 0, n - b->len);
  b->len = n;
  return 0;
}
AUX_INLINE int aux_buf_clear(aux_buf* b) {
  b->len = 0;
  return 0;
}
AUX_INLINE int aux_buf_push(aux_buf* b, const void* src, size_t n) {
  if (aux_buf_reserve(b, b->len + n) != 0) return -1;
  memcpy(b->data + b->len, src, n);
  b->len += n;
  return 0;
}
AUX_INLINE int aux_buf_push_cstr(aux_buf* b, const char* s) {
  return aux_buf_push(b, s, s ? strlen(s) : 0);
}
AUX_INLINE int aux_buf_push_u8(aux_buf* b, uint8_t v) {
  if (aux_buf_reserve(b, b->len + 1) != 0) return -1;
  b->data[b->len++] = v;
  return 0;
}

/* ---------- arène ---------- */
AUX_INLINE int aux_arena_init(aux_arena* a, size_t cap) {
  a->base = (uint8_t*)malloc(cap ? cap : 1);
  if (!a->base) return -1;
  a->cap = cap;
  a->off = 0;
  return 0;
}
AUX_INLINE void aux_arena_free(aux_arena* a) {
  if (a->base) free(a->base);
  a->base = NULL;
  a->cap = a->off = 0;
}
AUX_INLINE void aux_arena_reset(aux_arena* a) { a->off = 0; }
AUX_INLINE void* aux_arena_alloc(aux_arena* a, size_t n, size_t align) {
  if (align == 0) align = 1;
  size_t cur = (size_t)(a->base + a->off);
  size_t base = (size_t)a->base;
  size_t off = ((cur + (align - 1)) & ~(align - 1)) - base;
  if (n > a->cap - off) return NULL;
  void* p = a->base + off;
  a->off = off + n;
  return p;
}

/* ---------- hash ---------- */
AUX_INLINE uint32_t aux_hash32(const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}
AUX_INLINE uint64_t aux_hash64(const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}
AUX_INLINE uint64_t aux_hash_sv(aux_sv s) { return aux_hash64(s.p, s.n); }

/* ---------- rng (xoshiro256**) ---------- */
AUX_INLINE uint64_t aux__rotl(uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}
AUX_INLINE void aux_rng_seed(aux_rng* r, uint64_t seed) {
  /* SplitMix64 pour remplir l’état */
  uint64_t x = seed ? seed : 0x9E3779B97F4A7C15ull ^ (uint64_t)time(NULL);
  for (int i = 0; i < 4; i++) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    r->s[i] = z ^ (z >> 31);
  }
}
AUX_INLINE uint64_t aux_rng_next_u64(aux_rng* r) {
  const uint64_t result = aux__rotl(r->s[1] * 5ull, 7) * 9ull;
  const uint64_t t = r->s[1] << 17;
  r->s[2] ^= r->s[0];
  r->s[3] ^= r->s[1];
  r->s[1] ^= r->s[2];
  r->s[0] ^= r->s[3];
  r->s[2] ^= t;
  r->s[3] = aux__rotl(r->s[3], 45);
  return result;
}
AUX_INLINE uint32_t aux_rng_next_u32(aux_rng* r) {
  return (uint32_t)aux_rng_next_u64(r);
}
AUX_INLINE double aux_rng_next_f64(aux_rng* r) {
  /* 53 bits → [0,1) */
  return (aux_rng_next_u64(r) >> 11) * (1.0 / 9007199254740992.0);
}
AUX_INLINE uint64_t aux_rng_range_u64(aux_rng* r, uint64_t lo, uint64_t hi) {
  if (hi < lo) {
    uint64_t t = lo;
    lo = hi;
    hi = t;
  }
  uint64_t span = hi - lo + 1;
  uint64_t x, lim = ((uint64_t)-1) - (((uint64_t)-1) % span);
  do {
    x = aux_rng_next_u64(r);
  } while (x > lim);
  return lo + (x % span);
}

/* ---------- base64 ---------- */
AUX_INLINE int aux_base64_encode(const void* src, size_t n, aux_buf* out) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t* p = (const uint8_t*)src;
  size_t olen = ((n + 2) / 3) * 4;
  if (aux_buf_reserve(out, out->len + olen) != 0) return -1;
  size_t i = 0;
  while (i + 3 <= n) {
    uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
    out->data[out->len++] = T[(v >> 18) & 63];
    out->data[out->len++] = T[(v >> 12) & 63];
    out->data[out->len++] = T[(v >> 6) & 63];
    out->data[out->len++] = T[v & 63];
    i += 3;
  }
  if (i < n) {
    uint32_t v = p[i] << 16;
    int pad = 0;
    if (i + 1 < n) {
      v |= p[i + 1] << 8;
    } else
      pad = 2;
    if (i + 2 < n) {
      v |= p[i + 2];
    } else
      pad = 1;
    out->data[out->len++] = T[(v >> 18) & 63];
    out->data[out->len++] = T[(v >> 12) & 63];
    out->data[out->len++] = (pad >= 1) ? '=' : T[(v >> 6) & 63];
    out->data[out->len++] = (pad >= 2) ? '=' : T[v & 63];
  }
  return 0;
}
AUX_INLINE int aux__b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return -2;
  if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
    return -3; /* ignorer espaces */
  return -1;
}
AUX_INLINE int aux_base64_decode(aux_sv in, aux_buf* out) {
  uint32_t buf = 0;
  int bits = 0;
  size_t i = 0;
  for (; i < in.n; i++) {
    int v = aux__b64v((unsigned char)in.p[i]);
    if (v == -3) continue; /* skip ws */
    if (v == -1) return -1;
    if (v == -2) {      /* '=' padding → flush */
      if (bits == 18) { /* one output */
        if (aux_buf_push_u8(out, (uint8_t)(buf >> 16))) return -1;
      } else if (bits == 24) { /* two output */
        if (aux_buf_push_u8(out, (uint8_t)(buf >> 16))) return -1;
        if (aux_buf_push_u8(out, (uint8_t)((buf >> 8) & 0xFF))) return -1;
      }
      /* ignore rest of input */
      return 0;
    }
    buf = (buf << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 24) {
      if (aux_buf_push_u8(out, (uint8_t)(buf >> 16))) return -1;
      if (aux_buf_push_u8(out, (uint8_t)((buf >> 8) & 0xFF))) return -1;
      if (aux_buf_push_u8(out, (uint8_t)(buf & 0xFF))) return -1;
      buf = 0;
      bits = 0;
    }
  }
  if (bits == 12) return -1;
  if (bits == 18) {
    if (aux_buf_push_u8(out, (uint8_t)(buf >> 10))) return -1;
  } else if (bits == 24) {
    if (aux_buf_push_u8(out, (uint8_t)(buf >> 16))) return -1;
    if (aux_buf_push_u8(out, (uint8_t)((buf >> 8) & 0xFF))) return -1;
  }
  return 0;
}

/* ---------- hex ---------- */
AUX_INLINE int aux_hex_encode(const void* src, size_t n, aux_buf* out,
                              int upper) {
  const uint8_t* p = (const uint8_t*)src;
  const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (aux_buf_reserve(out, out->len + n * 2) != 0) return -1;
  for (size_t i = 0; i < n; i++) {
    out->data[out->len++] = d[(p[i] >> 4) & 0xF];
    out->data[out->len++] = d[(p[i]) & 0xF];
  }
  return 0;
}
AUX_INLINE int aux__hexv(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
AUX_INLINE int aux_hex_decode(aux_sv in, aux_buf* out) {
  if (in.n % 2) return -1;
  if (aux_buf_reserve(out, out->len + in.n / 2) != 0) return -1;
  for (size_t i = 0; i < in.n; i += 2) {
    int hi = aux__hexv((unsigned char)in.p[i]);
    int lo = aux__hexv((unsigned char)in.p[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out->data[out->len++] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

/* ---------- temps ---------- */
AUX_INLINE uint64_t aux_time_unix_ns(void) {
#if defined(_WIN32)
  /* Windows FILETIME: 100ns since 1601-01-01 */
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  uint64_t t100 = u.QuadPart;                      /* 100ns */
  uint64_t unix100 = t100 - 116444736000000000ull; /* delta 1601→1970 */
  return unix100 * 100ull;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
AUX_INLINE uint64_t aux_time_mono_ns(void) {
#if defined(_WIN32)
  static LARGE_INTEGER freq;
  static int inited = 0;
  if (!inited) {
    QueryPerformanceFrequency(&freq);
    inited = 1;
  }
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (uint64_t)((c.QuadPart * 1000000000ULL) / freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
AUX_INLINE double aux_time_mono_sec(void) {
  return (double)aux_time_mono_ns() / 1e9;
}
AUX_INLINE int aux_time_iso8601(char* dst, size_t dstsz, time_t t, int utc) {
  struct tm tmv;
#if defined(_WIN32)
  if (utc) {
    gmtime_s(&tmv, &t);
  } else {
    localtime_s(&tmv, &t);
  }
#else
  if (utc) {
    gmtime_r(&t, &tmv);
  } else {
    localtime_r(&t, &tmv);
  }
#endif
  int n = snprintf(dst, dstsz, "%04d-%02d-%02dT%02d:%02d:%02d%s",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                   tmv.tm_min, tmv.tm_sec, utc ? "Z" : "");
  return (n > 0 && (size_t)n < dstsz) ? 0 : -1;
}

/* ---------- fichiers/chemins/env ---------- */
AUX_INLINE int aux_file_exists(const char* path) {
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(path);
  return (attr != INVALID_FILE_ATTRIBUTES);
#else
  return access(path, F_OK) == 0;
#endif
}
AUX_INLINE int aux_file_size(const char* path, int64_t* sz) {
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return -1;
  }
  fclose(f);
  if (sz) *sz = (int64_t)n;
  return 0;
}
AUX_INLINE int aux_file_read_all(const char* path, aux_buf* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return -1;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  if (aux_buf_resize(out, (size_t)n) != 0) {
    fclose(f);
    return -1;
  }
  size_t rd = fread(out->data, 1, (size_t)n, f);
  fclose(f);
  if (rd != (size_t)n) return -1;
  return 0;
}
AUX_INLINE int aux_file_write_all(const char* path, const void* data,
                                  size_t n) {
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  size_t wr = fwrite(data, 1, n, f);
  int ok = (wr == n) && (fflush(f) == 0) && (fclose(f) == 0);
  if (!ok) {
    fclose(f);
    return -1;
  }
  return 0;
}
AUX_INLINE int aux__mkdir_one(const char* p) {
#if defined(_WIN32)
  return _mkdir(p) == 0 || errno == EEXIST;
#else
  return mkdir(p, 0775) == 0 || errno == EEXIST;
#endif
}
AUX_INLINE int aux_mkdirs(const char* path) {
  /* crée chaque segment */
  char tmp[4096];
  size_t L = strlen(path);
  if (L >= sizeof(tmp)) return -1;
  memcpy(tmp, path, L + 1);
  for (size_t i = 1; i < L; i++) {
    if (tmp[i] == '/' || tmp[i] == '\\') {
      char c = tmp[i];
      tmp[i] = '\0';
      if (tmp[0]) {
        if (!aux__mkdir_one(tmp)) return -1;
      }
      tmp[i] = c;
    }
  }
  return aux__mkdir_one(tmp) ? 0 : -1;
}
AUX_INLINE const char* aux_getenv_str(const char* key) {
#if defined(_WIN32)
  /* _dupenv_s pour sécurité, mais ici on expose le pointeur CRT (non
   * threadsafe) */
  return getenv(key);
#else
  return getenv(key);
#endif
}
AUX_INLINE int aux_path_is_abs(const char* p) {
  if (!p || !p[0]) return 0;
#if defined(_WIN32)
  return ((p[0] == '\\' || p[0] == '/') ||
          (p[1] == ':' && (p[2] == '\\' || p[2] == '/')));
#else
  return p[0] == '/';
#endif
}
AUX_INLINE int aux_path_join2(const char* a, const char* b, aux_buf* out) {
  size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
  if (la && (a[la - 1] == '/' || a[la - 1] == '\\')) la--;
  size_t need = la + (la ? 1 : 0) + lb;
  if (aux_buf_reserve(out, out->len + need + 1) != 0) return -1;
  if (la) {
    memcpy(out->data + out->len, a, la);
    out->len += la;
    out->data[out->len++] = AUX_PATH_SEP;
  }
  if (lb) {
    memcpy(out->data + out->len, b, lb);
    out->len += lb;
  }
  out->data[out->len] = 0;
  return 0;
}
AUX_INLINE void aux_path_normalize(char* p) {
  if (!p) return;
  char* w = p;
  for (char* r = p; *r; ++r) {
#if defined(_WIN32)
    char c = (*r == '/') ? '\\' : *r;
    if (!(c == '\\' && w > p && w[-1] == '\\')) *w++ = c;
#else
    char c = (*r == '\\') ? '/' : *r;
    if (!(c == '/' && w > p && w[-1] == '/')) *w++ = c;
#endif
  }
  *w = 0;
}

/* ---------- UTF-8 ---------- */
AUX_INLINE int aux_utf8_decode(const char* s, size_t n, uint32_t* cp) {
  if (n == 0) return -1;
  unsigned char c0 = (unsigned char)s[0];
  if (c0 < 0x80) {
    if (cp) *cp = c0;
    return 1;
  }
  if ((c0 & 0xE0) == 0xC0) {
    if (n < 2) return -1;
    unsigned c1 = s[1];
    if ((c1 & 0xC0) != 0x80) return -1;
    uint32_t u = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    if (u < 0x80) return -1;
    if (cp) *cp = u;
    return 2;
  }
  if ((c0 & 0xF0) == 0xE0) {
    if (n < 3) return -1;
    unsigned c1 = s[1], c2 = s[2];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return -1;
    uint32_t u = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
    if (u < 0x800 || (u >= 0xD800 && u <= 0xDFFF)) return -1;
    if (cp) *cp = u;
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0) {
    if (n < 4) return -1;
    unsigned c1 = s[1], c2 = s[2], c3 = s[3];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
      return -1;
    uint32_t u = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                 ((c2 & 0x3F) << 6) | (c3 & 0x3F);
    if (u < 0x10000 || u > 0x10FFFF) return -1;
    if (cp) *cp = u;
    return 4;
  }
  return -1;
}
AUX_INLINE int aux_utf8_encode(uint32_t u, char out[4]) {
  if (u <= 0x7F) {
    out[0] = (char)u;
    return 1;
  }
  if (u <= 0x7FF) {
    out[0] = (char)(0xC0 | (u >> 6));
    out[1] = (char)(0x80 | (u & 0x3F));
    return 2;
  }
  if (u >= 0xD800 && u <= 0xDFFF) return -1;
  if (u <= 0xFFFF) {
    out[0] = (char)(0xE0 | (u >> 12));
    out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    out[2] = (char)(0x80 | (u & 0x3F));
    return 3;
  }
  if (u <= 0x10FFFF) {
    out[0] = (char)(0xF0 | (u >> 18));
    out[1] = (char)(0x80 | ((u >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((u >> 6) & 0x3F));
    out[3] = (char)(0x80 | (u & 0x3F));
    return 4;
  }
  return -1;
}
AUX_INLINE int aux_utf8_valid(const char* s, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint32_t cp;
    int k = aux_utf8_decode(s + i, n - i, &cp);
    if (k < 0) return 0;
    i += (size_t)k;
  }
  return 1;
}

#endif /* AUX_HEADER_ONLY */

/* ------------------------------------------------------------------------- */
#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* AUXLIB_H */
