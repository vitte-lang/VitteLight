// vitte-light/core/string.c
// Utilitaires chaînes pour VitteLight: concat, slice, case, trim, search,
// conversions, repeat, escape imprimable. Basé sur VL_String/VL_Value.
//
// Dépendances: api.h, object.h (VL_String), mem.h (VL_Buffer).
// Implémentation pure ASCII pour les opérations case/trim; UTF‑8 conservé
// octet à octet sans réécriture.
//
// Fonctions exposées (si vous voulez un header, demandez `string.h`):
//   VL_Value  vl_str_concat(struct VL_Context *ctx, VL_Value a, VL_Value b);
//   VL_Value  vl_str_slice_v(struct VL_Context *ctx, VL_Value s, size_t pos,
//   size_t len); VL_Value  vl_str_lower_v(struct VL_Context *ctx, VL_Value s);
//   VL_Value  vl_str_upper_v(struct VL_Context *ctx, VL_Value s);
//   VL_Value  vl_str_trim_v (struct VL_Context *ctx, VL_Value s);
//   int       vl_str_to_int64 (const VL_String *s, int64_t *out);
//   int       vl_str_to_double(const VL_String *s, double *out);
//   ssize_t   vl_str_find_cstr(const VL_String *s, const char *needle);
//   int       vl_str_starts_with_cstr(const VL_String *s, const char *prefix);
//   int       vl_str_ends_with_cstr  (const VL_String *s, const char *suffix);
//   VL_Value  vl_str_repeat_v(struct VL_Context *ctx, const VL_String *s,
//   size_t times); size_t    vl_str_write_escaped(const VL_String *s, FILE
//   *out);
//   // Helpers bas-niveau:
//   uint32_t  vl_str_hash_bytes(const void *p, size_t n); // FNV-1a
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/string.c

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

#include "api.h"
#include "mem.h"
#include "object.h"

// ───────────────────────── Hash (FNV‑1a 32‑bit) ─────────────────────────
uint32_t vl_str_hash_bytes(const void *p, size_t n) {
  const unsigned char *s = (const unsigned char *)p;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= s[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}

// ───────────────────────── Internes ─────────────────────────
static inline int vl_is_str(const VL_Value *v) {
  return v && v->type == VT_STR && v->as.s;
}
static inline uint32_t vl_slen(const VL_String *s) { return s ? s->len : 0u; }
static inline const char *vl_sdata(const VL_String *s) {
  return s ? s->data : "";
}

static VL_Value vl_make_strn_hash(struct VL_Context *ctx, const char *p,
                                  size_t n, uint32_t h) {
  // Utilise la fabrique canonique puis ajuste le hash si pas rempli.
  VL_Value v = vl_make_strn(ctx, p, n);
  if (v.type == VT_STR && v.as.s) {
    if (v.as.s->hash == 0) v.as.s->hash = h ? h : vl_str_hash_bytes(p, n);
  }
  return v;
}

// ───────────────────────── Concat ─────────────────────────
VL_Value vl_str_concat(struct VL_Context *ctx, VL_Value a, VL_Value b) {
  if (!vl_is_str(&a) || !vl_is_str(&b)) return vlv_nil();
  const VL_String *sa = a.as.s, *sb = b.as.s;
  size_t na = sa->len, nb = sb->len;
  if (na + nb > (size_t)UINT32_MAX) return vlv_nil();
  VL_Buffer buf;
  vl_buf_init(&buf);
  if (!vl_buf_reserve(&buf, na + nb + 1)) {
    vl_buf_free(&buf);
    return vlv_nil();
  }
  vl_buf_append(&buf, sa->data, na);
  vl_buf_append(&buf, sb->data, nb);
  char *s = vl_buf_take_cstr(&buf);
  if (!s) {
    vl_buf_free(&buf);
    return vlv_nil();
  }
  uint32_t h = vl_str_hash_bytes(s, na + nb);
  VL_Value out = vl_make_strn_hash(ctx, s, na + nb, h);
  free(s);
  return out;
}

// ───────────────────────── Slice ─────────────────────────
VL_Value vl_str_slice_v(struct VL_Context *ctx, VL_Value s, size_t pos,
                        size_t len) {
  if (!vl_is_str(&s)) return vlv_nil();
  const VL_String *ss = s.as.s;
  size_t n = ss->len;
  if (pos > n) pos = n;
  if (pos + len > n) len = n - pos;  // clamp
  return vl_make_strn_hash(ctx, ss->data + pos, len,
                           vl_str_hash_bytes(ss->data + pos, len));
}

// ───────────────────────── Case transform (ASCII) ─────────────────────────
static inline unsigned char to_low(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}
static inline unsigned char to_up(unsigned char c) {
  return (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c;
}

static VL_Value vl__case_map(struct VL_Context *ctx, const VL_String *s,
                             int to_upper) {
  size_t n = s->len;
  VL_Buffer b;
  vl_buf_init(&b);
  if (!vl_buf_reserve(&b, n + 1)) {
    vl_buf_free(&b);
    return vlv_nil();
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s->data[i];
    c = to_upper ? to_up(c) : to_low(c);
    vl_buf_putc(&b, (int)c);
  }
  char *p = vl_buf_take_cstr(&b);
  if (!p) {
    vl_buf_free(&b);
    return vlv_nil();
  }
  VL_Value out = vl_make_strn_hash(ctx, p, n, 0);
  free(p);
  return out;
}

VL_Value vl_str_lower_v(struct VL_Context *ctx, VL_Value s) {
  if (!vl_is_str(&s)) return vlv_nil();
  return vl__case_map(ctx, s.as.s, 0);
}
VL_Value vl_str_upper_v(struct VL_Context *ctx, VL_Value s) {
  if (!vl_is_str(&s)) return vlv_nil();
  return vl__case_map(ctx, s.as.s, 1);
}

// ───────────────────────── Trim (ASCII whitespace) ─────────────────────────
static int is_ws(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}
VL_Value vl_str_trim_v(struct VL_Context *ctx, VL_Value s) {
  if (!vl_is_str(&s)) return vlv_nil();
  const char *p = s.as.s->data;
  size_t n = s.as.s->len;
  size_t i = 0, j = n;
  while (i < j && is_ws((unsigned char)p[i])) i++;
  while (j > i && is_ws((unsigned char)p[j - 1])) j--;
  return vl_make_strn_hash(ctx, p + i, j - i, 0);
}

// ───────────────────────── Conversions ─────────────────────────
int vl_str_to_int64(const VL_String *s, int64_t *out) {
  if (!s || !out) return 0;
  char buf[64];
  size_t n = s->len < sizeof(buf) - 1 ? s->len : sizeof(buf) - 1;
  memcpy(buf, s->data, n);
  buf[n] = '\0';
  char *end = NULL;
  long long v = strtoll(buf, &end, 0);
  if (end == buf) return 0;
  *out = (int64_t)v;
  return 1;
}
int vl_str_to_double(const VL_String *s, double *out) {
  if (!s || !out) return 0;
  char buf[64];
  size_t n = s->len < sizeof(buf) - 1 ? s->len : sizeof(buf) - 1;
  memcpy(buf, s->data, n);
  buf[n] = '\0';
  char *end = NULL;
  double v = strtod(buf, &end);
  if (end == buf) return 0;
  *out = v;
  return 1;
}

// ───────────────────────── Recherche simple ─────────────────────────
static ssize_t memmem_naive(const char *hay, size_t hn, const char *ndl,
                            size_t nn) {
  if (nn == 0) return 0;
  if (hn < nn) return -1;
  for (size_t i = 0; i + nn <= hn; i++) {
    if (hay[i] == ndl[0] && memcmp(hay + i, ndl, nn) == 0) return (ssize_t)i;
  }
  return -1;
}

ssize_t vl_str_find_cstr(const VL_String *s, const char *needle) {
  if (!s || !needle) return -1;
  size_t nn = strlen(needle);
  return memmem_naive(s->data, s->len, needle, nn);
}
int vl_str_starts_with_cstr(const VL_String *s, const char *prefix) {
  if (!s || !prefix) return 0;
  size_t pn = strlen(prefix);
  return s->len >= pn && memcmp(s->data, prefix, pn) == 0;
}
int vl_str_ends_with_cstr(const VL_String *s, const char *suffix) {
  if (!s || !suffix) return 0;
  size_t sn = strlen(suffix);
  return s->len >= sn && memcmp(s->data + (s->len - sn), suffix, sn) == 0;
}

// ───────────────────────── Repeat ─────────────────────────
VL_Value vl_str_repeat_v(struct VL_Context *ctx, const VL_String *s,
                         size_t times) {
  if (!s) return vlv_nil();
  if (times == 0) return vl_make_strn_hash(ctx, "", 0, 0);
  if (s->len && times > (size_t)UINT32_MAX / s->len) return vlv_nil();
  size_t total = (size_t)s->len * times;
  VL_Buffer b;
  vl_buf_init(&b);
  if (!vl_buf_reserve(&b, total + 1)) {
    vl_buf_free(&b);
    return vlv_nil();
  }
  for (size_t i = 0; i < times; i++) {
    vl_buf_append(&b, s->data, s->len);
  }
  char *p = vl_buf_take_cstr(&b);
  if (!p) {
    vl_buf_free(&b);
    return vlv_nil();
  }
  VL_Value out = vl_make_strn_hash(ctx, p, total, 0);
  free(p);
  return out;
}

// ───────────────────────── Escape imprimable ─────────────────────────
size_t vl_str_write_escaped(const VL_String *s, FILE *out) {
  if (!s) {
    return 0;
  }
  if (!out) out = stdout;
  size_t n = 0;
  fputc('"', out);
  n++;
  for (uint32_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->data[i];
    switch (c) {
      case '\\':
        fputs("\\\\", out);
        n += 2;
        break;
      case '"':
        fputs("\\\"", out);
        n += 2;
        break;
      case '\n':
        fputs("\\n", out);
        n += 2;
        break;
      case '\r':
        fputs("\\r", out);
        n += 2;
        break;
      case '\t':
        fputs("\\t", out);
        n += 2;
        break;
      default:
        if (c < 0x20) {
          fprintf(out, "\\x%02X", (unsigned)c);
          n += 4;
        } else {
          fputc(c, out);
          n++;
        }
    }
  }
  fputc('"', out);
  n++;
  return n;
}

// ───────────────────────── Tests ─────────────────────────
#ifdef VL_STRING_TEST_MAIN
int main(void) {
  struct VL_Context *ctx =
      NULL;  // adapter si votre fabrique a besoin d'un contexte réel
  VL_Value a = vl_make_str(NULL, "Hello");
  VL_Value b = vl_make_str(NULL, ", world");
  VL_Value c = vl_str_concat(ctx, a, b);
  vl_value_print(&c, stdout);
  fputc('\n', stdout);
  VL_Value s = vl_str_slice_v(ctx, c, 7, 5);
  vl_value_print(&s, stdout);
  fputc('\n', stdout);
  VL_Value u = vl_str_upper_v(ctx, c);
  vl_value_print(&u, stdout);
  fputc('\n', stdout);
  VL_Value t = vl_str_trim_v(ctx, vl_make_str(NULL, "  x  "));
  vl_value_print(&t, stdout);
  fputc('\n', stdout);
  int64_t iv;
  double dv;
  (void)vl_str_to_int64(vl_make_str(NULL, "42").as.s, &iv);
  (void)vl_str_to_double(vl_make_str(NULL, "3.5").as.s, &dv);
  fprintf(stderr, "iv=%" PRId64 " dv=%g\n", iv, dv);
  return 0;
}
#endif
