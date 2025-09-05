// SPDX-License-Identifier: GPL-3.0-or-later
//
// libm.c — Math standard library for Vitte Light (C17, portable)
// Namespace: "math"
//
// Functions:
//   Trigonometry:   sin, cos, tan, asin, acos, atan, atan2(y,x)
//   Hyperbolic:     sinh, cosh, tanh, asinh, acosh, atanh
//   Exponentials:   exp, exp2, log, log10, log2, pow(x,y), sqrt, cbrt
//   Rounding:       floor, ceil, trunc, round
//   Arithmetic:     fmod(x,y), hypot(x,y), copysign(x,y), nextafter(x,y)
//   Decompose:      frexp(x) -> mantissa, exp;  ldexp(x, exp)
//   Conversions:    rad(x), deg(x)    // radians <-> degrees
//   Predicates:     isfinite(x), isinf(x), isnan(x), sign(x) -> -1|0|1
//   Helpers:        clamp(x, lo, hi), lerp(a,b,t)
//   Random:         random([m[, n]]), randomseed(seed)
//
// Notes:
//   - All numeric inputs accept int or float; outputs are float unless
//   documented.
//   - random(): no args -> uniform double in [0,1). One arg m -> int in [1,m].
//               Two args m,n -> int in [m,n] (inclusive). Errors on invalid
//               ranges.
//   - No global constants injected to avoid VM API assumptions; expose via
//   functions if needed.
//
// Depends: includes/auxlib.h, state.h, object.h, vm.h

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884L
#endif

// ---------------------------------------------------------------------
// VM helpers
// ---------------------------------------------------------------------

static double vm_check_num(VL_State *S, int idx) {
  VL_Value *v = vl_get(S, idx);
  if (!v) {
    vl_errorf(S, "argument #%d: number expected", idx);
    return vl_error(S);
  }
  return vl_tonumber(S, v);
}

static int64_t vm_check_int(VL_State *S, int idx) {
  VL_Value *v = vl_get(S, idx);
  if (!v || !(vl_isint(S, idx) || vl_isfloat(S, idx))) {
    vl_errorf(S, "argument #%d: integer expected", idx);
    return vl_error(S);
  }
  return vl_isint(S, idx) ? vl_toint(S, v) : (int64_t)vl_tonumber(S, v);
}

static double vm_opt_num(VL_State *S, int idx, double defv) {
  VL_Value *v = vl_get(S, idx);
  if (!v) return defv;
  return vl_tonumber(S, v);
}

// ---------------------------------------------------------------------
// RNG (xorshift64*), thread-local
// ---------------------------------------------------------------------

#if defined(_WIN32)
#define TLS_SPEC __declspec(thread)
#else
#define TLS_SPEC __thread
#endif

static TLS_SPEC uint64_t g_rng = 0;

static inline uint64_t xorshift64s(uint64_t *s) {
  uint64_t x = *s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static void rng_ensure_seed(void) {
  if (g_rng == 0) {
    uint64_t v = 0;
    if (aux_rand_bytes(&v, sizeof v) != AUX_OK || v == 0) {
      v = aux_now_nanos() ^ 0x9E3779B97F4A7C15ULL;
    }
    if (v == 0) v = 0xD1B54A32D192ED03ULL;
    g_rng = v;
  }
}

static double rng_uniform01(void) {
  rng_ensure_seed();
  // 53-bit mantissa to double in [0,1)
  uint64_t r = xorshift64s(&g_rng);
  return (double)((r >> 11) & ((1ULL << 53) - 1)) *
         (1.0 / 9007199254740992.0);  // 2^53
}

// ---------------------------------------------------------------------
// Macros for wrappers
// ---------------------------------------------------------------------

#define M_UN(name, op)                \
  static int vm_##name(VL_State *S) { \
    double x = vm_check_num(S, 1);    \
    vl_push_float(S, op(x));          \
    return 1;                         \
  }

#define M_BIN(name, op)               \
  static int vm_##name(VL_State *S) { \
    double a = vm_check_num(S, 1);    \
    double b = vm_check_num(S, 2);    \
    vl_push_float(S, op(a, b));       \
    return 1;                         \
  }

// ---------------------------------------------------------------------
// Trig / Hyperbolic
// ---------------------------------------------------------------------

M_UN(sin, sin)
M_UN(cos, cos)
M_UN(tan, tan)
M_UN(asin, asin)
M_UN(acos, acos)
M_UN(atan, atan)
M_BIN(atan2, atan2)

M_UN(sinh, sinh)
M_UN(cosh, cosh)
M_UN(tanh, tanh)
M_UN(asinh, asinh)
M_UN(acosh, acosh)
M_UN(atanh, atanh)

// ---------------------------------------------------------------------
// Exponentials / Logs / Roots
// ---------------------------------------------------------------------

M_UN(exp, exp)
M_UN(exp2, exp2)
M_UN(log, log)
M_UN(log10, log10)
M_UN(log2, log2)
M_BIN(pow, pow)
M_UN(sqrt, sqrt)
M_UN(cbrt, cbrt)

// ---------------------------------------------------------------------
// Rounding / Decompose / Arithmetic helpers
// ---------------------------------------------------------------------

M_UN(floor, floor)
M_UN(ceil, ceil)
M_UN(trunc, trunc)
M_UN(round, round)
M_BIN(fmod, fmod)
M_BIN(hypot, hypot)
M_BIN(copysign, copysign)
M_BIN(nextafter, nextafter)

static int vm_frexp(VL_State *S) {
  int expv = 0;
  double x = vm_check_num(S, 1);
  double m = frexp(x, &expv);
  vl_push_float(S, m);
  vl_push_int(S, (int64_t)expv);
  return 2;
}

static int vm_ldexp(VL_State *S) {
  double x = vm_check_num(S, 1);
  int e = (int)vm_check_int(S, 2);
  vl_push_float(S, ldexp(x, e));
  return 1;
}

// ---------------------------------------------------------------------
// Conversions / Predicates / Small helpers
// ---------------------------------------------------------------------

static int vm_rad(VL_State *S) {
  double d = vm_check_num(S, 1);
  vl_push_float(S, d * (double)(M_PI / 180.0));
  return 1;
}
static int vm_deg(VL_State *S) {
  double r = vm_check_num(S, 1);
  vl_push_float(S, r * (double)(180.0 / M_PI));
  return 1;
}

static int vm_isfinite(VL_State *S) {
  double x = vm_check_num(S, 1);
  vl_push_bool(S, isfinite(x) != 0);
  return 1;
}
static int vm_isinf(VL_State *S) {
  double x = vm_check_num(S, 1);
  vl_push_bool(S, isinf(x) != 0);
  return 1;
}
static int vm_isnan(VL_State *S) {
  double x = vm_check_num(S, 1);
  vl_push_bool(S, isnan(x) != 0);
  return 1;
}

static int vm_sign(VL_State *S) {
  double x = vm_check_num(S, 1);
  int s = (x > 0) - (x < 0);
  vl_push_int(S, (int64_t)s);
  return 1;
}

static int vm_clamp(VL_State *S) {
  double x = vm_check_num(S, 1);
  double lo = vm_check_num(S, 2);
  double hi = vm_check_num(S, 3);
  if (lo > hi) {
    double t = lo;
    lo = hi;
    hi = t;
  }
  if (x < lo)
    x = lo;
  else if (x > hi)
    x = hi;
  vl_push_float(S, x);
  return 1;
}

static int vm_lerp(VL_State *S) {
  double a = vm_check_num(S, 1);
  double b = vm_check_num(S, 2);
  double t = vm_check_num(S, 3);
  vl_push_float(S, a + (b - a) * t);
  return 1;
}

// ---------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------

static int vm_randomseed(VL_State *S) {
  uint64_t seed = (uint64_t)vm_check_int(S, 1);
  if (seed == 0) seed = 0xD1B54A32D192ED03ULL;  // avoid zero state
  g_rng = seed;
  vl_push_bool(S, 1);
  return 1;
}

static int vm_random(VL_State *S) {
  int n = vl_gettop(S);
  if (n <= 0) {
    vl_push_float(S, rng_uniform01());
    return 1;
  }
  if (n == 1) {
    int64_t m = vm_check_int(S, 1);
    if (m <= 0) {
      vl_push_nil(S);
      vl_push_string(S, "ERANGE");
      return 2;
    }
    // uniform int in [1, m]
    double u = rng_uniform01();
    int64_t r = 1 + (int64_t)((double)m * u);
    if (r > m) r = m;
    vl_push_int(S, r);
    return 1;
  }
  int64_t a = vm_check_int(S, 1);
  int64_t b = vm_check_int(S, 2);
  if (a > b) {
    int64_t t = a;
    a = b;
    b = t;
  }
  uint64_t span = (uint64_t)(b - a + 1);
  // map uniform to range
  uint64_t r = (uint64_t)(rng_uniform01() * (double)span);
  if (r >= span) r = span - 1;
  vl_push_int(S, (int64_t)(a + (int64_t)r));
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

static const VL_Reg mathlib[] = {{"sin", vm_sin},
                                 {"cos", vm_cos},
                                 {"tan", vm_tan},
                                 {"asin", vm_asin},
                                 {"acos", vm_acos},
                                 {"atan", vm_atan},
                                 {"atan2", vm_atan2},

                                 {"sinh", vm_sinh},
                                 {"cosh", vm_cosh},
                                 {"tanh", vm_tanh},
                                 {"asinh", vm_asinh},
                                 {"acosh", vm_acosh},
                                 {"atanh", vm_atanh},

                                 {"exp", vm_exp},
                                 {"exp2", vm_exp2},
                                 {"log", vm_log},
                                 {"log10", vm_log10},
                                 {"log2", vm_log2},
                                 {"pow", vm_pow},
                                 {"sqrt", vm_sqrt},
                                 {"cbrt", vm_cbrt},

                                 {"floor", vm_floor},
                                 {"ceil", vm_ceil},
                                 {"trunc", vm_trunc},
                                 {"round", vm_round},
                                 {"fmod", vm_fmod},
                                 {"hypot", vm_hypot},
                                 {"copysign", vm_copysign},
                                 {"nextafter", vm_nextafter},
                                 {"frexp", vm_frexp},
                                 {"ldexp", vm_ldexp},

                                 {"rad", vm_rad},
                                 {"deg", vm_deg},
                                 {"isfinite", vm_isfinite},
                                 {"isinf", vm_isinf},
                                 {"isnan", vm_isnan},
                                 {"sign", vm_sign},
                                 {"clamp", vm_clamp},
                                 {"lerp", vm_lerp},

                                 {"random", vm_random},
                                 {"randomseed", vm_randomseed},

                                 {NULL, NULL}};

void vl_open_mathlib(VL_State *S) { vl_register_lib(S, "math", mathlib); }
