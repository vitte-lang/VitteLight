// vitte-light/core/ctype_ext.c
// Fonctions avancées autour des types/valeurs VitteLight.
// Compile: cc -std=c99 -O2 -Wall -Wextra -pedantic -c ctype_ext.c

#include "ctype.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "api.h"

// ───────────────────── Égalité / comparaison ─────────────────────
bool vl_value_equal(const VL_Value *a, const VL_Value *b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->type != b->type) {
    if ((a->type == VT_INT || a->type == VT_FLOAT) &&
        (b->type == VT_INT || b->type == VT_FLOAT)) {
      double x = (a->type == VT_FLOAT) ? a->as.f : (double)a->as.i;
      double y = (b->type == VT_FLOAT) ? b->as.f : (double)b->as.i;
      return x == y;
    }
    return false;
  }
  switch (a->type) {
    case VT_NIL:
      return true;
    case VT_BOOL:
      return a->as.b == b->as.b;
    case VT_INT:
      return a->as.i == b->as.i;
    case VT_FLOAT:
      return a->as.f == b->as.f;
    case VT_STR:
      return a->as.s == b->as.s ||
             (a->as.s && b->as.s && a->as.s->len == b->as.s->len &&
              memcmp(a->as.s->data, b->as.s->data, a->as.s->len) == 0);
    default:
      return a->as.ptr == b->as.ptr;
  }
}

int vl_value_compare(const VL_Value *a, const VL_Value *b) {
  if (!a || !b) return -2;
  if ((a->type == VT_INT || a->type == VT_FLOAT) &&
      (b->type == VT_INT || b->type == VT_FLOAT)) {
    double x = (a->type == VT_FLOAT) ? a->as.f : (double)a->as.i;
    double y = (b->type == VT_FLOAT) ? b->as.f : (double)b->as.i;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
  }
  if (a->type == VT_STR && b->type == VT_STR) {
    size_t la = a->as.s ? a->as.s->len : 0, lb = b->as.s ? b->as.s->len : 0;
    size_t m = la < lb ? la : lb;
    int c = memcmp(a->as.s ? a->as.s->data : NULL,
                   b->as.s ? b->as.s->data : NULL, m);
    if (c < 0) return -1;
    if (c > 0) return 1;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
  }
  return -2;
}

// ───────────────────── Arithmétique sûre ─────────────────────
static bool add_ovf_i64(int64_t a, int64_t b, int64_t *r) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
  return __builtin_add_overflow(a, b, r);
#endif
#endif
  if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) return true;
  *r = a + b;
  return false;
}
static bool sub_ovf_i64(int64_t a, int64_t b, int64_t *r) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_sub_overflow)
  return __builtin_sub_overflow(a, b, r);
#endif
#endif
  if ((b < 0 && a > LLONG_MAX + b) || (b > 0 && a < LLONG_MIN + b)) return true;
  *r = a - b;
  return false;
}
static bool mul_ovf_i64(int64_t a, int64_t b, int64_t *r) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
  return __builtin_mul_overflow(a, b, r);
#endif
#endif
  if (a == 0 || b == 0) {
    *r = 0;
    return false;
  }
  if (a == -1 && b == LLONG_MIN) return true;
  if (b == -1 && a == LLONG_MIN) return true;
  int64_t q = a * b;
  if (q / b != a) return true;
  *r = q;
  return false;
}

static VL_Status num_binop(const VL_Value *a, const VL_Value *b, int op,
                           VL_Value *out) {
  if (!a || !b || !out) return VL_ERR_BAD_ARG;
  if ((a->type == VT_INT || a->type == VT_FLOAT) &&
      (b->type == VT_INT || b->type == VT_FLOAT)) {
    if (a->type == VT_INT && b->type == VT_INT && op != 3) {
      int64_t r = 0;
      bool ov = false;
      if (op == 0)
        ov = add_ovf_i64(a->as.i, b->as.i, &r);
      else if (op == 1)
        ov = sub_ovf_i64(a->as.i, b->as.i, &r);
      else
        ov = mul_ovf_i64(a->as.i, b->as.i, &r);
      if (ov) {
        double x = (double)a->as.i, y = (double)b->as.i;
        double rr = (op == 0) ? x + y : (op == 1) ? x - y : x * y;
        *out = vlv_float(rr);
        return VL_OK;
      }
      *out = vlv_int(r);
      return VL_OK;
    }
    double x = (a->type == VT_FLOAT) ? a->as.f : (double)a->as.i;
    double y = (b->type == VT_FLOAT) ? b->as.f : (double)b->as.i;
    if (op == 3) {
      if (y == 0.0) return VL_ERR_RUNTIME;
      *out = vlv_float(x / y);
      return VL_OK;
    }
    double rr = (op == 0) ? x + y : (op == 1) ? x - y : x * y;
    *out = vlv_float(rr);
    return VL_OK;
  }
  return VL_ERR_RUNTIME;
}

VL_Status vl_value_add(const VL_Value *a, const VL_Value *b, VL_Value *out) {
  return num_binop(a, b, 0, out);
}
VL_Status vl_value_sub(const VL_Value *a, const VL_Value *b, VL_Value *out) {
  return num_binop(a, b, 1, out);
}
VL_Status vl_value_mul(const VL_Value *a, const VL_Value *b, VL_Value *out) {
  return num_binop(a, b, 2, out);
}
VL_Status vl_value_div(const VL_Value *a, const VL_Value *b, VL_Value *out) {
  return num_binop(a, b, 3, out);
}

// ───────────────────── Parsing et JSON ─────────────────────
bool vl_parse_i64(const char *s, int64_t *out) {
  if (!s) return false;
  char *end = NULL;
  long long v = strtoll(s, &end, 0);
  if (end == s) return false;
  if (out) *out = (int64_t)v;
  return true;
}
bool vl_parse_f64(const char *s, double *out) {
  if (!s) return false;
  char *end = NULL;
  double v = strtod(s, &end);
  if (end == s) return false;
  if (out) *out = v;
  return true;
}
bool vl_parse_bool(const char *s, bool *out) {
  if (!s) return false;
  if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0) {
    if (out) *out = true;
    return true;
  }
  if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0) {
    if (out) *out = false;
    return true;
  }
  return false;
}

VL_Status vl_value_parse(struct VL_Context *ctx, VL_Type t, const char *s,
                         VL_Value *out) {
  if (!out) return VL_ERR_BAD_ARG;
  switch (t) {
    case VT_BOOL: {
      bool b;
      if (!vl_parse_bool(s, &b)) return VL_ERR_BAD_ARG;
      *out = vlv_bool(b);
      return VL_OK;
    }
    case VT_INT: {
      int64_t i;
      if (!vl_parse_i64(s, &i)) return VL_ERR_BAD_ARG;
      *out = vlv_int(i);
      return VL_OK;
    }
    case VT_FLOAT: {
      double d;
      if (!vl_parse_f64(s, &d)) return VL_ERR_BAD_ARG;
      *out = vlv_float(d);
      return VL_OK;
    }
    case VT_STR: {
      if (!ctx) return VL_ERR_BAD_ARG;
      *out = vl_make_str(ctx, s ? s : "");
      return (out->type == VT_STR) ? VL_OK : VL_ERR_OOM;
    }
    case VT_NIL: {
      *out = vlv_nil();
      return VL_OK;
    }
    default:
      return VL_ERR_BAD_ARG;
  }
}

size_t vl_value_to_json(const VL_Value *v, char *buf, size_t n) {
  if (!buf || n == 0) return 0;
  switch (v ? v->type : VT_NIL) {
    case VT_NIL:
      return (size_t)snprintf(buf, n, "null");
    case VT_BOOL:
      return (size_t)snprintf(buf, n, v->as.b ? "true" : "false");
    case VT_INT:
      return (size_t)snprintf(buf, n, "%" PRId64, v->as.i);
    case VT_FLOAT:
      return (size_t)snprintf(buf, n, "%.*g", 17, v->as.f);
    case VT_STR: {
      size_t w = 0;
      if (w < n) buf[w++] = '"';
      for (uint32_t i = 0; i < (v->as.s ? v->as.s->len : 0); i++) {
        char c = v->as.s->data[i];
        const char *esc = NULL;
        char tmp[7];
        if (c == '"')
          esc = "\\\"";
        else if (c == '\\')
          esc = "\\\\";
        else if (c == '\n')
          esc = "\\n";
        else if ((unsigned char)c < 0x20) {
          snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned char)c);
          esc = tmp;
        }
        if (esc) {
          size_t el = strlen(esc);
          if (w + el < n) memcpy(buf + w, esc, el);
          w += el;
        } else {
          if (w + 1 < n) buf[w] = c;
          w++;
        }
      }
      if (w < n) buf[w] = '"';
      w++;
      if (w < n) buf[w] = '\0';
      return w - 1;
    }
    default:
      return (size_t)snprintf(buf, n, "\"<%s>\"", vl_type_name(v->type));
  }
}
