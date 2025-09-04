// vitte-light/libraries/strlib.c
// String library for VitteLight: UTF‑8 helpers + rich string natives.
// Self-contained C99. No external deps except VL runtime headers.
//
// Public entry:
//   void vl_register_strlib(struct VL_Context *ctx);
//
// Natives exported:
//   s_ascii_lower(s)        -> str        // ASCII only
//   s_ascii_upper(s)        -> str
//   s_trim(s)               -> str        // ASCII <= 0x20
//   s_ltrim(s)              -> str
//   s_rtrim(s)              -> str
//   s_startswith(s,prefix)  -> bool
//   s_endswith(s,suffix)    -> bool
//   s_repeat(s,n)           -> str
//   s_pad_left(s,width[,ch])-> str        // ch: first byte used, default ' '
//   s_pad_right(s,width[,ch])-> str
//   s_replace(s,from,to)    -> str        // replace all (byte-wise)
//   s_replace_n(s,from,to,n)-> str        // replace at most n
//   s_reverse(s)            -> str        // UTF‑8 aware
//   s_len_cp(s)             -> int        // codepoint count
//   s_slice_cp(s,st,len)    -> str        // slice by codepoints
//   s_hex(s)                -> str        // hex encode bytes
//   s_unhex(hex)            -> str|error  // decode hex -> bytes
//   s_b64enc(s)             -> str        // base64 encode
//   s_b64dec(s)             -> str|error  // base64 decode
//   s_urlenc(s)             -> str        // percent-encoding
//   s_urldec(s)             -> str|error  // percent-decoding
//   s_json_escape(s)        -> str        // escape JSON string content (no
//   quotes) s_hash64(s)             -> int        // FNV-1a 64-bit
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/strlib.c

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"
#include "mem.h"     // VL_Buffer
#include "string.h"  // VL_String, vl_make_strn

// ───────────────────────── VM helpers ─────────────────────────
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

// ───────────────────────── ASCII transforms ─────────────────────────
static VL_Status s_ascii_lower(struct VL_Context *ctx, const VL_Value *a,
                               uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  size_t L = a[0].as.s->len;
  const unsigned char *s = (const unsigned char *)a[0].as.s->data;
  char *tmp = (char *)malloc(L);
  if (!tmp) return VL_ERR_OOM;
  for (size_t i = 0; i < L; i++) {
    unsigned char ch = s[i];
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    tmp[i] = (char)ch;
  }
  RET_STR_BYTES(tmp, L);
}
static VL_Status s_ascii_upper(struct VL_Context *ctx, const VL_Value *a,
                               uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  size_t L = a[0].as.s->len;
  const unsigned char *s = (const unsigned char *)a[0].as.s->data;
  char *tmp = (char *)malloc(L);
  if (!tmp) return VL_ERR_OOM;
  for (size_t i = 0; i < L; i++) {
    unsigned char ch = s[i];
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    tmp[i] = (char)ch;
  }
  RET_STR_BYTES(tmp, L);
}

// ───────────────────────── Trim ─────────────────────────
static inline int is_space_ascii(unsigned char c) { return c <= 0x20; }
static VL_Status s_trim(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const char *s = a[0].as.s->data;
  size_t L = a[0].as.s->len;
  size_t i = 0, j = L;
  while (i < j && is_space_ascii((unsigned char)s[i])) i++;
  while (j > i && is_space_ascii((unsigned char)s[j - 1])) j--;
  RET_STR_BYTES(s + i, j - i);
}
static VL_Status s_ltrim(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const char *s = a[0].as.s->data;
  size_t L = a[0].as.s->len;
  size_t i = 0;
  while (i < L && is_space_ascii((unsigned char)s[i])) i++;
  RET_STR_BYTES(s + i, L - i);
}
static VL_Status s_rtrim(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const char *s = a[0].as.s->data;
  size_t L = a[0].as.s->len;
  size_t j = L;
  while (j > 0 && is_space_ascii((unsigned char)s[j - 1])) j--;
  RET_STR_BYTES(s, j);
}

// ───────────────────────── Prefix/Suffix ─────────────────────────
static VL_Status s_startswith(struct VL_Context *ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 2 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  const VL_String *S = a[0].as.s, *P = a[1].as.s;
  RET_BOOL(S->len >= P->len && memcmp(S->data, P->data, P->len) == 0);
}
static VL_Status s_endswith(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 2 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  const VL_String *S = a[0].as.s, *P = a[1].as.s;
  RET_BOOL(S->len >= P->len &&
           memcmp(S->data + S->len - P->len, P->data, P->len) == 0);
}

// ───────────────────────── Repeat / Pad ─────────────────────────
static VL_Status s_repeat(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)u;
  if (c < 2 || !need_str(&a[0])) return VL_ERR_TYPE;
  int64_t n = 0;
  if (!vl_value_as_int(&a[1], &n) || n < 0) return VL_ERR_INVAL;
  const VL_String *S = a[0].as.s;
  if ((uint64_t)S->len * (uint64_t)n > (uint64_t)UINT32_MAX) return VL_ERR_OOM;
  VL_Buffer b;
  vl_buf_init(&b);
  for (int64_t i = 0; i < n; i++) {
    vl_buf_append(&b, S->data, S->len);
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}

static unsigned char get_pad_ch(const VL_Value *v) {
  if (!v || v->type != VT_STR || !v->as.s || v->as.s->len == 0) return ' ';
  return (unsigned char)v->as.s->data[0];
}
static VL_Status pad_common(struct VL_Context *ctx, const VL_String *S,
                            int left, int64_t width, unsigned char ch,
                            VL_Value *ret) {
  if (width <= 0) RET_STR_BYTES(S->data, S->len);
  if ((uint64_t)width > UINT32_MAX) return VL_ERR_OOM;
  size_t w = (size_t)width;
  size_t cur = S->len;
  if (cur >= w) RET_STR_BYTES(S->data, S->len);
  size_t pad = w - cur;
  char *tmp = (char *)malloc(w);
  if (!tmp) return VL_ERR_OOM;
  if (left) {
    memset(tmp, ch, pad);
    memcpy(tmp + pad, S->data, S->len);
  } else {
    memcpy(tmp, S->data, S->len);
    memset(tmp + S->len, ch, pad);
  }
  RET_STR_BYTES(tmp, w);
}
static VL_Status s_pad_left(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 2 || !need_str(&a[0])) return VL_ERR_TYPE;
  int64_t w = 0;
  if (!vl_value_as_int(&a[1], &w) || w < 0) return VL_ERR_INVAL;
  unsigned char ch = (c >= 3) ? get_pad_ch(&a[2]) : ' ';
  return pad_common(ctx, a[0].as.s, 1, w, ch, ret);
}
static VL_Status s_pad_right(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 2 || !need_str(&a[0])) return VL_ERR_TYPE;
  int64_t w = 0;
  if (!vl_value_as_int(&a[1], &w) || w < 0) return VL_ERR_INVAL;
  unsigned char ch = (c >= 3) ? get_pad_ch(&a[2]) : ' ';
  return pad_common(ctx, a[0].as.s, 0, w, ch, ret);
}

// ───────────────────────── Replace (byte-wise) ─────────────────────────
static VL_Status s_replace_n(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 3 || !need_str(&a[0]) || !need_str(&a[1]) || !need_str(&a[2]))
    return VL_ERR_TYPE;
  int64_t nmax = (c >= 4 && a[3].type != VT_NIL) ? 0 : 0;
  if (c >= 4 && a[3].type != VT_NIL) {
    if (!vl_value_as_int(&a[3], &nmax) || nmax < 0) return VL_ERR_INVAL;
  }
  const VL_String *S = a[0].as.s, *F = a[1].as.s, *T = a[2].as.s;
  if (F->len == 0) {  // degenerate: insert T between bytes, max nmax
    VL_Buffer b;
    vl_buf_init(&b);
    size_t reps = (nmax == 0)
                      ? (S->len ? S->len - 1 : 0)
                      : (size_t)((S->len ? S->len - 1 : 0) < (size_t)nmax
                                     ? (S->len ? S->len - 1 : 0)
                                     : (size_t)nmax);
    for (size_t i = 0; i < S->len; i++) {
      vl_buf_append(&b, S->data + i, 1);
      if (i + 1 < S->len && reps) {
        vl_buf_append(&b, T->data, T->len);
        if (nmax > 0) nmax--;
      }
    }
    VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
    vl_buf_free(&b);
    if (out.type != VT_STR) return VL_ERR_OOM;
    if (ret) *ret = out;
    return VL_OK;
  }
  VL_Buffer b;
  vl_buf_init(&b);
  const char *p = S->data;
  size_t i = 0;
  int64_t done = 0;
  while (i + F->len <= S->len) {
    if (memcmp(p + i, F->data, F->len) == 0 && (nmax == 0 || done < nmax)) {
      vl_buf_append(&b, T->data, T->len);
      i += F->len;
      done++;
    } else {
      vl_buf_append(&b, p + i, 1);
      i++;
    }
  }
  if (i < S->len) vl_buf_append(&b, p + i, S->len - i);
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}
static VL_Status s_replace(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *u) {
  return s_replace_n(ctx, a, c, ret, u);
}

// ───────────────────────── UTF‑8 core ─────────────────────────
static int utf8_decode(const unsigned char *s, size_t n, uint32_t *cp,
                       size_t *adv) {
  if (n == 0) return 0;
  unsigned c0 = s[0];
  if (c0 < 0x80) {
    if (cp) *cp = c0;
    if (adv) *adv = 1;
    return 1;
  }
  if ((c0 & 0xE0) == 0xC0) {
    if (n < 2) return 0;
    unsigned c1 = s[1];
    if ((c1 & 0xC0) != 0x80) return 0;
    uint32_t u = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    if (u < 0x80) return 0;
    if (cp) *cp = u;
    if (adv) *adv = 2;
    return 1;
  }
  if ((c0 & 0xF0) == 0xE0) {
    if (n < 3) return 0;
    unsigned c1 = s[1], c2 = s[2];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return 0;
    uint32_t u = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
    if (u < 0x800 || (u >= 0xD800 && u <= 0xDFFF)) return 0;
    if (cp) *cp = u;
    if (adv) *adv = 3;
    return 1;
  }
  if ((c0 & 0xF8) == 0xF0) {
    if (n < 4) return 0;
    unsigned c1 = s[1], c2 = s[2], c3 = s[3];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
      return 0;
    uint32_t u = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                 ((c2 & 0x3F) << 6) | (c3 & 0x3F);
    if (u < 0x10000 || u > 0x10FFFF) return 0;
    if (cp) *cp = u;
    if (adv) *adv = 4;
    return 1;
  }
  return 0;
}

static size_t utf8_count(const char *s, size_t n, int *ok) {
  *ok = 1;
  size_t i = 0, cnt = 0;
  while (i < n) {
    size_t adv = 0;
    if (!utf8_decode((const unsigned char *)s + i, n - i, NULL, &adv)) {
      *ok = 0;
      break;
    }
    i += adv;
    cnt++;
  }
  return cnt;
}

static int utf8_slice_bounds(const char *s, size_t n, int64_t st_cp,
                             int64_t len_cp, size_t *boff, size_t *blen) {
  if (st_cp < 0) st_cp = 0;
  size_t off = 0;
  for (int64_t k = 0; k < st_cp && off < n; k++) {
    size_t adv = 0;
    if (!utf8_decode((const unsigned char *)s + off, n - off, NULL, &adv))
      return 0;
    off += adv;
  }
  if (off > n) off = n;
  size_t end = off;
  if (len_cp < 0) {
    end = n;
  } else {
    for (int64_t k = 0; k < len_cp && end < n; k++) {
      size_t adv = 0;
      if (!utf8_decode((const unsigned char *)s + end, n - end, NULL, &adv))
        return 0;
      end += adv;
    }
  }
  *boff = off;
  *blen = (end > n ? n : end) - off;
  return 1;
}

// ───────────────────────── UTF‑8 ops ─────────────────────────
static VL_Status s_len_cp(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  int ok = 0;
  size_t cnt = utf8_count(a[0].as.s->data, a[0].as.s->len, &ok);
  if (!ok) return VL_ERR_INVAL;
  RET_INT((int64_t)cnt);
}

static VL_Status s_slice_cp(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 3 || !need_str(&a[0])) return VL_ERR_TYPE;
  int64_t st = 0, len = -1;
  if (!vl_value_as_int(&a[1], &st)) return VL_ERR_TYPE;
  if (!vl_value_as_int(&a[2], &len)) return VL_ERR_TYPE;
  size_t off = 0, bl = 0;
  if (!utf8_slice_bounds(a[0].as.s->data, a[0].as.s->len, st, len, &off, &bl))
    return VL_ERR_INVAL;
  RET_STR_BYTES(a[0].as.s->data + off, bl);
}

static VL_Status s_reverse(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const char *s = a[0].as.s->data;
  size_t n = a[0].as.s->len;  // collect byte offsets of each rune
  size_t cap = 128, m = 0;
  size_t *ofs = (size_t *)malloc(cap * sizeof(size_t));
  if (!ofs) return VL_ERR_OOM;
  size_t i = 0;
  while (i < n) {
    size_t adv = 0;
    if (!utf8_decode((const unsigned char *)s + i, n - i, NULL, &adv)) {
      free(ofs);
      return VL_ERR_INVAL;
    }
    if (m == cap) {
      size_t nc = cap * 2;
      size_t *no = (size_t *)realloc(ofs, nc * sizeof(size_t));
      if (!no) {
        free(ofs);
        return VL_ERR_OOM;
      }
      ofs = no;
      cap = nc;
    }
    ofs[m++] = i;
    i += adv;
  }
  VL_Buffer b;
  vl_buf_init(&b);
  for (size_t k = 0; k < m; k++) {
    size_t rb = ofs[m - 1 - k];
    size_t adv = 0;
    uint32_t cp = 0;
    utf8_decode((const unsigned char *)s + rb, n - rb, &cp, &adv);
    vl_buf_append(&b, s + rb, adv);
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  free(ofs);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}

// ───────────────────────── Hex ─────────────────────────
static inline int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}
static VL_Status s_hex(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                       VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const unsigned char *p = (const unsigned char *)a[0].as.s->data;
  size_t n = a[0].as.s->len;
  static const char *tab = "0123456789abcdef";
  VL_Buffer b;
  vl_buf_init(&b);
  vl_buf_reserve(&b, n * 2);
  for (size_t i = 0; i < n; i++) {
    char q[2] = {tab[p[i] >> 4], tab[p[i] & 15]};
    vl_buf_append(&b, q, 2);
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}
static VL_Status s_unhex(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const char *s = a[0].as.s->data;
  size_t n = a[0].as.s->len;
  if (n % 2) return VL_ERR_INVAL;
  VL_Buffer b;
  vl_buf_init(&b);
  vl_buf_reserve(&b, n / 2);
  for (size_t i = 0; i < n; i += 2) {
    int h = hexval((unsigned char)s[i]), l = hexval((unsigned char)s[i + 1]);
    if (h < 0 || l < 0) {
      vl_buf_free(&b);
      return VL_ERR_INVAL;
    }
    unsigned char v = (unsigned char)((h << 4) | l);
    vl_buf_append(&b, &v, 1);
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}

// ───────────────────────── Base64 ─────────────────────────
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static VL_Status s_b64enc(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const unsigned char *p = (const unsigned char *)a[0].as.s->data;
  size_t n = a[0].as.s->len;
  size_t outn = ((n + 2) / 3) * 4;
  VL_Buffer b;
  vl_buf_init(&b);
  vl_buf_reserve(&b, outn);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = p[i] << 16;
    if (i + 1 < n) v |= p[i + 1] << 8;
    if (i + 2 < n) v |= p[i + 2];
    char o[4];
    o[0] = b64tab[(v >> 18) & 63];
    o[1] = b64tab[(v >> 12) & 63];
    o[2] = (i + 1 < n) ? b64tab[(v >> 6) & 63] : '=';
    o[3] = (i + 2 < n) ? b64tab[v & 63] : '=';
    vl_buf_append(&b, o, 4);
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}

static int b64val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return -2;
  if (c == '\n' || c == '\r' || c == '\t' || c == ' ') return -3;
  return -1;
}
static VL_Status s_b64dec(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const unsigned char *s = (const unsigned char *)a[0].as.s->data;
  size_t n = a[0].as.s->len;
  VL_Buffer b;
  vl_buf_init(&b);
  int quad[4];
  int qi = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64val(s[i]);
    if (v == -3) continue;
    if (v < 0 && v != -2) {
      vl_buf_free(&b);
      return VL_ERR_INVAL;
    }
    quad[qi++] = v;
    if (qi == 4) {
      uint32_t x = 0;
      int pad = 0;
      for (int k = 0; k < 4; k++) {
        if (quad[k] == -2) {
          quad[k] = 0;
          pad++;
        }
        x = (x << 6) | (uint32_t)quad[k];
      }
      unsigned char o[3];
      o[0] = (x >> 16) & 0xFF;
      o[1] = (x >> 8) & 0xFF;
      o[2] = x & 0xFF;
      size_t outc = (pad >= 2) ? 1 : (pad == 1 ? 2 : 3);
      vl_buf_append(&b, o, outc);
      qi = 0;
    }
  }
  if (qi != 0) {
    vl_buf_free(&b);
    return VL_ERR_INVAL;
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}

// ───────────────────────── URL encode/decode ─────────────────────────
static int is_unreserved(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}
static VL_Status s_urlenc(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const unsigned char *p = (const unsigned char *)a[0].as.s->data;
  size_t n = a[0].as.s->len;
  VL_Buffer b;
  vl_buf_init(&b);
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = p[i];
    if (is_unreserved(ch)) {
      vl_buf_append(&b, &ch, 1);
    } else if (ch == ' ') {
      const char *pct = "%20";
      vl_buf_append(&b, pct, 3);
    } else {
      char q[3] = {'%', "0123456789ABCDEF"[ch >> 4],
                   "0123456789ABCDEF"[ch & 15]};
      vl_buf_append(&b, q, 3);
    }
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}
static VL_Status s_urldec(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const char *s = a[0].as.s->data;
  size_t n = a[0].as.s->len;
  VL_Buffer b;
  vl_buf_init(&b);
  for (size_t i = 0; i < n;) {
    unsigned char ch = s[i];
    if (ch == '%') {
      if (i + 2 >= n) {
        vl_buf_free(&b);
        return VL_ERR_INVAL;
      }
      int h = hexval((unsigned char)s[i + 1]),
          l = hexval((unsigned char)s[i + 2]);
      if (h < 0 || l < 0) {
        vl_buf_free(&b);
        return VL_ERR_INVAL;
      }
      unsigned char v = (unsigned char)((h << 4) | l);
      vl_buf_append(&b, &v, 1);
      i += 3;
    } else {
      vl_buf_append(&b, &ch, 1);
      i++;
    }
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}

// ───────────────────────── JSON escape ─────────────────────────
static VL_Status s_json_escape(struct VL_Context *ctx, const VL_Value *a,
                               uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const unsigned char *p = (const unsigned char *)a[0].as.s->data;
  size_t n = a[0].as.s->len;
  VL_Buffer b;
  vl_buf_init(&b);
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = p[i];
    switch (ch) {
      case '"':
        vl_buf_append(&b, "\\\"", 2);
        break;
      case '\\':
        vl_buf_append(&b, "\\\\", 2);
        break;
      case '\n':
        vl_buf_append(&b, "\\n", 2);
        break;
      case '\r':
        vl_buf_append(&b, "\\r", 2);
        break;
      case '\t':
        vl_buf_append(&b, "\\t", 2);
        break;
      default:
        if (ch < 0x20) {
          char uesc[6];
          snprintf(uesc, sizeof(uesc), "\\u%04X", ch);
          vl_buf_append(&b, uesc, 6);
        } else {
          vl_buf_append(&b, &ch, 1);
        }
    }
  }
  VL_Value out = vl_make_strn(ctx, (const char *)b.d, (uint32_t)b.n);
  vl_buf_free(&b);
  if (out.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = out;
  return VL_OK;
}

// ───────────────────────── Hash ─────────────────────────
static VL_Status s_hash64(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  const unsigned char *p = (const unsigned char *)a[0].as.s->data;
  size_t n = a[0].as.s->len;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  RET_INT((int64_t)h);
}

// ───────────────────────── Registration ─────────────────────────
void vl_register_strlib(struct VL_Context *ctx) {
  if (!ctx) return;
  vl_register_native(ctx, "s_ascii_lower", s_ascii_lower, NULL);
  vl_register_native(ctx, "s_ascii_upper", s_ascii_upper, NULL);
  vl_register_native(ctx, "s_trim", s_trim, NULL);
  vl_register_native(ctx, "s_ltrim", s_ltrim, NULL);
  vl_register_native(ctx, "s_rtrim", s_rtrim, NULL);
  vl_register_native(ctx, "s_startswith", s_startswith, NULL);
  vl_register_native(ctx, "s_endswith", s_endswith, NULL);
  vl_register_native(ctx, "s_repeat", s_repeat, NULL);
  vl_register_native(ctx, "s_pad_left", s_pad_left, NULL);
  vl_register_native(ctx, "s_pad_right", s_pad_right, NULL);
  vl_register_native(ctx, "s_replace", s_replace, NULL);
  vl_register_native(ctx, "s_replace_n", s_replace_n, NULL);
  vl_register_native(ctx, "s_reverse", s_reverse, NULL);
  vl_register_native(ctx, "s_len_cp", s_len_cp, NULL);
  vl_register_native(ctx, "s_slice_cp", s_slice_cp, NULL);
  vl_register_native(ctx, "s_hex", s_hex, NULL);
  vl_register_native(ctx, "s_unhex", s_unhex, NULL);
  vl_register_native(ctx, "s_b64enc", s_b64enc, NULL);
  vl_register_native(ctx, "s_b64dec", s_b64dec, NULL);
  vl_register_native(ctx, "s_urlenc", s_urlenc, NULL);
  vl_register_native(ctx, "s_urldec", s_urldec, NULL);
  vl_register_native(ctx, "s_json_escape", s_json_escape, NULL);
  vl_register_native(ctx, "s_hash64", s_hash64, NULL);
}

// ───────────────────────── Test (optional) ─────────────────────────
#ifdef VL_STRLIB_TEST_MAIN
#include <inttypes.h>
int main(void) {
  const char *u = "hé🐱";  // UTF‑8
  // Local smoke tests
  int ok = 0;
  size_t cnt = utf8_count(u, strlen(u), &ok);
  printf("cnt=%zu ok=%d\n", cnt, ok);
  size_t off = 0, bl = 0;
  utf8_slice_bounds(u, strlen(u), 1, 1, &off, &bl);
  fwrite(u + off, 1, bl, stdout);
  putchar('\n');
  return 0;
}
#endif
