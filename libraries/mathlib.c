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
//   Decompose:      frexp, ldexp
//   Conversions:    rad→deg, deg→rad
//   Predicates:     isfinite, isinf, isnan, sign → -1|0|1
//   Helpers:        clamp, lerp, min, max
//   Constants:      pi, tau, e, inf, nan
//   Random:         random([max])
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c mathlib.c
//
// Notes:
//   - API minimaliste, wrappers C standard + utilitaires.
//   - Compatible VM Vitte Light. Expose fonctions via en-tête.

#include <math.h>
#include <float.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

// --- Trigonometry ---
double math_sin(double x)    { return sin(x); }
double math_cos(double x)    { return cos(x); }
double math_tan(double x)    { return tan(x); }
double math_asin(double x)   { return asin(x); }
double math_acos(double x)   { return acos(x); }
double math_atan(double x)   { return atan(x); }
double math_atan2(double y, double x) { return atan2(y, x); }

// --- Hyperbolic ---
double math_sinh(double x)   { return sinh(x); }
double math_cosh(double x)   { return cosh(x); }
double math_tanh(double x)   { return tanh(x); }
double math_asinh(double x)  { return asinh(x); }
double math_acosh(double x)  { return acosh(x); }
double math_atanh(double x)  { return atanh(x); }

// --- Exponential / Log ---
double math_exp(double x)    { return exp(x); }
double math_exp2(double x)   { return exp2(x); }
double math_log(double x)    { return log(x); }
double math_log10(double x)  { return log10(x); }
double math_log2(double x)   { return log2(x); }
double math_pow(double x, double y) { return pow(x, y); }
double math_sqrt(double x)   { return sqrt(x); }
double math_cbrt(double x)   { return cbrt(x); }

// --- Rounding ---
double math_floor(double x)  { return floor(x); }
double math_ceil(double x)   { return ceil(x); }
double math_trunc(double x)  { return trunc(x); }
double math_round(double x)  { return round(x); }

// --- Arithmetic ---
double math_fmod(double x, double y)      { return fmod(x, y); }
double math_hypot(double x, double y)     { return hypot(x, y); }
double math_copysign(double x, double y)  { return copysign(x, y); }
double math_nextafter(double x, double y) { return nextafter(x, y); }

// --- Decompose ---
double math_frexp(double x, int *exp)     { return frexp(x, exp); }
double math_ldexp(double x, int exp)      { return ldexp(x, exp); }

// --- Conversion ---
double math_rad(double deg) { return deg * (M_PI / 180.0); }
double math_deg(double rad) { return rad * (180.0 / M_PI); }

// --- Predicates ---
int math_isfinite(double x) { return isfinite(x); }
int math_isinf(double x)    { return isinf(x); }
int math_isnan(double x)    { return isnan(x); }
int math_sign(double x) {
    return (x > 0) - (x < 0);
}

// --- Helpers ---
double math_clamp(double x, double lo, double hi) {
    return (x < lo) ? lo : (x > hi) ? hi : x;
}
double math_lerp(double a, double b, double t) {
    return a + (b - a) * t;
}
double math_min(double a, double b) { return (a < b) ? a : b; }
double math_max(double a, double b) { return (a > b) ? a : b; }

// --- Constants ---
double math_pi(void)  { return M_PI; }
double math_tau(void) { return 2.0 * M_PI; }
double math_e(void)   { return M_E; }
double math_inf(void) { return INFINITY; }
double math_nan(void) { return NAN; }

// --- Random ---
static bool rng_init = false;
static void ensure_rng(void) {
    if (!rng_init) {
        srand((unsigned) time(NULL));
        rng_init = true;
    }
}
double math_random(void) {
    ensure_rng();
    return (double) rand() / (double) RAND_MAX;
}
long math_random_range(long max) {
    ensure_rng();
    if (max <= 0) return 0;
    return rand() % max;
}

// --- Demo ---
#ifdef MATHLIB_DEMO
#include <stdio.h>
int main(void) {
    printf("pi=%.10f tau=%.10f e=%.10f\n", math_pi(), math_tau(), math_e());
    printf("deg(π)=%.2f rad(180)=%.2f\n", math_deg(M_PI), math_rad(180.0));
    printf("sin(π/2)=%.3f cos(π)=%.3f\n", math_sin(M_PI/2), math_cos(M_PI));
    printf("clamp(5,0,3)=%.1f\n", math_clamp(5,0,3));
    printf("rand=%.3f rand[10]=%ld\n", math_random(), math_random_range(10));
    return 0;
}
#endif