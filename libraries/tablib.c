// vitte-light/libraries/mathlib.c
// Mathlib pour VitteLight: enregistrement de natives numériques complètes.
// Portabilité C99. Sans dépendance forte au runtime, mais expose
//   void vl_register_mathlib(struct VL_Context *ctx);
// pour attacher les fonctions dans l'espace global de la VM.
//
// Fonctions fournies (toutes scalaires, doubles):
//  - trig: sin, cos, tan, asin, acos, atan, atan2
//  - exp/log: exp, log, log10, pow
//  - racines/arrondi: sqrt, cbrt, floor, ceil, round, fmod, hypot
//  - helpers: abs, min, max, clamp(x,lo,hi), lerp(a,b,t), mix(a,b,t)
//  - unités: rad(deg), deg(rad), pi(), tau(), e()
//  - stats varargs: sum(...), mean(...), var(...), stddev(...)
//  - aléa: srand(seed), rand()∈[0,1), rand_u32(), randn() (loi normale ~N(0,1))
//  - approx: approx_eq(a,b[,eps])
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/mathlib.c

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"  // vl_value_as_int/float, vl_value_truthy
#include "tm.h"     // vl_mono_time_ns (seed optionnelle)

#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884
#endif
#ifndef M_E
#define M_E 2.718281828459045235360287471352662497
#endif

// ───────────────────────── Helpers VM ─────────────────────────
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
#define RET_FLOAT(v)                          \
  do {                                        \
    if (ret) *(ret) = vlv_float((double)(v)); \
    return VL_OK;                             \
  } while (0)
#define RET_BOOL(v)                       \
  do {                                    \
    if (ret) *(ret) = vlv_bool((v) != 0); \
    return VL_OK;                         \
  } while (0)

static int to_double(const VL_Value *x, double *out) {
  if (!x) return 0;
  switch (x->type) {
    case VT_FLOAT:
      if (out) *out = x->as.f;
      return 1;
    case VT_INT:
      if (out) *out = (double)x->as.i;
      return 1;
    case VT_BOOL:
      if (out) *out = x->as.b ? 1.0 : 0.0;
      return 1;
    default:
      return 0;
  }
}
static int to_int(const VL_Value *x, int64_t *out) {
  return vl_value_as_int(x, out);
}

static double fmin_many(const VL_Value *a, uint8_t c, int *ok) {
  double m = 0.0, v = 0.0;
  *ok = 0;
  for (uint8_t i = 0; i < c; i++) {
    if (!to_double(&a[i], &v)) return 0.0;
    if (!*ok || v < m) m = v;
    *ok = 1;
  }
  return m;
}
static double fmax_many(const VL_Value *a, uint8_t c, int *ok) {
  double M = 0.0, v = 0.0;
  *ok = 0; for(uint8_t i=0;i<c;i"){ if(!to_double(&a[i],&v)) return 0.0; if(!*ok || v>M) M=v; *ok=1; } return M; }

// ───────────────────────── PRNG ─────────────────────────
static uint64_t g_rng = 0; // 0 => non-initialisé
static inline uint64_t xorshift64(void){
    if (g_rng == 0) {  // seed à partir de l'horloge
      uint64_t t = (uint64_t)vl_mono_time_ns();
      g_rng = t ? t : 88172645463393265ull;
    }
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x; }
static inline uint32_t xorshift32(void){
    return (uint32_t)(xorshift64() >> 32); }
static inline double   urand01(void){
    return (xorshift64() >> 11) * (1.0 / 9007199254740992.0); } // 53 bits -> [0,1)

// ───────────────────────── Impl natives ─────────────────────────
// trig
static VL_Status nb_sin (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(sin(x)); }
static VL_Status nb_cos (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(cos(x)); }
static VL_Status nb_tan (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(tan(x)); }
static VL_Status nb_asin(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(asin(x)); }
static VL_Status nb_acos(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(acos(x)); }
static VL_Status nb_atan(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(atan(x)); }
static VL_Status nb_atan2(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double y, x;
    if (c < 2 || !to_double(&a[0], &y) || !to_double(&a[1], &x))
      return VL_ERR_TYPE;
    RET_FLOAT(atan2(y, x)); }

// exp/log
static VL_Status nb_exp (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(exp(x)); }
static VL_Status nb_log (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(log(x)); }
static VL_Status nb_log10(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(log10(x)); }
static VL_Status nb_pow (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x, y;
    if (c < 2 || !to_double(&a[0], &x) || !to_double(&a[1], &y))
      return VL_ERR_TYPE;
    RET_FLOAT(pow(x, y)); }

// racines / arrondis
static VL_Status nb_sqrt (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
    RET_FLOAT(sqrt(x)); }
static VL_Status nb_cbrt (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *u){
    (void)ctx;
    (void)u;
    double x;
    if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
#ifdef cbrt RET_FLOAT(cbrt(x));
#else RET_FLOAT(pow(x, 1.0 / 3.0));
#endif }
    static VL_Status nb_floor(struct VL_Context * ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x;
      if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
      RET_FLOAT(floor(x));
    }
    static VL_Status nb_ceil(struct VL_Context * ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x;
      if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
      RET_FLOAT(ceil(x));
    }
    static VL_Status nb_round(struct VL_Context * ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x;
      if (c < 1 || !to_double(&a[0], &x)) return VL_ERR_TYPE;
      RET_FLOAT(round(x));
    }
    static VL_Status nb_fmod(struct VL_Context * ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x, y;
      if (c < 2 || !to_double(&a[0], &x) || !to_double(&a[1], &y))
        return VL_ERR_TYPE;
      RET_FLOAT(fmod(x, y));
    }
    static VL_Status nb_hypot(struct VL_Context * ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x, y;
      if (c < 2 || !to_double(&a[0], &x) || !to_double(&a[1], &y))
        return VL_ERR_TYPE;
      RET_FLOAT(hypot(x, y));
    }

    // helpers
    static VL_Status nb_abs(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      if (c < 1) return VL_ERR_INVAL;
      if (a[0].type == VT_INT)
        RET_INT((a[0].as.i < 0) ? -a[0].as.i : a[0].as.i);
      double x;
      if (!to_double(&a[0], &x)) return VL_ERR_TYPE;
      RET_FLOAT(fabs(x));
    }
    static VL_Status nb_min(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      int ok = 0;
      if (c < 1) return VL_ERR_INVAL;
      double m = fmin_many(a, c, &ok);
      if (!ok) return VL_ERR_TYPE;
      RET_FLOAT(m);
    }
    static VL_Status nb_max(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      int ok = 0;
      if (c < 1) return VL_ERR_INVAL;
      double M = fmax_many(a, c, &ok);
      if (!ok) return VL_ERR_TYPE;
      RET_FLOAT(M);
    }
    static VL_Status nb_clamp(struct VL_Context * ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x, lo, hi;
      if (c < 3 || !to_double(&a[0], &x) || !to_double(&a[1], &lo) ||
          !to_double(&a[2], &hi))
        return VL_ERR_TYPE;
      if (lo > hi) {
        double t = lo;
        lo = hi;
        hi = t;
      }
      if (x < lo) x = lo;
      if (x > hi) x = hi;
      RET_FLOAT(x);
    }
    static VL_Status nb_lerp(struct VL_Context * ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x, y, t;
      if (c < 3 || !to_double(&a[0], &x) || !to_double(&a[1], &y) ||
          !to_double(&a[2], &t))
        return VL_ERR_TYPE;
      RET_FLOAT(x + (y - x) * t);
    }

    // unités/constantes
    static VL_Status nb_rad(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double d;
      if (c < 1 || !to_double(&a[0], &d)) return VL_ERR_TYPE;
      RET_FLOAT(d * (M_PI / 180.0));
    }
    static VL_Status nb_deg(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double r;
      if (c < 1 || !to_double(&a[0], &r)) return VL_ERR_TYPE;
      RET_FLOAT(r * (180.0 / M_PI));
    }
    static VL_Status nb_pi(struct VL_Context * ctx, const VL_Value *a,
                           uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)a;
      (void)c;
      (void)u;
      RET_FLOAT(M_PI);
    }
    static VL_Status nb_tau(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)a;
      (void)c;
      (void)u;
      RET_FLOAT(2.0 * M_PI);
    }
    static VL_Status nb_e(struct VL_Context * ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
      (void)ctx;
      (void)a;
      (void)c;
      (void)u;
      RET_FLOAT(M_E);
    }

    // stats varargs
    static VL_Status nb_sum(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      if (c < 1) RET_FLOAT(0.0);
      double s = 0.0, v;
      for (uint8_t i = 0; i < c; i++) {
        if (!to_double(&a[i], &v)) return VL_ERR_TYPE;
        s += v;
      }
      RET_FLOAT(s);
    }
    static VL_Status nb_mean(struct VL_Context * ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      if (c < 1) return VL_ERR_INVAL;
      double s = 0.0, v;
      for (uint8_t i = 0; i < c; i++) {
        if (!to_double(&a[i], &v)) return VL_ERR_TYPE;
        s += v;
      }
      RET_FLOAT(s / (double)c);
    }
    static VL_Status nb_var(struct VL_Context * ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      if (c < 2) return VL_ERR_INVAL;
      double s = 0.0, ss = 0.0, v;
      for (uint8_t i = 0; i < c; i++) {
        if (!to_double(&a[i], &v)) return VL_ERR_TYPE;
        s += v;
        ss += v * v;
      }
      double n = (double)c;
      double var = (ss - (s * s) / n) / (n - 1.0);
      RET_FLOAT(var);
    }
    static VL_Status nb_stddev(struct VL_Context * ctx, const VL_Value *a,
                               uint8_t c, VL_Value *ret, void *u) {
      VL_Value t;
      (void)ctx;
      (void)u;
      VL_Status st = nb_var(ctx, a, c, &t, u);
      if (st != VL_OK) return st;
      if (t.type != VT_FLOAT) return VL_ERR_TYPE;
      RET_FLOAT(sqrt(t.as.f));
    }

    // approx
    static VL_Status nb_approx(struct VL_Context * ctx, const VL_Value *a,
                               uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      double x, y, eps = 1e-9;
      if (c < 2 || !to_double(&a[0], &x) || !to_double(&a[1], &y))
        return VL_ERR_TYPE;
      if (c >= 3 && !to_double(&a[2], &eps)) return VL_ERR_TYPE;
      RET_BOOL(fabs(x - y) <= eps);
    }

    // aléa
    static VL_Status nb_srand(struct VL_Context * ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)u;
      if (c < 1) return VL_ERR_INVAL;
      int64_t s = 0;
      if (!to_int(&a[0], &s)) return VL_ERR_TYPE;
      g_rng = (uint64_t)s;
      if (g_rng == 0) g_rng = 1;
      RET_NIL();
    }
    static VL_Status nb_rand01(struct VL_Context * ctx, const VL_Value *a,
                               uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)a;
      (void)c;
      (void)u;
      RET_FLOAT(urand01());
    }
    static VL_Status nb_rand_u32(struct VL_Context * ctx, const VL_Value *a,
                                 uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)a;
      (void)c;
      (void)u;
      RET_INT((int64_t)xorshift32());
    }
    static VL_Status nb_randn(struct VL_Context * ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
      (void)ctx;
      (void)a;
      (void)c;
      (void)u;  // Box-Muller
      double u1 = urand01();
      if (u1 <= 0.0) u1 = 1e-12;
      double u2 = urand01();
      double r = sqrt(-2.0 * log(u1));
      double th = 2.0 * M_PI * u2;
      RET_FLOAT(r * cos(th));
    }

    // ───────────────────────── Enregistrement ─────────────────────────
    void vl_register_mathlib(struct VL_Context * ctx) {
      if (!ctx) return;
      vl_register_native(ctx, "sin", nb_sin, NULL);
      vl_register_native(ctx, "cos", nb_cos, NULL);
      vl_register_native(ctx, "tan", nb_tan, NULL);
      vl_register_native(ctx, "asin", nb_asin, NULL);
      vl_register_native(ctx, "acos", nb_acos, NULL);
      vl_register_native(ctx, "atan", nb_atan, NULL);
      vl_register_native(ctx, "atan2", nb_atan2, NULL);

      vl_register_native(ctx, "exp", nb_exp, NULL);
      vl_register_native(ctx, "log", nb_log, NULL);
      vl_register_native(ctx, "log10", nb_log10, NULL);
      vl_register_native(ctx, "pow", nb_pow, NULL);

      vl_register_native(ctx, "sqrt", nb_sqrt, NULL);
      vl_register_native(ctx, "cbrt", nb_cbrt, NULL);
      vl_register_native(ctx, "floor", nb_floor, NULL);
      vl_register_native(ctx, "ceil", nb_ceil, NULL);
      vl_register_native(ctx, "round", nb_round, NULL);
      vl_register_native(ctx, "fmod", nb_fmod, NULL);
      vl_register_native(ctx, "hypot", nb_hypot, NULL);

      vl_register_native(ctx, "abs", nb_abs, NULL);
      vl_register_native(ctx, "min", nb_min, NULL);
      vl_register_native(ctx, "max", nb_max, NULL);
      vl_register_native(ctx, "clamp", nb_clamp, NULL);
      vl_register_native(ctx, "lerp", nb_lerp, NULL);
      vl_register_native(ctx, "mix", nb_lerp, NULL);  // alias

      vl_register_native(ctx, "rad", nb_rad, NULL);
      vl_register_native(ctx, "deg", nb_deg, NULL);
      vl_register_native(ctx, "pi", nb_pi, NULL);
      vl_register_native(ctx, "tau", nb_tau, NULL);
      vl_register_native(ctx, "e", nb_e, NULL);

      vl_register_native(ctx, "sum", nb_sum, NULL);
      vl_register_native(ctx, "mean", nb_mean, NULL);
      vl_register_native(ctx, "var", nb_var, NULL);
      vl_register_native(ctx, "stddev", nb_stddev, NULL);

      vl_register_native(ctx, "approx_eq", nb_approx, NULL);

      vl_register_native(ctx, "srand", nb_srand, NULL);
      vl_register_native(ctx, "rand", nb_rand01, NULL);
      vl_register_native(ctx, "rand_u32", nb_rand_u32, NULL);
      vl_register_native(ctx, "randn", nb_randn, NULL);
    }

// ───────────────────────── Test rapide ─────────────────────────
#ifdef VL_MATHLIB_TEST_MAIN
    int main(void) {
      // Smoke tests côté C
      printf("sin(pi/2)=%.3f\n", sin(M_PI / 2));
      // Pas de VM ici; les natives sont destinées au runtime VL.
      return 0;
    }
#endif
