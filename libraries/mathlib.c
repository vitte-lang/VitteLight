// SPDX-License-Identifier: GPL-3.0-or-later
//
// mathlib.c — Vitte Light math standard library (C17, portable)
// Namespace: "math"
//
// Coverage:
//   Trigonometry:   sin, cos, tan, asin, acos, atan, atan2
//   Hyperbolic:     sinh, cosh, tanh, asinh, acosh, atanh
//   Exponentials:   exp, exp2, log, log10, log2, pow, sqrt, cbrt
//   Rounding:       floor, ceil, trunc, round
//   Arithmetic:     fmod, hypot, copysign, nextafter
//   Decompose:      frexp -> mantissa, exp;  ldexp(x, exp)
//   Conversions:    rad(deg), deg(rad)
//   Predicates:     isfinite, isinf, isnan, sign -> -1|0|1
//   Helpers:        clamp(x, lo, hi), lerp(a,b,t), min(a,b), max(a,b)
//   Constants:      pi(), tau(), e(), inf(), nan()
//   Random:         random([m[, n]]), randomseed(seed)
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
#ifndef M_E
#define M_E 2.718281828459045235360287471352662498L
#endif

// ---------------------------------------------------------------------
// VM helpers
// ---------------------------------------------------------------------

static double m_check_num(VL_State *S, int idx) {
  VL_Value *v = vl_get(S, idx);
  if (!v) {
    vl_errorf(S, "argument #%d: number expected", idx);
    return vl_error(S);
  }
  return vl_tonumber(S, v);
}

static int64_t m_check_int(VL_State *S, int idx) {
  VL_Value *v = vl_get(S, idx);
  if (!v || !(vl_isint(S, idx) || vl_isfloat(S, idx))) {
    vl_errorf(S, "argument #%d: integer expected", idx);
    return vl_error(S);
  }
  return vl_isint(S, idx) ? vl_toint(S, v) : (int64_t)vl_tonumber(S, v);
}

static double m_opt_num(VL_State *S, int idx, double defv) {
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

static void rng_seed_if_needed(void) {
  if (g_rng == 0) {
    uint64_t v = 0;
    if (aux_rand_bytes(&v, sizeof v) != AUX_OK || v == 0) {
      v = aux_now_nanos() ^ 0x9E3779B97F4A7C15ULL;
    }
    if (v == 0) v = 0xD1B54A32D192ED03ULL;
    g_rng = v;
  }
}

static double rng_u01(void) {
  rng_seed_if_needed();
  uint64_t r = xorshift64s(&g_rng);
  // 53-bit to double in [0,1)
  return (double)((r >> 11) & ((1ULL << 53) - 1)) * (1.0 / 9007199254740992.0);
}

// ---------------------------------------------------------------------
// Macros for thin wrappers
// ---------------------------------------------------------------------

#define M_UN(name, op)                \
  static int vm_##name(VL_State *S) { \
    double x = m_check_num(S, 1);     \
    vl_push_float(S, op(x));          \
    return 1;                         \
  }

#define M_BIN(name, op)               \
  static int vm_##name(VL_State *S) { \
    double a = m_check_num(S, 1);     \
    double b = m_check_num(S, 2);     \
    vl_push_float(S, op(a, b));       \
    return 1;                         \
  }

// ---------------------------------------------------------------------
// Trigonometry / Hyperbolic
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
// Rounding / Arithmetic / Decompose
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
  int e = 0;
  double x = m_check_num(S, 1);
  double m = frexp(x, &e);
  vl_push_float(S, m);
  vl_push_int(S, (int64_t)e);
  return 2;
}

static int vm_ldexp(VL_State *S) {
  double x = m_check_num(S, 1);
  int e = (int)m_check_int(S, 2);
  vl_push_float(S, ldexp(x, e));
  return 1;
}

// ---------------------------------------------------------------------
// Conversions / Predicates / Small helpers
// ---------------------------------------------------------------------

static int vm_rad(VL_State *S) {
  double d = m_check_num(S, 1);
  vl_push_float(S, d * (double)(M_PI / 180.0));
  return 1;
}
static int vm_deg(VL_State *S) {
  double r = m_check_num(S, 1);
  vl_push_float(S, r * (double)(180.0 / M_PI));
  return 1;
}

static int vm_isfinite(VL_State *S) {
  double x = m_check_num(S, 1);
  vl_push_bool(S, isfinite(x) != 0);
  return 1;
}
static int vm_isinf(VL_State *S) {
  double x = m_check_num(S, 1);
  vl_push_bool(S, isinf(x) != 0);
  return 1;
}
static int vm_isnan(VL_State *S) {
  double x = m_check_num(S, 1);
  vl_push_bool(S, isnan(x) != 0);
  return 1;
}

static int vm_sign(VL_State *S) {
  double x = m_check_num(S, 1);
  int s = (x > 0) - (x < 0);
  vl_push_int(S, (int64_t)s);
  return 1;
}

static int vm_clamp(VL_State *S) {
  double x = m_check_num(S, 1);
  double lo = m_check_num(S, 2);
  double hi = m_check_num(S, 3);
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
  double a = m_check_num(S, 1);
  double b = m_check_num(S, 2);
  double t = m_check_num(S, 3);
  vl_push_float(S, a + (b - a) * t);
  return 1;
}

static int vm_min(VL_State *S) {
  double a = m_check_num(S, 1), b = m_check_num(S, 2);
  vl_push_float(S, a < b ? a : b);
  return 1;
}

static int vm_max(VL_State *S) {
  double a = m_check_num(S, 1), b = m_check_num(S, 2);
  vl_push_float(S, a > b ? a : b);
  return 1;
}

// ---------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------

static int vm_pi(VL_State *S) {
  vl_push_float(S, (double)M_PI);
  return 1;
}
static int vm_tau(VL_State *S) {
  vl_push_float(S, (double)(2.0 * M_PI));
  return 1;
}
static int vm_e(VL_State *S) {
  vl_push_float(S, (double)M_E);
  return 1;
}
static int vm_inf(VL_State *S) {
  vl_push_float(S, (double)INFINITY);
  return 1;
}
static int vm_nan(VL_State *S) {
  volatile double z = 0.0;
  vl_push_float(S, 0.0 / z);
  return 1;
}

// ---------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------

static int vm_randomseed(VL_State *S) {
  uint64_t seed = (uint64_t)m_check_int(S, 1);
  if (seed == 0) seed = 0xD1B54A32D192ED03ULL;
  g_rng = seed;
  vl_push_bool(S, 1);
  return 1;
}

// random():
//   - no args -> uniform double in [0,1)
//   - one arg m -> int in [1, m]
//   - two args m,n -> int in [m, n]
static int vm_random(VL_State *S) {
  int n = vl_gettop(S);
  if (n <= 0) {
    vl_push_float(S, rng_u01());
    return 1;
  }
  if (n == 1) {
    int64_t m = m_check_int(S, 1);
    if (m <= 0) {
      vl_push_nil(S);
      vl_push_string(S, "ERANGE");
      return 2;
    }
    // unbiased map using rejection sampling
    uint64_t range = (uint64_t)m;
    uint64_t limit = UINT64_MAX - (UINT64_MAX % range);
    uint64_t r;
    do {
      r = xorshift64s(&g_rng);
      rng_seed_if_needed();
    } while (r > limit);
    vl_push_int(S, (int64_t)(1 + (r % range)));
    return 1;
  }
  int64_t a = m_check_int(S, 1), b = m_check_int(S, 2);
  if (a > b) {
    int64_t t = a;
    a = b;
    b = t;
  }
  uint64_t span = (uint64_t)(b - a + 1);
  uint64_t limit = UINT64_MAX - (UINT64_MAX % span);
  uint64_t r;
  do {
    r = xorshift64s(&g_rng);
    rng_seed_if_needed();
  } while (r > limit);
  vl_push_int(S, (int64_t)(a + (r % span)));
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

static const VL_Reg mathlib[] = {
    // Trig
    {"sin", vm_sin},
    {"cos", vm_cos},
    {"tan", vm_tan},
    {"asin", vm_asin},
    {"acos", vm_acos},
    {"atan", vm_atan},
    {"atan2", vm_atan2},
    // Hyperbolic
    {"sinh", vm_sinh},
    {"cosh", vm_cosh},
    {"tanh", vm_tanh},
    {"asinh", vm_asinh},
    {"acosh", vm_acosh},
    {"atanh", vm_atanh},
    // Exp/log
    {"exp", vm_exp},
    {"exp2", vm_exp2},
    {"log", vm_log},
    {"log10", vm_log10},
    {"log2", vm_log2},
    {"pow", vm_pow},
    {"sqrt", vm_sqrt},
    {"cbrt", vm_cbrt},
    // Rounding / arithmetic
    {"floor", vm_floor},
    {"ceil", vm_ceil},
    {"trunc", vm_trunc},
    {"round", vm_round},
    {"fmod", vm_fmod},
    {"hypot", vm_hypot},
    {"copysign", vm_copysign},
    {"nextafter", vm_nextafter},
    // Decompose
    {"frexp", vm_frexp},
    {"ldexp", vm_ldexp},
    // Conversions, predicates, helpers
    {"rad", vm_rad},
    {"deg", vm_deg},
    {"isfinite", vm_isfinite},
    {"isinf", vm_isinf},
    {"isnan", vm_isnan},
    {"sign", vm_sign},
    {"clamp", vm_clamp},
    {"lerp", vm_lerp},
    {"min", vm_min},
    {"max", vm_max},
    // Constants
    {"pi", vm_pi},
    {"tau", vm_tau},
    {"e", vm_e},
    {"inf", vm_inf},
    {"nan", vm_nan},
    // Random
    {"random", vm_random},
    {"randomseed", vm_randomseed},
    {NULL, NULL}};

void vl_open_mathlib(VL_State *S) { vl_register_lib(S, "math", mathlib); }
