/* ============================================================================
   string.c — Bibliothèque chaînes Vitte/Vitl (C17, UTF-8, portable)
   - vt_str       : chaîne dynamique (small API, printf-like, replace, trim…)
   - vt_sv        : string view (pas d’allocation)
   - UTF-8        : validation, itération codepoints, longueur
   - Conversions  : int/float parse, hex/base64, hash
   - Patterns     : find/rfind, KMP, wildcard *, ?, split/join
   - Paths        : normalisation, basename, dirname, join
   - Sécurité     : aucune UB sur tailles. OOM → abort contrôlé.
   Licence: MIT.
   ============================================================================
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#if defined(_WIN32) && !defined(_SSIZE_T_) && !defined(_SSIZE_T_DEFINED)
typedef ptrdiff_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* ----------------------------------------------------------------------------
   Integrations optionnelles
---------------------------------------------------------------------------- */
#ifdef __has_include
#if __has_include("debug.h")
#include "debug.h"
#endif
#if __has_include("stringex.h")
#include "stringex.h" /* S’il existe déjà un header projet */
#endif
#if __has_include("vt_string.h")
#include "vt_string.h"
#endif
#endif

/* ----------------------------------------------------------------------------
   Fallback d’API publique si aucun header n’est présent
---------------------------------------------------------------------------- */
#ifndef VT_STRING_H
#define VT_STRING_H_SENTINEL 1

#ifndef VT_STRING_API
#define VT_STRING_API extern
#endif

#if !defined(VT_PRINTF)
#  if defined(__GNUC__) || defined(__clang__)
#    define VT_PRINTF(idx, vaidx) __attribute__((format(printf, idx, vaidx)))
#  else
#    define VT_PRINTF(idx, vaidx)
#  endif
#endif

typedef struct vt_sv {
  const char* data;
  size_t len;
} vt_sv;

typedef struct vt_str {
  char* data;
  size_t len;
  size_t cap;
} vt_str;

/* lifecycle */
VT_STRING_API void vt_str_init(vt_str* s);
VT_STRING_API void vt_str_with_cap(vt_str* s, size_t cap_hint);
VT_STRING_API void vt_str_free(vt_str* s);
VT_STRING_API void vt_str_clear(vt_str* s);
VT_STRING_API void vt_str_shrink(vt_str* s);

/* view utils */
VT_STRING_API vt_sv vt_sv_from_cstr(const char* z);
VT_STRING_API vt_sv vt_sv_sub(vt_sv v, size_t pos, size_t n);

/* reserve + append */
VT_STRING_API void vt_str_reserve(vt_str* s, size_t need);
VT_STRING_API void vt_str_push_char(vt_str* s, char c);
VT_STRING_API void vt_str_append(vt_str* s, const char* z);
VT_STRING_API void vt_str_append_n(vt_str* s, const char* p, size_t n);
VT_STRING_API void vt_str_append_sv(vt_str* s, vt_sv v);
VT_STRING_API void vt_str_append_fmt(vt_str* s, const char* fmt, ...)
    VT_PRINTF(2, 3);

/* edit */
VT_STRING_API void vt_str_insert(vt_str* s, size_t pos, const char* z);
VT_STRING_API void vt_str_erase(vt_str* s, size_t pos, size_t n);
VT_STRING_API size_t vt_str_replace_all(vt_str* s, vt_sv from, vt_sv to);

/* trim / case */
VT_STRING_API void vt_str_ltrim_ws(vt_str* s);
VT_STRING_API void vt_str_rtrim_ws(vt_str* s);
VT_STRING_API void vt_str_trim_ws(vt_str* s);
VT_STRING_API void vt_str_to_lower_ascii(vt_str* s);
VT_STRING_API void vt_str_to_upper_ascii(vt_str* s);

/* find */
VT_STRING_API ssize_t vt_sv_find(vt_sv hay, vt_sv nee); /* KMP */
VT_STRING_API ssize_t vt_sv_rfind(vt_sv hay,
                                  vt_sv nee); /* reverse scan (naif) */
VT_STRING_API bool vt_sv_starts_with(vt_sv s, vt_sv pre);
VT_STRING_API bool vt_sv_ends_with(vt_sv s, vt_sv suf);
VT_STRING_API int vt_sv_cmp(vt_sv a, vt_sv b);
VT_STRING_API int vt_sv_casecmp_ascii(vt_sv a, vt_sv b);

/* split/join */
typedef struct vt_sv_vec {
  vt_sv* v;
  size_t len, cap;
} vt_sv_vec;
VT_STRING_API void vt_sv_vec_free(vt_sv_vec* out);
VT_STRING_API void vt_sv_split_char(vt_sv s, char sep, vt_sv_vec* out);
VT_STRING_API void vt_sv_split_sv(vt_sv s, vt_sv sep, vt_sv_vec* out);
VT_STRING_API void vt_str_join_sv(vt_str* dst, vt_sv glue, const vt_sv* parts,
                                  size_t n);

/* wildcard */
VT_STRING_API bool vt_wildcard_match_ascii(vt_sv text, vt_sv pat); /* * ? */

/* utf-8 */
VT_STRING_API bool vt_utf8_valid(vt_sv s);
VT_STRING_API size_t vt_utf8_count(vt_sv s); /* nb codepoints */
VT_STRING_API int32_t vt_utf8_next(const char* p, size_t n,
                                   size_t* adv); /* -1 si invalide */

/* conv */
VT_STRING_API bool vt_parse_i64(vt_sv s, int base, int64_t* out);
VT_STRING_API bool vt_parse_u64(vt_sv s, int base, uint64_t* out);
VT_STRING_API bool vt_parse_f64(vt_sv s, double* out);

/* hex/base64 */
VT_STRING_API void vt_hex_encode(vt_str* out, const void* data, size_t n,
                                 bool upper);
VT_STRING_API bool vt_hex_decode(vt_str* out_bin, vt_sv hex);
VT_STRING_API void vt_base64_encode(vt_str* out, const void* data, size_t n);
VT_STRING_API bool vt_base64_decode(vt_str* out_bin, vt_sv b64);

/* hash */
VT_STRING_API uint64_t vt_hash_fnv1a64(const void* p, size_t n);

/* path helpers */
VT_STRING_API void vt_path_normalize_slashes(vt_str* s);
VT_STRING_API vt_sv vt_path_basename(vt_sv s);
VT_STRING_API vt_sv vt_path_dirname(vt_sv s);
VT_STRING_API void vt_path_join(vt_str* out, vt_sv a, vt_sv b);

#endif /* VT_STRING_H */

/* ----------------------------------------------------------------------------
   Logs fallback
---------------------------------------------------------------------------- */
#ifndef VT_DEBUG_H
#define VT_TRACE(...) (void)0
#define VT_DEBUG(...) (void)0
#define VT_INFO(...) (void)0
#define VT_WARN(...) (void)0
#define VT_ERROR(...) (void)0
#define VT_FATAL(...)                           \
  do {                                          \
    fprintf(stderr, "[FATAL] string: abort\n"); \
    abort();                                    \
  } while (0)
#endif

/* ----------------------------------------------------------------------------
   Utils internes
---------------------------------------------------------------------------- */
static void* xmalloc(size_t n) {
  void* p = malloc(n);
  if (!p) {
    VT_FATAL("OOM");
  }
  return p;
}
static void* xrealloc(void* q, size_t n) {
  void* p = realloc(q, n);
  if (!p) {
    VT_FATAL("OOM");
  }
  return p;
}
static char* xstrndup(const char* s, size_t n) {
  char* d = (char*)xmalloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

static size_t round_cap(size_t want) {
  size_t cap = 16;
  while (cap < want) cap <<= 1;
  return cap;
}
static inline size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }
static inline size_t max_sz(size_t a, size_t b) { return a > b ? a : b; }

/* ----------------------------------------------------------------------------
   vt_str — lifecycle
---------------------------------------------------------------------------- */
void vt_str_init(vt_str* s) {
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
}
void vt_str_with_cap(vt_str* s, size_t cap_hint) {
  s->len = 0;
  s->cap = cap_hint ? round_cap(cap_hint) : 16;
  s->data = (char*)xmalloc(s->cap);
  s->data[0] = 0;
}
void vt_str_free(vt_str* s) {
  if (!s) return;
  free(s->data);
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
}
void vt_str_clear(vt_str* s) {
  if (s && s->data) {
    s->len = 0;
    s->data[0] = 0;
  }
}
void vt_str_shrink(vt_str* s) {
  if (!s || !s->data) return;
  size_t need = s->len + 1;
  if (need < s->cap) {
    s->data = (char*)xrealloc(s->data, need);
    s->cap = need;
  }
}

/* ----------------------------------------------------------------------------
   vt_sv
---------------------------------------------------------------------------- */
vt_sv vt_sv_from_cstr(const char* z) {
  vt_sv v;
  v.data = z ? z : "";
  v.len = z ? strlen(z) : 0;
  return v;
}
vt_sv vt_sv_sub(vt_sv v, size_t pos, size_t n) {
  if (pos > v.len) pos = v.len;
  size_t take = min_sz(n, v.len - pos);
  vt_sv out = {v.data + pos, take};
  return out;
}

/* ----------------------------------------------------------------------------
   reserve/append
---------------------------------------------------------------------------- */
void vt_str_reserve(vt_str* s, size_t need) {
  size_t want = need + 1;
  if (want <= s->cap) return;
  size_t ncap = round_cap(want);
  s->data = (char*)xrealloc(s->data, ncap);
  s->cap = ncap;
}
void vt_str_push_char(vt_str* s, char c) {
  if (!s->data) vt_str_with_cap(s, 16);
  vt_str_reserve(s, s->len + 1);
  s->data[s->len++] = c;
  s->data[s->len] = 0;
}
void vt_str_append_n(vt_str* s, const char* p, size_t n) {
  if (n == 0) return;
  if (!s->data) vt_str_with_cap(s, n + 16);
  vt_str_reserve(s, s->len + n);
  memcpy(s->data + s->len, p, n);
  s->len += n;
  s->data[s->len] = 0;
}
void vt_str_append(vt_str* s, const char* z) {
  if (z) vt_str_append_n(s, z, strlen(z));
}
void vt_str_append_sv(vt_str* s, vt_sv v) { vt_str_append_n(s, v.data, v.len); }

void vt_str_insert(vt_str* s, size_t pos, const char* z) {
  if (!z) return;
  size_t zl = strlen(z);
  if (!s->data) {
    vt_str_with_cap(s, zl + 16);
    pos = 0;
  }
  if (pos > s->len) pos = s->len;
  vt_str_reserve(s, s->len + zl);
  memmove(s->data + pos + zl, s->data + pos, s->len - pos);
  memcpy(s->data + pos, z, zl);
  s->len += zl;
  s->data[s->len] = 0;
}
void vt_str_erase(vt_str* s, size_t pos, size_t n) {
  if (pos >= s->len || n == 0) return;
  size_t end = min_sz(s->len, pos + n);
  size_t tail = s->len - end;
  memmove(s->data + pos, s->data + end, tail);
  s->len = pos + tail;
  s->data[s->len] = 0;
}

/* ----------------------------------------------------------------------------
   printf-like
---------------------------------------------------------------------------- */
void vt_str_append_vfmt(vt_str* s, const char* fmt, va_list ap) {
  if (!fmt) return;
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (need < 0) return;
  size_t n = (size_t)need;
  vt_str_reserve(s, s->len + n);
  vsnprintf(s->data + s->len, s->cap - s->len, fmt, ap);
  s->len += n;
}
void vt_str_append_fmt(vt_str* s, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vt_str_append_vfmt(s, fmt, ap);
  va_end(ap);
}

/* ----------------------------------------------------------------------------
   replace_all
---------------------------------------------------------------------------- */
static void kmp_build(const char* p, size_t m, int* lps) {
  size_t len = 0, i = 1;
  lps[0] = 0;
  while (i < m) {
    if (p[i] == p[len]) {
      lps[i++] = (int)++len;
    } else if (len) {
      len = (size_t)lps[len - 1];
    } else {
      lps[i++] = 0;
    }
  }
}
size_t vt_str_replace_all(vt_str* s, vt_sv from, vt_sv to) {
  if (from.len == 0 || s->len == 0) return 0;
  /* 1) compter matches */
  vt_sv hay = {s->data, s->len};
  ssize_t at = 0;
  size_t cnt = 0;
  while ((at = (ssize_t)vt_sv_find(hay, from)) >= 0) {
    cnt++; /* avancer */
    size_t next = (size_t)at + from.len;
    hay.data += next;
    hay.len -= next;
  }
  if (!cnt) return 0;
  /* 2) allouer buffer final */
  size_t final = s->len + cnt * (to.len - from.len);
  char* out = (char*)xmalloc(final + 1);
  size_t oi = 0;
  /* 3) remplacer */
  const char* h = s->data;
  size_t hl = s->len;
  while (hl) {
    vt_sv H = {h, hl};
    ssize_t i = (ssize_t)vt_sv_find(H, from);
    if (i < 0) {
      memcpy(out + oi, h, hl);
      oi += hl;
      break;
    }
    size_t isz = (size_t)i;
    memcpy(out + oi, h, isz);
    oi += isz;
    memcpy(out + oi, to.data, to.len);
    oi += to.len;
    h += isz + from.len;
    hl -= isz + from.len;
  }
  out[oi] = 0;
  free(s->data);
  s->data = out;
  s->len = oi;
  s->cap = final + 1;
  return cnt;
}

/* ----------------------------------------------------------------------------
   trim / case
---------------------------------------------------------------------------- */
static inline int isws(int c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
         c == '\f';
}
void vt_str_ltrim_ws(vt_str* s) {
  size_t i = 0;
  while (i < s->len && isws((unsigned char)s->data[i])) i++;
  if (i) vt_str_erase(s, 0, i);
}
void vt_str_rtrim_ws(vt_str* s) {
  if (s->len == 0) return;
  size_t i = s->len;
  while (i > 0 && isws((unsigned char)s->data[i - 1])) i--;
  if (i < s->len) {
    s->len = i;
    s->data[i] = 0;
  }
}
void vt_str_trim_ws(vt_str* s) {
  vt_str_rtrim_ws(s);
  vt_str_ltrim_ws(s);
}
void vt_str_to_lower_ascii(vt_str* s) {
  for (size_t i = 0; i < s->len; i++) {
    unsigned char c = s->data[i];
    if (c >= 'A' && c <= 'Z') s->data[i] = (char)(c + ('a' - 'A'));
  }
}
void vt_str_to_upper_ascii(vt_str* s) {
  for (size_t i = 0; i < s->len; i++) {
    unsigned char c = s->data[i];
    if (c >= 'a' && c <= 'z') s->data[i] = (char)(c - ('a' - 'A'));
  }
}

/* ----------------------------------------------------------------------------
   find / KMP
---------------------------------------------------------------------------- */
ssize_t vt_sv_find(vt_sv hay, vt_sv nee) {
  if (nee.len == 0) return 0;
  if (nee.len > hay.len) return -1;
  int* lps = (int*)xmalloc(sizeof(int) * nee.len);
  kmp_build(nee.data, nee.len, lps);
  size_t i = 0, j = 0;
  while (i < hay.len) {
    if (hay.data[i] == nee.data[j]) {
      i++;
      j++;
      if (j == nee.len) {
        free(lps);
        return (ssize_t)(i - j);
      }
    } else if (j)
      j = (size_t)lps[j - 1];
    else
      i++;
  }
  free(lps);
  return -1;
}
ssize_t vt_sv_rfind(vt_sv hay, vt_sv nee) {
  if (nee.len == 0) return (ssize_t)hay.len;
  if (nee.len > hay.len) return -1;
  for (size_t pos = hay.len - nee.len + 1; pos-- > 0;) {
    if (memcmp(hay.data + pos, nee.data, nee.len) == 0)
      return (ssize_t)pos;
    if (pos == 0) break;
  }
  return -1;
}
bool vt_sv_starts_with(vt_sv s, vt_sv pre) {
  return s.len >= pre.len && memcmp(s.data, pre.data, pre.len) == 0;
}
bool vt_sv_ends_with(vt_sv s, vt_sv suf) {
  return s.len >= suf.len &&
         memcmp(s.data + (s.len - suf.len), suf.data, suf.len) == 0;
}
int vt_sv_cmp(vt_sv a, vt_sv b) {
  size_t n = min_sz(a.len, b.len);
  int c = memcmp(a.data, b.data, n);
  if (c) return c;
  return (a.len < b.len) ? -1 : (a.len > b.len);
}
int vt_sv_casecmp_ascii(vt_sv a, vt_sv b) {
  size_t n = min_sz(a.len, b.len);
  for (size_t i = 0; i < n; i++) {
    unsigned char x = a.data[i], y = b.data[i];
    if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + ('a' - 'A'));
    if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + ('a' - 'A'));
    if (x != y) return (int)x - (int)y;
  }
  return (a.len < b.len) ? -1 : (a.len > b.len);
}

/* ----------------------------------------------------------------------------
   split/join
---------------------------------------------------------------------------- */
static void sv_vec_push(vt_sv_vec* v, vt_sv x) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    v->v = (vt_sv*)xrealloc(v->v, nc * sizeof(vt_sv));
    v->cap = nc;
  }
  v->v[v->len++] = x;
}
void vt_sv_vec_free(vt_sv_vec* out) {
  free(out->v);
  out->v = NULL;
  out->len = out->cap = 0;
}

void vt_sv_split_char(vt_sv s, char sep, vt_sv_vec* out) {
  size_t i = 0, start = 0;
  while (i <= s.len) {
    if (i == s.len || s.data[i] == sep) {
      vt_sv piece = {s.data + start, i - start};
      sv_vec_push(out, piece);
      start = i + 1;
    }
    i++;
  }
}
void vt_sv_split_sv(vt_sv s, vt_sv sep, vt_sv_vec* out) {
  if (sep.len == 0) {
    sv_vec_push(out, s);
    return;
  }
  size_t i = 0;
  while (i <= s.len) {
    vt_sv rest = {s.data + i, s.len - i};
    ssize_t k = vt_sv_find(rest, sep);
    if (k < 0) {
      sv_vec_push(out, rest);
      break;
    }
    size_t pos = i + (size_t)k;
    vt_sv piece = {s.data + i, pos - i};
    sv_vec_push(out, piece);
    i = pos + sep.len;
  }
}
void vt_str_join_sv(vt_str* dst, vt_sv glue, const vt_sv* parts, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (i) vt_str_append_sv(dst, glue);
    vt_str_append_sv(dst, parts[i]);
  }
}

/* ----------------------------------------------------------------------------
   wildcard *, ?  (ASCII, sans classes ni escapes)
---------------------------------------------------------------------------- */
static bool wild_match_rec(const char* t, size_t tn, const char* p, size_t pn) {
  while (pn) {
    char pc = *p;
    if (pc == '*') {
      /* consomme '*' groupé */
      while (pn && *p == '*') {
        p++;
        pn--;
      }
      if (!pn) return true; /* étoile finale */
      for (size_t i = 0; i < tn; i++) {
        if (wild_match_rec(t + i, tn - i, p, pn)) return true;
      }
      return false;
    } else if (pc == '?') {
      if (!tn) return false;
      t++;
      tn--;
      p++;
      pn--;
    } else {
      if (!tn || *t != pc) return false;
      t++;
      tn--;
      p++;
      pn--;
    }
  }
  return tn == 0;
}
bool vt_wildcard_match_ascii(vt_sv text, vt_sv pat) {
  return wild_match_rec(text.data, text.len, pat.data, pat.len);
}

/* ----------------------------------------------------------------------------
   UTF-8
---------------------------------------------------------------------------- */
static int utf8_len(unsigned char lead) {
  if (lead < 0x80)
    return 1;
  else if ((lead >> 5) == 0x6)
    return 2;
  else if ((lead >> 4) == 0xE)
    return 3;
  else if ((lead >> 3) == 0x1E)
    return 4;
  return -1;
}
int32_t vt_utf8_next(const char* p, size_t n, size_t* adv) {
  if (n == 0) {
    if (adv) *adv = 0;
    return -1;
  }
  unsigned char c = (unsigned char)p[0];
  int k = utf8_len(c);
  if (k < 0 || (size_t)k > n) {
    if (adv) *adv = 1;
    return -1;
  }
  if (k == 1) {
    if (adv) *adv = 1;
    return (int32_t)c;
  }
  int32_t cp = c & (0x7F >> k);
  for (int i = 1; i < k; i++) {
    unsigned char cc = (unsigned char)p[i];
    if ((cc >> 6) != 0x2) {
      if (adv) *adv = i;
      return -1;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  if (adv) *adv = (size_t)k;
  return cp;
}
bool vt_utf8_valid(vt_sv s) {
  size_t i = 0;
  while (i < s.len) {
    size_t adv = 0;
    int32_t r = vt_utf8_next(s.data + i, s.len - i, &adv);
    if (r < 0) return false;
    i += adv;
  }
  return true;
}
size_t vt_utf8_count(vt_sv s) {
  size_t i = 0, cnt = 0;
  while (i < s.len) {
    size_t adv = 0;
    int32_t r = vt_utf8_next(s.data + i, s.len - i, &adv);
    if (r < 0) return (size_t)-1;
    i += adv;
    cnt++;
  }
  return cnt;
}

/* ----------------------------------------------------------------------------
   Conversions
---------------------------------------------------------------------------- */
static int digit_of(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return -1;
}
bool vt_parse_i64(vt_sv s, int base, int64_t* out) {
  if (!s.len) return false;
  const char* p = s.data;
  size_t n = s.len;
  int sign = 1;
  if (*p == '+' || *p == '-') {
    if (*p == '-') sign = -1;
    p++;
    n--;
    if (n == 0) return false;
  }
  if ((base == 0 || base == 16) && n >= 2 && p[0] == '0' &&
      (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
    n -= 2;
  } else if ((base == 0 || base == 2) && n >= 2 && p[0] == '0' &&
             (p[1] == 'b' || p[1] == 'B')) {
    base = 2;
    p += 2;
    n -= 2;
  } else if (base == 0) {
    base = 10;
  }
  if (base < 2 || base > 36) return false;

  int64_t val = 0;
  for (size_t i = 0; i < n; i++) {
    int d = digit_of((unsigned char)p[i]);
    if (d < 0 || d >= base) return false;
    if (val > (INT64_MAX - d) / base) return false;
    val = val * base + d;
  }
  *out = sign > 0 ? val : -val;
  return true;
}
bool vt_parse_u64(vt_sv s, int base, uint64_t* out) {
  if (!s.len) return false;
  const char* p = s.data;
  size_t n = s.len;
  if (*p == '+') {
    p++;
    n--;
    if (n == 0) return false;
  }
  if ((base == 0 || base == 16) && n >= 2 && p[0] == '0' &&
      (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
    n -= 2;
  } else if ((base == 0 || base == 2) && n >= 2 && p[0] == '0' &&
             (p[1] == 'b' || p[1] == 'B')) {
    base = 2;
    p += 2;
    n -= 2;
  } else if (base == 0) {
    base = 10;
  }
  if (base < 2 || base > 36) return false;

  uint64_t val = 0;
  for (size_t i = 0; i < n; i++) {
    int d = digit_of((unsigned char)p[i]);
    if (d < 0 || d >= base) return false;
    if (val > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return false;
    val = val * (uint64_t)base + (uint64_t)d;
  }
  *out = val;
  return true;
}
bool vt_parse_f64(vt_sv s, double* out) {
  /* tolère \0 sentinel via strtod sur copie */
  char* tmp = xstrndup(s.data, s.len);
  char* endp = NULL;
  errno = 0;
  double v = strtod(tmp, &endp);
  bool ok = (endp && *endp == 0 && errno == 0);
  if (ok && out) *out = v;
  free(tmp);
  return ok;
}

/* ----------------------------------------------------------------------------
   Hex / Base64
---------------------------------------------------------------------------- */
void vt_hex_encode(vt_str* out, const void* data, size_t n, bool upper) {
  static const char* HEX = "0123456789abcdef";
  static const char* HEXU = "0123456789ABCDEF";
  const char* H = upper ? HEXU : HEX;
  const unsigned char* p = (const unsigned char*)data;
  vt_str_reserve(out, out->len + n * 2);
  for (size_t i = 0; i < n; i++) {
    unsigned b = p[i];
    vt_str_push_char(out, H[b >> 4]);
    vt_str_push_char(out, H[b & 15]);
  }
}
static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}
bool vt_hex_decode(vt_str* out_bin, vt_sv hex) {
  if (hex.len % 2) return false;
  size_t bytes = hex.len / 2;
  vt_str_reserve(out_bin, out_bin->len + bytes);
  for (size_t i = 0; i < bytes; i++) {
    int hi = hexval((unsigned char)hex.data[2 * i]);
    int lo = hexval((unsigned char)hex.data[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    char b = (char)((hi << 4) | lo);
    vt_str_push_char(out_bin, b);
  }
  return true;
}
/* Base64 (RFC4648, sans padding optionnel obligatoire en decode) */
void vt_base64_encode(vt_str* out, const void* data, size_t n) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char* p = (const unsigned char*)data;
  size_t i = 0;
  while (i + 3 <= n) {
    unsigned a = p[i++], b = p[i++], c = p[i++];
    vt_str_push_char(out, T[a >> 2]);
    vt_str_push_char(out, T[((a & 3) << 4) | (b >> 4)]);
    vt_str_push_char(out, T[((b & 15) << 2) | (c >> 6)]);
    vt_str_push_char(out, T[c & 63]);
  }
  size_t rem = n - i;
  if (rem == 1) {
    unsigned a = p[i];
    vt_str_push_char(out, T[a >> 2]);
    vt_str_push_char(out, T[(a & 3) << 4]);
    vt_str_push_char(out, '=');
    vt_str_push_char(out, '=');
  } else if (rem == 2) {
    unsigned a = p[i], b = p[i + 1];
    vt_str_push_char(out, T[a >> 2]);
    vt_str_push_char(out, T[((a & 3) << 4) | (b >> 4)]);
    vt_str_push_char(out, T[(b & 15) << 2]);
    vt_str_push_char(out, '=');
  }
}
static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return -2; /* pad */
  return -1;
}
bool vt_base64_decode(vt_str* out_bin, vt_sv b64) {
  size_t i = 0;
  int quad[4];
  int qi = 0;
  while (i < b64.len) {
    int v = b64v((unsigned char)b64.data[i++]);
    if (v == -1) continue; /* ignore whitespace/autres */
    quad[qi++] = v;
    if (qi == 4) {
      int a = quad[0], b = quad[1], c = quad[2], d = quad[3];
      if (a < 0 || b < 0) return false;
      unsigned x = (unsigned)((a << 18) | (b << 12) | ((c < 0 ? 0 : c) << 6) |
                              (d < 0 ? 0 : d));
      vt_str_push_char(out_bin, (char)((x >> 16) & 0xFF));
      if (c != -2) vt_str_push_char(out_bin, (char)((x >> 8) & 0xFF));
      if (d != -2) vt_str_push_char(out_bin, (char)(x & 0xFF));
      qi = 0;
    }
  }
  return qi == 0;
}

/* ----------------------------------------------------------------------------
   Hash
---------------------------------------------------------------------------- */
uint64_t vt_hash_fnv1a64(const void* p, size_t n) {
  const unsigned char* s = (const unsigned char*)p;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= s[i];
    h *= 1099511628211ull;
  }
  return h ? h : 0x9e3779b97f4a7c15ull;
}

/* ----------------------------------------------------------------------------
   Path helpers
---------------------------------------------------------------------------- */
void vt_path_normalize_slashes(vt_str* s) {
  if (!s->data) return;
  for (size_t i = 0; i < s->len; i++)
    if (s->data[i] == '\\') s->data[i] = '/';
  /* compacte // */
  size_t w = 0;
  for (size_t r = 0; r < s->len; r++) {
    if (r > 0 && s->data[r] == '/' && s->data[w - 1] == '/') continue;
    s->data[w++] = s->data[r];
  }
  s->len = w;
  s->data[w] = 0;
}
vt_sv vt_path_basename(vt_sv s) {
  size_t i = s.len;
  while (i > 0) {
    char c = s.data[i - 1];
    if (c == '/' || c == '\\') break;
    i--;
  }
  return (vt_sv){s.data + i, s.len - i};
}
vt_sv vt_path_dirname(vt_sv s) {
  size_t i = s.len;
  while (i > 0) {
    char c = s.data[i - 1];
    if (c == '/' || c == '\\') break;
    i--;
  }
  if (i == 0) return (vt_sv){s.data, 0};
  /* retire slash final */
  size_t j = i;
  while (j > 0 && (s.data[j - 1] == '/' || s.data[j - 1] == '\\')) j--;
  return (vt_sv){s.data, j};
}
void vt_path_join(vt_str* out, vt_sv a, vt_sv b) {
  vt_str_clear(out);
  vt_str_append_sv(out, a);
  if (out->len && out->data[out->len - 1] != '/') vt_str_push_char(out, '/');
  vt_str_append_sv(out, b);
  vt_path_normalize_slashes(out);
}

/* ----------------------------------------------------------------------------
   Fin
---------------------------------------------------------------------------- */
