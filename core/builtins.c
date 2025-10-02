// SPDX-License-Identifier: MIT
/* ============================================================================
   core/builtins.c — Implémentations C11 des "builtins" communs pour Vitte/Vitl
   But : fournir une couche portable si le compilateur n’a pas certaines
         primitives (__builtin_expect, __builtin_popcount, etc.)
   ============================================================================
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#include <intrin.h>
#endif

#include "builtins.h"
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------- */
/* Branch prediction hints                                                    */
/* -------------------------------------------------------------------------- */
API_EXPORT int vl_likely(int cond)   { return cond; }
API_EXPORT int vl_unlikely(int cond) { return cond; }

/* -------------------------------------------------------------------------- */
/* Unreachable / assume                                                       */
/* -------------------------------------------------------------------------- */
API_EXPORT ATTR_NORETURN void vl_builtin_unreachable(const char* why) {
  if (why) fprintf(stderr, "unreachable reached: %s\n", why);
  else     fprintf(stderr, "unreachable reached\n");
  abort();
}

API_EXPORT void vl_builtin_assume(int cond) {
#if defined(__clang__) || defined(__GNUC__)
  if (!cond) __builtin_unreachable();
#elif defined(_MSC_VER)
  __assume(cond);
#else
  (void)cond;
#endif
}

/* -------------------------------------------------------------------------- */
/* Popcount / ctz / clz 64-bit                                                */
/* -------------------------------------------------------------------------- */
API_EXPORT unsigned vl_popcount64(u64 x) {
#if defined(__clang__) || defined(__GNUC__)
  return (unsigned)__builtin_popcountll(x);
#elif defined(_MSC_VER)
  return (unsigned)__popcnt64(x);
#else
  unsigned c = 0;
  while (x) { c += (unsigned)(x & 1ull); x >>= 1; }
  return c;
#endif
}

API_EXPORT unsigned vl_ctz64(u64 x) {
  if (x == 0) return 64;
#if defined(__clang__) || defined(__GNUC__)
  return (unsigned)__builtin_ctzll(x);
#elif defined(_MSC_VER)
  unsigned long r;
  _BitScanForward64(&r, x);
  return (unsigned)r;
#else
  unsigned c = 0;
  while (!(x & 1ull)) { c++; x >>= 1; }
  return c;
#endif
}

API_EXPORT unsigned vl_clz64(u64 x) {
  if (x == 0) return 64;
#if defined(__clang__) || defined(__GNUC__)
  return (unsigned)__builtin_clzll(x);
#elif defined(_MSC_VER)
  unsigned long r;
  _BitScanReverse64(&r, x);
  return (unsigned)(63 - r);
#else
  unsigned c = 0;
  u64 mask = 1ull << 63;
  while (!(x & mask)) { c++; mask >>= 1; }
  return c;
#endif
}

/* -------------------------------------------------------------------------- */
/* Rotation                                                                   */
/* -------------------------------------------------------------------------- */
API_EXPORT u64 vl_rotl64(u64 x, unsigned r) {
  return (x << (r & 63)) | (x >> ((64 - r) & 63));
}
API_EXPORT u64 vl_rotr64(u64 x, unsigned r) {
  return (x >> (r & 63)) | (x << ((64 - r) & 63));
}

/* -------------------------------------------------------------------------- */
/* Fallback demo (optionnel)                                                  */
/* -------------------------------------------------------------------------- */
#ifdef BUILTINS_DEMO_MAIN
int main(void) {
  u64 v = 0x00f0ull;
  printf("popcount(0x%llx)=%u\n", (unsigned long long)v, vl_popcount64(v));
  printf("ctz=%u clz=%u\n", vl_ctz64(v), vl_clz64(v));
  printf("rotl=%llx rotr=%llx\n",
         (unsigned long long)vl_rotl64(v, 4),
         (unsigned long long)vl_rotr64(v, 4));
  return 0;
}
#endif
