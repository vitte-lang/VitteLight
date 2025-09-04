// vitte-light/core/func.c
// Natives avec signatures, coercions et mini-stdlib pour VitteLight.
// Fournit:
//   - vl_register_native_sig(ctx, name, sig, fn, ud)
//       * Vérifie l'arité et les types selon une signature compacte
//       * Effectue des conversions sûres (int<->float, value->string,
//       truthiness->bool, parse str)
//       * Appelle votre VL_NativeFn avec des arguments déjà convertis
//   - vl_register_std_natives(ctx, flags)
//       * math: sin, cos, tan, sqrt, pow, abs, min, max
//       * str:  len, concat
//
// Signature DSL:
//   "i,f,s,b,n,a"  => int64, double, string, bool, nil, any
//   Exemples:
//     "i,i->i"      add deux int -> int
//     "f->f"        mono float -> float
//     "s,s->s"      concat
//     "a->n"        arg quelconque, pas de retour (nil)
//   Parenthèses optionnelles: "(i,f)->f" OK. Espaces ignorés.
//   Varargs: terminez la liste par ",*" pour autoriser des args supplémentaires
//   de tout type.
//            ex: "s,*->s" (premier arg string requis, puis n'importe quoi)
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/func.c

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"

// ───────────────────────── Types internes ─────────────────────────

typedef enum { SK_I, SK_F, SK_S, SK_B, SK_N, SK_A } VL_ScalarK;

typedef struct {
  VL_ScalarK ret;
  VL_ScalarK args[16];
  size_t argc;
  int vararg;  // 1 si ",*" présent
} VL_FuncSig;

typedef struct {
  VL_FuncSig sig;
  VL_NativeFn user;
  void *user_ud;
  const char *name;  // debug only
} VL_FuncWrap;

// ───────────────────────── Parsing de signature ─────────────────────────
static int sk_from_char(char c, VL_ScalarK *out) {
  switch (c) {
    case 'i':
      *out = SK_I;
      return 1;
    case 'f':
      *out = SK_F;
      return 1;
    case 's':
      *out = SK_S;
      return 1;
    case 'b':
      *out = SK_B;
      return 1;
    case 'n':
      *out = SK_N;
      return 1;
    case 'a':
      *out = SK_A;
      return 1;
    default:
      return 0;
  }
}

static void skip_ws(const char **p) {
  while (**p == ' ' || **p == '\t') (*p)++;
}

static int parse_sig(const char *sig, VL_FuncSig *out) {
  if (!sig || !out) return 0;
  VL_FuncSig s = {0};
  const char *p = sig;
  skip_ws(&p);
  if (*p == '(') {
    p++;
  }
  // args
  while (*p && *p != ')' && *p != '-') {
    skip_ws(&p);
    if (*p == '*') {
      s.vararg = 1;
      p++;
      break;
    }
    VL_ScalarK k;
    if (!sk_from_char(*p, &k)) return 0;
    p++;
    if (s.argc >= 16) return 0;
    s.args[s.argc++] = k;
    skip_ws(&p);
    if (*p == ',') {
      p++;
      continue;
    }
  }
  if (*p == ')') p++;
  skip_ws(&p);
  if (*p != '-') {  // pas de retour explicite => par défaut n (nil)
    s.ret = SK_N;
    *out = s;
    return 1;
  }
  p++;
  if (*p != '>') return 0;
  p++;
  skip_ws(&p);
  VL_ScalarK r;
  if (!sk_from_char(*p, &r)) return 0;
  s.ret = r;
  p++;
  skip_ws(&p);
  if (*p) {           // optionnelle fin
    if (*p == ',') {  // support legacy "args->ret, *" (rare)
      p++;
      skip_ws(&p);
      if (*p == '*') {
        s.vararg = 1;
        p++;
      }
    }
    skip_ws(&p);
    if (*p) return 0;
  }
  *out = s;
  return 1;
}

static const char *sk_name(VL_ScalarK k) {
  switch (k) {
    case SK_I:
      return "int";
    case SK_F:
      return "float";
    case SK_S:
      return "str";
    case SK_B:
      return "bool";
    case SK_N:
      return "nil";
    case SK_A:
      return "any";
    default:
      return "?";
  }
}

// ───────────────────────── Coercions ─────────────────────────
static int coerce_to_str(struct VL_Context *ctx, const VL_Value *in,
                         VL_Value *out) {
  if (in->type == VT_STR) {
    *out = *in;
    return 1;
  }
  // stringify via vl_value_to_cstr
  size_t cap = 128;
  char *buf = (char *)malloc(cap);
  if (!buf) return 0;
  size_t wrote = vl_value_to_cstr(in, buf, cap);
  if (wrote >= cap - 1) {  // réallouer
    cap = wrote + 1;
    char *tmp = (char *)realloc(buf, cap);
    if (!tmp) {
      free(buf);
      return 0;
    }
    buf = tmp;
    vl_value_to_cstr(in, buf, cap);
  }
  VL_Value s = vl_make_str(ctx, buf);
  free(buf);
  if (s.type != VT_STR) return 0;
  *out = s;
  return 1;
}

static int coerce_value(struct VL_Context *ctx, const VL_Value *in,
                        VL_ScalarK target, VL_Value *out) {
  if (target == SK_A) {
    *out = *in;
    return 1;
  }
  switch (target) {
    case SK_N:
      *out = vlv_nil();
      return 1;
    case SK_B: {
      bool b = vl_value_truthy(in);
      *out = vlv_bool(b);
      return 1;
    }
    case SK_I: {
      int64_t iv = 0;
      if (in->type == VT_INT) {
        *out = *in;
        return 1;
      }
      if (in->type == VT_FLOAT) {
        *out = vlv_int((int64_t)in->as.f);
        return 1;
      }
      if (in->type == VT_BOOL) {
        *out = vlv_int(in->as.b ? 1 : 0);
        return 1;
      }
      // tenter parse string
      if (in->type == VT_STR) {
        int64_t t;
        if (vl_parse_i64((const char *)in->as.s->data, &t)) {
          *out = vlv_int(t);
          return 1;
        }
      }
      return 0;
    }
    case SK_F: {
      double dv = 0.0;
      if (in->type == VT_FLOAT) {
        *out = *in;
        return 1;
      }
      if (in->type == VT_INT) {
        *out = vlv_float((double)in->as.i);
        return 1;
      }
      if (in->type == VT_BOOL) {
        *out = vlv_float(in->as.b ? 1.0 : 0.0);
        return 1;
      }
      if (in->type == VT_STR) {
        double t;
        if (vl_parse_f64((const char *)in->as.s->data, &t)) {
          *out = vlv_float(t);
          return 1;
        }
      }
      return 0;
    }
    case SK_S:
      return coerce_to_str(ctx, in, out);
    default:
      return 0;
  }
}

// ───────────────────────── Trampoline natif ─────────────────────────
static VL_Status native_dispatch(struct VL_Context *ctx, VL_Value *args,
                                 size_t argc, VL_Value *ret, void *user) {
  VL_FuncWrap *w = (VL_FuncWrap *)user;
  if (!w) return VL_ERR_RUNTIME;
  // Vérif arité
  if (argc < w->sig.argc) return VL_ERR_BAD_ARG;
  if (!w->sig.vararg && argc != w->sig.argc) return VL_ERR_BAD_ARG;
  // Convertir les N premiers args
  VL_Value tmp[32];
  if (argc > 32) return VL_ERR_BAD_ARG;  // limite raisonnable
  for (size_t i = 0; i < w->sig.argc; i++) {
    if (!coerce_value(ctx, &args[i], w->sig.args[i], &tmp[i]))
      return VL_ERR_BAD_ARG;
  }
  // Copier varargs tels quels
  for (size_t i = w->sig.argc; i < argc; i++) tmp[i] = args[i];
  // Appel utilisateur
  VL_Value user_ret = vlv_nil();
  VL_Status rc = w->user(ctx, tmp, argc, &user_ret, w->user_ud);
  if (rc != VL_OK) return rc;
  // Coercer le retour
  VL_Value outv;
  if (!coerce_value(ctx, &user_ret, w->sig.ret, &outv)) return VL_ERR_BAD_ARG;
  if (ret) *ret = outv;
  return VL_OK;
}

// ───────────────────────── API: enregistrement avec signature
// ─────────────────────────
VL_Status vl_register_native_sig(struct VL_Context *ctx, const char *name,
                                 const char *sig, VL_NativeFn user_fn,
                                 void *user_ud) {
  if (!ctx || !name || !sig || !user_fn) return VL_ERR_BAD_ARG;
  VL_FuncSig s = {0};
  if (!parse_sig(sig, &s)) return VL_ERR_BAD_ARG;
  VL_FuncWrap *w = (VL_FuncWrap *)malloc(sizeof(*w));
  if (!w) return VL_ERR_OOM;
  w->sig = s;
  w->user = user_fn;
  w->user_ud = user_ud;
  w->name = name;
  return vl_register_native(ctx, name, native_dispatch, w);
}

// ───────────────────────── Mini-stdlib: math ─────────────────────────
static VL_Status nf_sin(struct VL_Context *ctx, VL_Value *a, size_t n,
                        VL_Value *r, void *ud) {
  (void)ud;
  double x = 0;
  vl_value_as_float(&a[0], &x);
  if (r) *r = vlv_float(sin(x));
  return VL_OK;
}
static VL_Status nf_cos(struct VL_Context *ctx, VL_Value *a, size_t n,
                        VL_Value *r, void *ud) {
  (void)ud;
  double x = 0;
  vl_value_as_float(&a[0], &x);
  if (r) *r = vlv_float(cos(x));
  return VL_OK;
}
static VL_Status nf_tan(struct VL_Context *ctx, VL_Value *a, size_t n,
                        VL_Value *r, void *ud) {
  (void)ud;
  double x = 0;
  vl_value_as_float(&a[0], &x);
  if (r) *r = vlv_float(tan(x));
  return VL_OK;
}
static VL_Status nf_sqrt(struct VL_Context *ctx, VL_Value *a, size_t n,
                         VL_Value *r, void *ud) {
  (void)ud;
  double x = 0;
  vl_value_as_float(&a[0], &x);
  if (x < 0) return VL_ERR_RUNTIME;
  if (r) *r = vlv_float(sqrt(x));
  return VL_OK;
}
static VL_Status nf_pow(struct VL_Context *ctx, VL_Value *a, size_t n,
                        VL_Value *r, void *ud) {
  (void)ud;
  double x = 0, y = 0;
  vl_value_as_float(&a[0], &x);
  vl_value_as_float(&a[1], &y);
  if (r) *r = vlv_float(pow(x, y));
  return VL_OK;
}
static VL_Status nf_abs(struct VL_Context *ctx, VL_Value *a, size_t n,
                        VL_Value *r, void *ud) {
  (void)ud;
  if (a[0].type == VT_INT) {
    if (r) *r = vlv_int(a[0].as.i < 0 ? -a[0].as.i : a[0].as.i);
  } else {
    double x = 0;
    vl_value_as_float(&a[0], &x);
    if (r) *r = vlv_float(fabs(x));
  }
  return VL_OK;
}
static VL_Status nf_min(struct VL_Context *ctx, VL_Value *a, size_t n,
                        VL_Value *r, void *ud) {
  (void)ud;
  double x = 0, y = 0;
  vl_value_as_float(&a[0], &x);
  vl_value_as_float(&a[1], &y);
  if (r) *r = vlv_float(x < y ? x : y);
  return VL_OK;
}
static VL_Status nf_max(struct VL_Context *ctx, VL_Value *a, size_t n,
                        VL_Value *r, void *ud) {
  (void)ud;
  double x = 0, y = 0;
  vl_value_as_float(&a[0], &x);
  vl_value_as_float(&a[1], &y);
  if (r) *r = vlv_float(x > y ? x : y);
  return VL_OK;
}

// ───────────────────────── Mini-stdlib: string ─────────────────────────
static VL_Status nf_strlen(struct VL_Context *ctx, VL_Value *a, size_t n,
                           VL_Value *r, void *ud) {
  (void)ud;
  VL_Value s;
  if (!coerce_value(ctx, &a[0], SK_S, &s)) return VL_ERR_BAD_ARG;
  if (r) *r = vlv_int((int64_t)s.as.s->len);
  return VL_OK;
}
static VL_Status nf_concat(struct VL_Context *ctx, VL_Value *a, size_t n,
                           VL_Value *r, void *ud) {
  (void)ud;  // support varargs strings
  // convertir tous les args en string puis concaténer
  size_t total = 0;
  VL_Value sv[32];
  if (n > 32) return VL_ERR_BAD_ARG;
  for (size_t i = 0; i < n; i++) {
    if (!coerce_value(ctx, &a[i], SK_S, &sv[i])) return VL_ERR_BAD_ARG;
    total += sv[i].as.s ? sv[i].as.s->len : 0;
  }
  char *buf = (char *)malloc(total + 1);
  if (!buf) return VL_ERR_OOM;
  size_t off = 0;
  for (size_t i = 0; i < n; i++) {
    if (sv[i].as.s && sv[i].as.s->len) {
      memcpy(buf + off, sv[i].as.s->data, sv[i].as.s->len);
      off += sv[i].as.s->len;
    }
  }
  buf[off] = '\0';
  VL_Value out = vl_make_str(ctx, buf);
  free(buf);
  if (r) *r = out;
  return (out.type == VT_STR) ? VL_OK : VL_ERR_OOM;
}

// ───────────────────────── API: enregistrement stdlib
// ─────────────────────────
#define VLF_STD_MATH 0x01
#define VLF_STD_STR 0x02

VL_Status vl_register_std_natives(struct VL_Context *ctx, unsigned flags) {
  if (!ctx) return VL_ERR_BAD_ARG;
  VL_Status rc = VL_OK;
  if (flags & VLF_STD_MATH) {
    rc = vl_register_native_sig(ctx, "math.sin", "f->f", nf_sin, NULL);
    if (rc != VL_OK) return rc;
    rc = vl_register_native_sig(ctx, "math.cos", "f->f", nf_cos, NULL);
    if (rc != VL_OK) return rc;
    rc = vl_register_native_sig(ctx, "math.tan", "f->f", nf_tan, NULL);
    if (rc != VL_OK) return rc;
    rc = vl_register_native_sig(ctx, "math.sqrt", "f->f", nf_sqrt, NULL);
    if (rc != VL_OK) return rc;
    rc = vl_register_native_sig(ctx, "math.pow", "f,f->f", nf_pow, NULL);
    if (rc != VL_OK) return rc;
    rc = vl_register_native_sig(ctx, "math.abs", "a->a", nf_abs, NULL);
    if (rc != VL_OK) return rc;  // retourne même genre (int->int sinon float)
    rc = vl_register_native_sig(ctx, "math.min", "f,f->f", nf_min, NULL);
    if (rc != VL_OK) return rc;
    rc = vl_register_native_sig(ctx, "math.max", "f,f->f", nf_max, NULL);
    if (rc != VL_OK) return rc;
  }
  if (flags & VLF_STD_STR) {
    rc = vl_register_native_sig(ctx, "str.len", "s->i", nf_strlen, NULL);
    if (rc != VL_OK) return rc;
    rc = vl_register_native_sig(ctx, "str.concat", "s,*->s", nf_concat, NULL);
    if (rc != VL_OK) return rc;
  }
  return rc;
}

// ───────────────────────── Test autonome ─────────────────────────
#ifdef VL_FUNC_TEST_MAIN
#include "debug.h"
static void emit_u8(uint8_t **p, uint8_t v) { *(*p)++ = v; }
static void emit_u32(uint8_t **p, uint32_t v) {
  memcpy(*p, &v, 4);
  *p += 4;
}
static void emit_u64(uint8_t **p, uint64_t v) {
  memcpy(*p, &v, 8);
  *p += 8;
}
int main(void) {
  // Assemble: PUSHI 1; PUSHI 2; CALLN "math.pow" 2; CALLN print 1; HALT
  uint8_t buf[512];
  uint8_t *p = buf;
  emit_u8(&p, 'V');
  emit_u8(&p, 'L');
  emit_u8(&p, 'B');
  emit_u8(&p, 'C');
  emit_u8(&p, 1);
  const char *s0 = "math.pow";
  const char *s1 = "print";
  uint32_t nstr = 2;
  emit_u32(&p, nstr);
  emit_u32(&p, (uint32_t)strlen(s0));
  memcpy(p, s0, strlen(s0));
  p += strlen(s0);
  emit_u32(&p, (uint32_t)strlen(s1));
  memcpy(p, s1, strlen(s1));
  p += strlen(s1);
  uint8_t *code_sz_ptr = p;
  emit_u32(&p, 0);  // patch later
  uint8_t *cs = p;
  emit_u8(&p, 1 /*PUSHI*/);
  emit_u64(&p, 2);  // base
  emit_u8(&p, 1 /*PUSHI*/);
  emit_u64(&p, 10);  // exp
  emit_u8(&p, 18 /*CALLN*/);
  emit_u32(&p, 0);
  emit_u8(&p, 2);
  emit_u8(&p, 18 /*CALLN*/);
  emit_u32(&p, 1);
  emit_u8(&p, 1);
  emit_u8(&p, 19 /*HALT*/);
  uint32_t code_sz = (uint32_t)(p - cs);
  memcpy(code_sz_ptr, &code_sz, 4);

  VL_Context *vm = vl_create_default();
  if (!vm) {
    fprintf(stderr, "vm alloc fail\n");
    return 1;
  }
  VL_Status rc = vl_register_std_natives(vm, VLF_STD_MATH);
  if (rc != VL_OK) {
    fprintf(stderr, "stdlib rc=%d\n", rc);
    return 2;
  }
  rc = vl_load_program_from_memory(vm, buf, (size_t)(p - buf));
  if (rc != VL_OK) {
    fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
    return 3;
  }
  rc = vl_run(vm, 0);
  if (rc != VL_OK) {
    fprintf(stderr, "run: %s\n", vl_last_error(vm)->msg);
  }
  // stack/globals
  vl_destroy(vm);
  return 0;
}
#endif
