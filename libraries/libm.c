// SPDX-License-Identifier: MIT
// VitteLight — libm bridge (ultra complet)
// Fichier: libraries/libm.c
// Objectif: exposer une API C plate et stable (double) couvrant les fonctions
//           math C99 usuelles + utilitaires (deg/rad, clamp, lerp, smoothstep,
//           wrap, nextafter, frexp/modf helpers), adaptée au FFI Vitte‑Light.
//
// ✔ Portabilité: POSIX, Linux, macOS, *BSD, Windows (MSVC/MinGW/Clang).
// ✔ Convention: préfixe vl_m_* ; doubles uniquement pour simplifier le FFI.
// ✔ Export: table {name, fnptr} via vl_m_function_table().
// ✔ Option: capture des exceptions IEEE-754 via fenv (VL_LIBM_USE_FENV).
//
// Build (Unix):
//   cc -std=c11 -O2 -fPIC libraries/libm.c -lm -shared -o libvllibm.so
// Build (Windows MSVC):
//   cl /std:c11 /O2 /LD libraries\libm.c
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN 1
  #include <windows.h>
  #define DLLEXPORT __declspec(dllexport)
#else
  #define DLLEXPORT __attribute__((visibility("default")))
#endif

#include <math.h>
#include <float.h>
#include <stdint.h>
#include <string.h>

#ifdef VL_LIBM_USE_FENV
  #include <fenv.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Constantes (double)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef VL_PI
#define VL_PI 3.14159265358979323846264338327950288419716939937510
#endif
#ifndef VL_TAU
#define VL_TAU (2.0*VL_PI)
#endif
#ifndef VL_E
#define VL_E 2.71828182845904523536028747135266249775724709369996
#endif

DLLEXPORT double vl_m_pi(void){ return VL_PI; }
DLLEXPORT double vl_m_tau(void){ return VL_TAU; }
DLLEXPORT double vl_m_e(void){ return VL_E; }
DLLEXPORT double vl_m_epsilon(void){ return DBL_EPSILON; }
DLLEXPORT double vl_m_inf(void){ return INFINITY; }
DLLEXPORT double vl_m_nan(void){ return NAN; }

// ─────────────────────────────────────────────────────────────────────────────
// IEEE754 classification helpers
// ─────────────────────────────────────────────────────────────────────────────
DLLEXPORT int vl_m_isnan(double x){ return isnan(x)!=0; }
DLLEXPORT int vl_m_isinf(double x){ return isinf(x)!=0; }
DLLEXPORT int vl_m_isfinite(double x){ return isfinite(x)!=0; }
DLLEXPORT int vl_m_signbit(double x){ return signbit(x)!=0; }

// ─────────────────────────────────────────────────────────────────────────────
// Degrés ⟷ Radians et utilitaires scalaires
// ─────────────────────────────────────────────────────────────────────────────
DLLEXPORT double vl_m_rad2deg(double r){ return r * (180.0 / VL_PI); }
DLLEXPORT double vl_m_deg2rad(double d){ return d * (VL_PI / 180.0); }
DLLEXPORT double vl_m_clamp(double x, double a, double b){ return fmax(a, fmin(b, x)); }
DLLEXPORT double vl_m_saturate(double x){ return vl_m_clamp(x, 0.0, 1.0); }
DLLEXPORT double vl_m_lerp(double a, double b, double t){ return fma(t, b - a, a); }
DLLEXPORT double vl_m_remap(double x, double in_min, double in_max, double out_min, double out_max){
  double t = (x - in_min) / (in_max - in_min);
  return vl_m_lerp(out_min, out_max, t);
}
DLLEXPORT double vl_m_smoothstep(double e0, double e1, double x){
  double t = vl_m_saturate((x - e0) / (e1 - e0));
  return t * t * (3.0 - 2.0 * t);
}
DLLEXPORT double vl_m_wrap(double x, double lo, double hi){
  double w = hi - lo; if(w==0.0) return lo;
  double y = fmod(x - lo, w); if(y < 0.0) y += w; return lo + y;
}
DLLEXPORT double vl_m_wrap_rad(double r){ return vl_m_wrap(r, 0.0, VL_TAU); }

// ─────────────────────────────────────────────────────────────────────────────
// Fonctions C99 standard (double)
// ─────────────────────────────────────────────────────────────────────────────
// Trig
DLLEXPORT double vl_m_sin(double x){ return sin(x); }
DLLEXPORT double vl_m_cos(double x){ return cos(x); }
DLLEXPORT double vl_m_tan(double x){ return tan(x); }
DLLEXPORT double vl_m_asin(double x){ return asin(x); }
DLLEXPORT double vl_m_acos(double x){ return acos(x); }
DLLEXPORT double vl_m_atan(double x){ return atan(x); }
DLLEXPORT double vl_m_atan2(double y, double x){ return atan2(y,x); }

// Hyperboliques
DLLEXPORT double vl_m_sinh(double x){ return sinh(x); }
DLLEXPORT double vl_m_cosh(double x){ return cosh(x); }
DLLEXPORT double vl_m_tanh(double x){ return tanh(x); }
DLLEXPORT double vl_m_asinh(double x){ return asinh(x); }
DLLEXPORT double vl_m_acosh(double x){ return acosh(x); }
DLLEXPORT double vl_m_atanh(double x){ return atanh(x); }

// Exponentielles / Logs
DLLEXPORT double vl_m_exp(double x){ return exp(x); }
DLLEXPORT double vl_m_exp2(double x){ return exp2(x); }
DLLEXPORT double vl_m_expm1(double x){ return expm1(x); }
DLLEXPORT double vl_m_log(double x){ return log(x); }
DLLEXPORT double vl_m_log10(double x){ return log10(x); }
DLLEXPORT double vl_m_log2(double x){ return log2(x); }
DLLEXPORT double vl_m_log1p(double x){ return log1p(x); }

// Puissances / Racines / Distances
DLLEXPORT double vl_m_pow(double x, double y){ return pow(x,y); }
DLLEXPORT double vl_m_sqrt(double x){ return sqrt(x); }
DLLEXPORT double vl_m_cbrt(double x){ return cbrt(x); }
DLLEXPORT double vl_m_hypot(double x, double y){ return hypot(x,y); }

// Arrondi / fractionnaires
DLLEXPORT double vl_m_floor(double x){ return floor(x); }
DLLEXPORT double vl_m_ceil(double x){ return ceil(x); }
DLLEXPORT double vl_m_trunc(double x){ return trunc(x); }
DLLEXPORT double vl_m_round(double x){ return round(x); }
DLLEXPORT long   vl_m_lround(double x){ return lround(x); }
DLLEXPORT long long vl_m_llround(double x){ return llround(x); }
DLLEXPORT double vl_m_rint(double x){ return rint(x); }
DLLEXPORT double vl_m_nearbyint(double x){ return nearbyint(x); }

DLLEXPORT double vl_m_modf(double x, double* iptr){ return modf(x, iptr); }
DLLEXPORT double vl_m_fmod(double x, double y){ return fmod(x,y); }
DLLEXPORT double vl_m_remainder(double x, double y){ return remainder(x,y); }

// Bits flottants
DLLEXPORT double vl_m_copysign(double x, double y){ return copysign(x,y); }
DLLEXPORT double vl_m_frexp(double x, int* exp){ return frexp(x, exp); }
DLLEXPORT double vl_m_ldexp(double x, int exp){ return ldexp(x, exp); }
DLLEXPORT int    vl_m_ilogb(double x){ return ilogb(x); }
DLLEXPORT double vl_m_logb(double x){ return logb(x); }
DLLEXPORT double vl_m_scalbn(double x, int n){ return scalbn(x, n); }
DLLEXPORT double vl_m_nextafter(double x, double y){ return nextafter(x, y); }
#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 199901L
DLLEXPORT double vl_m_nexttoward(double x, long double y){ return nexttoward(x, y); }
#else
DLLEXPORT double vl_m_nexttoward(double x, long double y){ (void)y; return nextafter(x, (y>0)?INFINITY:-INFINITY); }
#endif
#endif

// Combinaisons / minmax
DLLEXPORT double vl_m_fabs(double x){ return fabs(x); }
DLLEXPORT double vl_m_fdim(double x, double y){ return fdim(x,y); }
DLLEXPORT double vl_m_fmax(double x, double y){ return fmax(x,y); }
DLLEXPORT double vl_m_fmin(double x, double y){ return fmin(x,y); }
DLLEXPORT double vl_m_fma(double x, double y, double z){ return fma(x,y,z); }

// Fonctions spéciales
DLLEXPORT double vl_m_erf(double x){ return erf(x); }
DLLEXPORT double vl_m_erfc(double x){ return erfc(x); }
DLLEXPORT double vl_m_tgamma(double x){ return tgamma(x); }
DLLEXPORT double vl_m_lgamma(double x){ return lgamma(x); }

// Géométrie simple
DLLEXPORT double vl_m_length2(double x, double y){ return hypot(x,y); }
DLLEXPORT double vl_m_length3(double x, double y, double z){ return sqrt(x*x + y*y + z*z); }
DLLEXPORT double vl_m_dot2(double ax, double ay, double bx, double by){ return ax*bx + ay*by; }
DLLEXPORT double vl_m_dot3(double ax, double ay, double az, double bx, double by, double bz){ return ax*bx + ay*by + az*bz; }

// Normalisation scalaire (renvoie facteur 1/len pour appliquer côté VM)
DLLEXPORT double vl_m_invlen2(double x, double y){ double l = hypot(x,y); return l>0.0 ? 1.0/l : 0.0; }
DLLEXPORT double vl_m_invlen3(double x, double y, double z){ double l = sqrt(x*x+y*y+z*z); return l>0.0 ? 1.0/l : 0.0; }

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions IEEE-754 (optionnel)
// ─────────────────────────────────────────────────────────────────────────────
// Bitmask retourné par vl_m_test_excepts():
//   1=FE_INVALID, 2=FE_DIVBYZERO, 4=FE_OVERFLOW, 8=FE_UNDERFLOW, 16=FE_INEXACT
DLLEXPORT void vl_m_clear_excepts(void){
#ifdef VL_LIBM_USE_FENV
  feclearexcept(FE_ALL_EXCEPT);
#endif
}

DLLEXPORT int vl_m_test_excepts(void){
#ifdef VL_LIBM_USE_FENV
  int m = fetestexcept(FE_ALL_EXCEPT);
  int r = 0;
  if(m & FE_INVALID)   r |= 1;
  if(m & FE_DIVBYZERO) r |= 2;
  if(m & FE_OVERFLOW)  r |= 4;
  if(m & FE_UNDERFLOW) r |= 8;
  if(m & FE_INEXACT)   r |= 16;
  return r;
#else
  return 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Table d'export
// ─────────────────────────────────────────────────────────────────────────────

typedef struct { const char* name; void* fn; } VL_Fn;
#define FN(n) { #n, (void*)&n }
static const VL_Fn g_fn_table[] = {
  // Constantes / tests
  FN(vl_m_pi), FN(vl_m_tau), FN(vl_m_e), FN(vl_m_epsilon), FN(vl_m_inf), FN(vl_m_nan),
  FN(vl_m_isnan), FN(vl_m_isinf), FN(vl_m_isfinite), FN(vl_m_signbit),
  // Utilitaires
  FN(vl_m_rad2deg), FN(vl_m_deg2rad), FN(vl_m_clamp), FN(vl_m_saturate), FN(vl_m_lerp), FN(vl_m_remap), FN(vl_m_smoothstep), FN(vl_m_wrap), FN(vl_m_wrap_rad),
  // Trig
  FN(vl_m_sin), FN(vl_m_cos), FN(vl_m_tan), FN(vl_m_asin), FN(vl_m_acos), FN(vl_m_atan), FN(vl_m_atan2),
  // Hyperboliques
  FN(vl_m_sinh), FN(vl_m_cosh), FN(vl_m_tanh), FN(vl_m_asinh), FN(vl_m_acosh), FN(vl_m_atanh),
  // Exp/Log
  FN(vl_m_exp), FN(vl_m_exp2), FN(vl_m_expm1), FN(vl_m_log), FN(vl_m_log10), FN(vl_m_log2), FN(vl_m_log1p),
  // Puissances / racines
  FN(vl_m_pow), FN(vl_m_sqrt), FN(vl_m_cbrt), FN(vl_m_hypot),
  // Arrondi
  FN(vl_m_floor), FN(vl_m_ceil), FN(vl_m_trunc), FN(vl_m_round), FN(vl_m_lround), FN(vl_m_llround), FN(vl_m_rint), FN(vl_m_nearbyint),
  // Reste / fraction
  FN(vl_m_modf), FN(vl_m_fmod), FN(vl_m_remainder),
  // Bits flottants
  FN(vl_m_copysign), FN(vl_m_frexp), FN(vl_m_ldexp), FN(vl_m_ilogb), FN(vl_m_logb), FN(vl_m_scalbn), FN(vl_m_nextafter), FN(vl_m_nexttoward),
  // Min/Max/FMA
  FN(vl_m_fabs), FN(vl_m_fdim), FN(vl_m_fmax), FN(vl_m_fmin), FN(vl_m_fma),
  // Spéciales
  FN(vl_m_erf), FN(vl_m_erfc), FN(vl_m_tgamma), FN(vl_m_lgamma),
  // Géométrie
  FN(vl_m_length2), FN(vl_m_length3), FN(vl_m_dot2), FN(vl_m_dot3), FN(vl_m_invlen2), FN(vl_m_invlen3),
  // Exceptions
  FN(vl_m_clear_excepts), FN(vl_m_test_excepts),
};
#undef FN

DLLEXPORT const VL_Fn* vl_m_function_table(size_t* count){
  if(count) *count = sizeof(g_fn_table)/sizeof(g_fn_table[0]);
  return g_fn_table;
}

// ─────────────────────────────────────────────────────────────────────────────
// Notes d'intégration Vitte‑Light
// ─────────────────────────────────────────────────────────────────────────────
// • Types FFI recommandés: double pour tous les scalaires, int pour flags/tests.
// • Les fonctions ne modifient pas errno ; utilisez VL_LIBM_USE_FENV pour un
//   retour d'état numérique via vl_m_test_excepts().
// • Côté VITL, mappez les prototypes extern "C" 1:1, ex.:
//     extern "C" fun m.sin(x: f64)->f64 (dl: "vl_m_sin")
//     extern "C" fun m.modf(x: f64, iptr: *f64)->f64 (dl: "vl_m_modf")
// • Sous Unix, liez avec -lm.
// • Sous Windows/MSVC, aucune libm dédiée n'est nécessaire.
