// SPDX-License-Identifier: GPL-3.0-or-later
//
// strlib.c — String library for Vitte Light VM (C17, complet)
// Namespace: "str"
//
// Conventions:
//   - Byte indexing is 1-based. Negative indexes are from end (-1 == last).
//   - All operations are byte-based unless prefixed with utf8_*.
//   - Functions return (nil,"EINVAL") or (nil,"ERANGE") on invalid args/ranges.
//   - split() returns N results as multiple return values.
//
// API:
//   Basics:
//     str.len(s)                       -> int
//     str.byte_at(s, i)                -> int (0..255) | (nil,"ERANGE")
//     str.sub(s, i[, j])               -> string
//     str.find(s, needle[, start=1[, nocase=false]]) -> pos:int (0 if not
//     found) str.replace(s, from, to[, max=-1[, nocase=false]]) -> out:string,
//     count:int str.split(s, sep[, max=-1])      -> v1, v2, ... (N returns)
//     str.lower(s)                     -> string
//     str.upper(s)                     -> string
//     str.trim(s)                      -> string
//     str.ltrim(s)                     -> string
//     str.rtrim(s)                     -> string
//     str.starts_with(s, pfx[, nocase=false]) -> bool
//     str.ends_with(s, sfx[, nocase=false])   -> bool
//     str.repeat(s, n)                 -> string | (nil,"ERANGE")
//     str.reverse(s)                   -> string
//     str.pad_left(s, width[, ch=" "]) -> string
//     str.pad_right(s, width[, ch=" "])-> string
//     str.cmp(a,b[, nocase=false])     -> -1|0|1
//     str.hash32(s)                    -> uint32 (FNV-1a)
//
//   Encoding:
//     str.hex(s)                       -> hex:string
//     str.unhex(hex)                   -> bytes:string | (nil,"EINVAL")
//     str.base64_encode(s)             -> b64:string
//     str.base64_decode(b64)           -> bytes:string | (nil,"EINVAL")
//
//   UTF-8 helpers (best-effort, validation-light):
//     str.utf8_len(s)                  -> codepoints:int
//     str.utf8_sub(s, i[, j])          -> substring by codepoints
//
// Depends: auxlib.h, state.h, object.h, vm.h

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *st_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t st_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int st_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int st_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)st_check_int(S, idx);
  return defv;
}

// ---------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------
static size_t clamp_pos_1b(int64_t i, size_t n) {
  // 1-based, negatives from end
  if (i < 0) {
    int64_t p = (int64_t)n + i + 1;  // -1 -> n
    if (p < 1) p = 1;
    if ((uint64_t)p > n) p = (int64_t)n;
    return (size_t)p;
  }
  if (i < 1) return 1;
  if ((uint64_t)i > n) return n;
  return (size_t)i;
}
static int eq_char_ci(unsigned char a, unsigned char b) {
  return (int)tolower(a) == (int)tolower(b);
}

static const char *memmem_case(const char *hay, size_t hlen, const char *needle,
                               size_t nlen, int nocase) {
  if (nlen == 0) return hay;
  if (hlen < nlen) return NULL;
  if (!nocase) {
    // naive search
    const char *end = hay + (hlen - nlen) + 1;
    for (const char *p = hay; p < end; ++p) {
      if (*p == *needle && memcmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
  } else {
    const unsigned char *H = (const unsigned char *)hay;
    const unsigned char *N = (const unsigned char *)needle;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
      if (!eq_char_ci(H[i], N[0])) continue;
      size_t j = 1;
      for (; j < nlen; ++j)
        if (!eq_char_ci(H[i + j], N[j])) break;
      if (j == nlen) return (const char *)(H + i);
    }
    return NULL;
  }
}

// FNV-1a 32-bit
static uint32_t fnv1a32(const uint8_t *p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

// Base64
static const char B64TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// UTF-8 decode next; returns bytes consumed (1..4) or 0 on invalid
static size_t utf8_next(const unsigned char *s, size_t n, uint32_t *out_cp) {
  if (n == 0) return 0;
  unsigned char c = s[0];
  if (c < 0x80) {
    if (out_cp) *out_cp = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0 && n >= 2) {
    uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    if (cp < 0x80) return 0;  // overlong
    if (out_cp) *out_cp = cp;
    return 2;
  }
  if ((c & 0xF0) == 0xE0 && n >= 3) {
    uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                  ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    if (cp < 0x800) return 0;  // overlong
    if (out_cp) *out_cp = cp;
    return 3;
  }
  if ((c & 0xF8) == 0xF0 && n >= 4) {
    uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
                  ((uint32_t)(s[1] & 0x3F) << 12) |
                  ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    if (cp < 0x10000 || cp > 0x10FFFF) return 0;  // overlong or out of range
    if (out_cp) *out_cp = cp;
    return 4;
  }
  return 0;
}

// ---------------------------------------------------------------------
// VM functions
// ---------------------------------------------------------------------

static int vm_str_len(VL_State *S) {
  const char *s = st_check_str(S, 1);
  vl_push_int(S, (int64_t)strlen(s));
  return 1;
}

static int vm_str_byte_at(VL_State *S) {
  const char *s = st_check_str(S, 1);
  int64_t i = st_check_int(S, 2);
  size_t n = strlen(s);
  if (n == 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  size_t p = clamp_pos_1b(i, n);
  if (p < 1 || p > n) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  vl_push_int(S, (int64_t)(unsigned char)s[p - 1]);
  return 1;
}

static int vm_str_sub(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s);
  int have_j = (vl_get(S, 3) != NULL);
  int64_t i = st_check_int(S, 2);
  int64_t j = have_j ? st_check_int(S, 3) : (int64_t)n;
  size_t si = clamp_pos_1b(i, n);
  size_t sj = clamp_pos_1b(j, n);
  if (sj < si) {
    vl_push_string(S, "");
    return 1;
  }
  size_t len = sj - si + 1;
  vl_push_lstring(S, s + (si - 1), (int)len);
  return 1;
}

static int vm_str_find(VL_State *S) {
  const char *s = st_check_str(S, 1);
  const char *needle = st_check_str(S, 2);
  size_t n = strlen(s), m = strlen(needle);
  size_t start = (size_t)clamp_pos_1b(st_opt_int(S, 3, 1), n == 0 ? 1 : (int)n);
  int nocase = st_opt_bool(S, 4, 0);
  if (m == 0) {
    vl_push_int(S, (int64_t)start);
    return 1;
  }
  if (start < 1) start = 1;
  if (start > n) {
    vl_push_int(S, 0);
    return 1;
  }
  const char *p =
      memmem_case(s + (start - 1), n - (start - 1), needle, m, nocase);
  if (!p) {
    vl_push_int(S, 0);
    return 1;
  }
  size_t pos = (size_t)(p - s) + 1;
  vl_push_int(S, (int64_t)pos);
  return 1;
}

static int vm_str_replace(VL_State *S) {
  const char *s = st_check_str(S, 1);
  const char *from = st_check_str(S, 2);
  const char *to = st_check_str(S, 3);
  int maxrep = st_opt_int(S, 4, -1);
  int nocase = st_opt_bool(S, 5, 0);
  size_t sn = strlen(s), fn = strlen(from), tn = strlen(to);
  if (fn == 0) {
    vl_push_string(S, s);
    vl_push_int(S, 0);
    return 2;
  }

  AuxBuffer out = {0};
  size_t i = 0, count = 0;
  while (i < sn) {
    const char *p = memmem_case(s + i, sn - i, from, fn, nocase);
    if (!p) {
      aux_buffer_append(&out, (const uint8_t *)s + i, sn - i);
      break;
    }
    size_t off = (size_t)(p - s);
    aux_buffer_append(&out, (const uint8_t *)s + i, off - i);
    aux_buffer_append(&out, (const uint8_t *)to, tn);
    i = off + fn;
    count++;
    if (maxrep >= 0 && (int)count >= maxrep) {
      aux_buffer_append(&out, (const uint8_t *)s + i, sn - i);
      break;
    }
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  vl_push_int(S, (int64_t)count);
  aux_buffer_free(&out);
  return 2;
}

static int vm_str_split(VL_State *S) {
  const char *s = st_check_str(S, 1);
  const char *sep = st_check_str(S, 2);
  int maxparts = st_opt_int(S, 3, -1);
  size_t n = strlen(s), sp = strlen(sep);
  if (sp == 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  int pushed = 0;
  size_t i = 0;
  while (i <= n) {
    if (maxparts == 1) {
      vl_push_lstring(S, s + i, (int)(n - i));
      pushed++;
      break;
    }
    const char *p = memmem_case(s + i, n - i, sep, sp, 0);
    if (!p) {
      vl_push_lstring(S, s + i, (int)(n - i));
      pushed++;
      break;
    }
    size_t off = (size_t)(p - s);
    vl_push_lstring(S, s + i, (int)(off - i));
    pushed++;
    i = off + sp;
    if (maxparts > 0) maxparts--;
  }
  return pushed;
}

static int vm_str_lower(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s);
  char *buf = (char *)malloc(n + 1);
  if (!buf) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
  buf[n] = 0;
  vl_push_string(S, buf);
  free(buf);
  return 1;
}
static int vm_str_upper(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s);
  char *buf = (char *)malloc(n + 1);
  if (!buf) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  for (size_t i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)s[i]);
  buf[n] = 0;
  vl_push_string(S, buf);
  free(buf);
  return 1;
}

static int vm_str_trim(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s);
  size_t a = 0, b = n;
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  vl_push_lstring(S, s + a, (int)(b - a));
  return 1;
}
static int vm_str_ltrim(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s), a = 0;
  while (a < n && isspace((unsigned char)s[a])) a++;
  vl_push_lstring(S, s + a, (int)(n - a));
  return 1;
}
static int vm_str_rtrim(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s), b = n;
  while (b > 0 && isspace((unsigned char)s[b - 1])) b--;
  vl_push_lstring(S, s, (int)b);
  return 1;
}

static int vm_str_starts_with(VL_State *S) {
  const char *s = st_check_str(S, 1);
  const char *pfx = st_check_str(S, 2);
  int nocase = st_opt_bool(S, 3, 0);
  size_t n = strlen(s), m = strlen(pfx);
  if (m > n) {
    vl_push_bool(S, 0);
    return 1;
  }
  int ok = nocase ? (strncasecmp(s, pfx, m) == 0) : (memcmp(s, pfx, m) == 0);
#ifndef _WIN32
  // strncasecmp available on POSIX; on Windows emulate
#else
  if (nocase) {
    ok = 1;
    for (size_t i = 0; i < m; i++)
      if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i])) {
        ok = 0;
        break;
      }
  }
#endif
  vl_push_bool(S, ok ? 1 : 0);
  return 1;
}

static int vm_str_ends_with(VL_State *S) {
  const char *s = st_check_str(S, 1);
  const char *sfx = st_check_str(S, 2);
  int nocase = st_opt_bool(S, 3, 0);
  size_t n = strlen(s), m = strlen(sfx);
  if (m > n) {
    vl_push_bool(S, 0);
    return 1;
  }
  const char *a = s + (n - m);
  int ok;
  if (!nocase)
    ok = (memcmp(a, sfx, m) == 0);
  else {
    ok = 1;
    for (size_t i = 0; i < m; i++)
      if (tolower((unsigned char)a[i]) != tolower((unsigned char)sfx[i])) {
        ok = 0;
        break;
      }
  }
  vl_push_bool(S, ok ? 1 : 0);
  return 1;
}

static int vm_str_repeat(VL_State *S) {
  const char *s = st_check_str(S, 1);
  int64_t times = st_check_int(S, 2);
  if (times < 0 || times > (1 << 20)) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  size_t n = strlen(s);
  uint64_t need = (uint64_t)n * (uint64_t)times;
  if (need > (uint64_t)(32 * 1024 * 1024)) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  char *buf = (char *)malloc((size_t)need + 1);
  if (!buf) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  char *p = buf;
  for (int64_t i = 0; i < times; i++) {
    memcpy(p, s, n);
    p += n;
  }
  buf[need] = 0;
  vl_push_string(S, buf);
  free(buf);
  return 1;
}

static int vm_str_reverse(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s);
  char *b = (char *)malloc(n + 1);
  if (!b) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  for (size_t i = 0; i < n; i++) b[i] = s[n - 1 - i];
  b[n] = 0;
  vl_push_string(S, b);
  free(b);
  return 1;
}

static int vm_str_pad_left(VL_State *S) {
  const char *s = st_check_str(S, 1);
  int width = (int)st_check_int(S, 2);
  const char *pch =
      (vl_get(S, 3) && vl_isstring(S, 3)) ? st_check_str(S, 3) : " ";
  unsigned char ch = (unsigned char)(pch[0] ? pch[0] : ' ');
  size_t n = strlen(s);
  if (width <= 0 || (size_t)width <= n) {
    vl_push_string(S, s);
    return 1;
  }
  size_t pad = (size_t)width - n;
  char *b = (char *)malloc((size_t)width + 1);
  if (!b) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  memset(b, ch, pad);
  memcpy(b + pad, s, n);
  b[width] = 0;
  vl_push_string(S, b);
  free(b);
  return 1;
}
static int vm_str_pad_right(VL_State *S) {
  const char *s = st_check_str(S, 1);
  int width = (int)st_check_int(S, 2);
  const char *pch =
      (vl_get(S, 3) && vl_isstring(S, 3)) ? st_check_str(S, 3) : " ";
  unsigned char ch = (unsigned char)(pch[0] ? pch[0] : ' ');
  size_t n = strlen(s);
  if (width <= 0 || (size_t)width <= n) {
    vl_push_string(S, s);
    return 1;
  }
  size_t pad = (size_t)width - n;
  char *b = (char *)malloc((size_t)width + 1);
  if (!b) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  memcpy(b, s, n);
  memset(b + n, ch, pad);
  b[width] = 0;
  vl_push_string(S, b);
  free(b);
  return 1;
}

static int vm_str_cmp(VL_State *S) {
  const char *a = st_check_str(S, 1);
  const char *b = st_check_str(S, 2);
  int nocase = st_opt_bool(S, 3, 0);
  int r;
  if (!nocase)
    r = strcmp(a, b);
  else {
    size_t i = 0;
    for (;; i++) {
      unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
      int da = tolower(ca), db = tolower(cb);
      if (da != db) {
        r = da < db ? -1 : 1;
        break;
      }
      if (!ca || !cb) {
        r = 0;
        break;
      }
    }
  }
  vl_push_int(S, (int64_t)(r < 0 ? -1 : (r > 0 ? 1 : 0)));
  return 1;
}

static int vm_str_hash32(VL_State *S) {
  const char *s = st_check_str(S, 1);
  vl_push_int(S, (int64_t)(uint32_t)fnv1a32((const uint8_t *)s, strlen(s)));
  return 1;
}

// hex / unhex
static int vm_str_hex(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s);
  if (n > 32 * 1024 * 1024) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  size_t outn = n * 2;
  char *out = (char *)malloc(outn + 1);
  if (!out) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  static const char *H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    out[2 * i] = H[c >> 4];
    out[2 * i + 1] = H[c & 15];
  }
  out[outn] = 0;
  vl_push_string(S, out);
  free(out);
  return 1;
}
static int vm_str_unhex(VL_State *S) {
  const char *h = st_check_str(S, 1);
  size_t n = strlen(h);
  if (n % 2) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  size_t outn = n / 2;
  char *out = (char *)malloc(outn + 1);
  if (!out) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  for (size_t i = 0; i < outn; i++) {
    int c1 = h[2 * i], c2 = h[2 * i + 1];
    int v1 = (c1 >= '0' && c1 <= '9')   ? c1 - '0'
             : (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10
             : (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10
                                        : -1;
    int v2 = (c2 >= '0' && c2 <= '9')   ? c2 - '0'
             : (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10
             : (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10
                                        : -1;
    if (v1 < 0 || v2 < 0) {
      free(out);
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    out[i] = (char)((v1 << 4) | v2);
  }
  vl_push_lstring(S, out, (int)outn);
  free(out);
  return 1;
}

// base64
static int vm_str_b64_enc(VL_State *S) {
  const char *s = st_check_str(S, 1);
  size_t n = strlen(s);
  size_t outn = ((n + 2) / 3) * 4;
  char *o = (char *)malloc(outn + 1);
  if (!o) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  size_t i = 0, w = 0;
  while (i < n) {
    uint32_t v = 0;
    int bytes = 0;
    for (int k = 0; k < 3; k++) {
      v <<= 8;
      if (i < n) {
        v |= (unsigned char)s[i++];
        bytes++;
      }
    }
    int pad = 3 - bytes;
    o[w++] = B64TAB[(v >> 18) & 63];
    o[w++] = B64TAB[(v >> 12) & 63];
    o[w++] = pad >= 2 ? '=' : B64TAB[(v >> 6) & 63];
    o[w++] = pad >= 1 ? '=' : B64TAB[v & 63];
  }
  o[w] = 0;
  vl_push_string(S, o);
  free(o);
  return 1;
}
static int vm_str_b64_dec(VL_State *S) {
  const char *b = st_check_str(S, 1);
  size_t n = strlen(b);
  if (n % 4) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  char *o = (char *)malloc((n / 4) * 3 + 1);
  if (!o) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  size_t w = 0;
  for (size_t i = 0; i < n; i += 4) {
    int v0 = b64_val(b[i]), v1 = b64_val(b[i + 1]);
    int v2 = b[i + 2] == '=' ? -2 : b64_val(b[i + 2]);
    int v3 = b[i + 3] == '=' ? -2 : b64_val(b[i + 3]);
    if (v0 < 0 || v1 < 0 || v2 < -2 || v3 < -2) {
      free(o);
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    uint32_t v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
                 ((uint32_t)((v2 < 0) ? 0 : v2) << 6) |
                 (uint32_t)((v3 < 0) ? 0 : v3);
    o[w++] = (char)((v >> 16) & 0xFF);
    if (v2 >= 0) o[w++] = (char)((v >> 8) & 0xFF);
    if (v3 >= 0) o[w++] = (char)(v & 0xFF);
  }
  vl_push_lstring(S, o, (int)w);
  free(o);
  return 1;
}

// UTF-8
static int vm_utf8_len(VL_State *S) {
  const unsigned char *s = (const unsigned char *)st_check_str(S, 1);
  size_t n = strlen((const char *)s);
  size_t i = 0, cnt = 0;
  while (i < n) {
    size_t c = utf8_next(s + i, n - i, NULL);
    if (c == 0) {  // invalid, treat as single byte
      i++;
      cnt++;
      continue;
    }
    i += c;
    cnt++;
  }
  vl_push_int(S, (int64_t)cnt);
  return 1;
}

static int vm_utf8_sub(VL_State *S) {
  const unsigned char *s = (const unsigned char *)st_check_str(S, 1);
  size_t n = strlen((const char *)s);
  int have_j = (vl_get(S, 3) != NULL);
  int64_t I = st_check_int(S, 2);
  int64_t J = have_j ? st_check_int(S, 3) : (int64_t)1e9;

  // map codepoint indexes to byte offsets
  // 1-based; negatives from end
  // First, collect cp starts
  size_t cps = 0;
  // Count total cps and maybe record positions lazily
  // For efficiency, do two passes:
  size_t i = 0;
  while (i < n) {
    size_t c = utf8_next(s + i, n - i, NULL);
    if (!c) c = 1;
    i += c;
    cps++;
  }
  if (cps == 0) {
    vl_push_string(S, "");
    return 1;
  }

  auto size_t cp_to_abs = [&](int64_t k) -> size_t {
    if (k < 0) k = (int64_t)cps + k + 1;
    if (k < 1) k = 1;
    if ((uint64_t)k > cps) k = (int64_t)cps;
    return (size_t)k;
  };
  size_t a_cp = cp_to_abs(I);
  size_t b_cp = cp_to_abs(have_j ? J : (int64_t)cps);
  if (b_cp < a_cp) {
    vl_push_string(S, "");
    return 1;
  }

  // Walk again to find byte offsets
  size_t a_byte = 0, b_byte = 0;
  size_t cp = 1;
  i = 0;
  while (i < n && cp < a_cp) {
    size_t c = utf8_next(s + i, n - i, NULL);
    if (!c) c = 1;
    i += c;
    cp++;
  }
  a_byte = i;
  while (i < n && cp <= b_cp) {
    size_t c = utf8_next(s + i, n - i, NULL);
    if (!c) c = 1;
    i += c;
    cp++;
  }
  b_byte = i;
  vl_push_lstring(S, (const char *)s + a_byte, (int)(b_byte - a_byte));
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg strlib[] = {{"len", vm_str_len},
                                {"byte_at", vm_str_byte_at},
                                {"sub", vm_str_sub},
                                {"find", vm_str_find},
                                {"replace", vm_str_replace},
                                {"split", vm_str_split},

                                {"lower", vm_str_lower},
                                {"upper", vm_str_upper},
                                {"trim", vm_str_trim},
                                {"ltrim", vm_str_ltrim},
                                {"rtrim", vm_str_rtrim},
                                {"starts_with", vm_str_starts_with},
                                {"ends_with", vm_str_ends_with},
                                {"repeat", vm_str_repeat},
                                {"reverse", vm_str_reverse},
                                {"pad_left", vm_str_pad_left},
                                {"pad_right", vm_str_pad_right},
                                {"cmp", vm_str_cmp},
                                {"hash32", vm_str_hash32},

                                {"hex", vm_str_hex},
                                {"unhex", vm_str_unhex},
                                {"base64_encode", vm_str_b64_enc},
                                {"base64_decode", vm_str_b64_dec},

                                {"utf8_len", vm_utf8_len},
                                {"utf8_sub", vm_utf8_sub},

                                {NULL, NULL}};

void vl_open_strlib(VL_State *S) { vl_register_lib(S, "str", strlib); }
