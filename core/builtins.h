// SPDX-License-Identifier: MIT
/* ============================================================================
   core/builtins.h — En-tête C11 pour primitives "builtins" portables
   Public API, compatible C/C++.
   ============================================================================
 */
#ifndef CORE_BUILTINS_H
#define CORE_BUILTINS_H

#if defined(_MSC_VER)
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Export                                                                     */
/* -------------------------------------------------------------------------- */
#if !defined(API_EXPORT)
# if defined(_WIN32)
#  define API_EXPORT __declspec(dllexport)
# else
#  if defined(__GNUC__) || defined(__clang__)
#   define API_EXPORT __attribute__((visibility("default")))
#  else
#   define API_EXPORT
#  endif
# endif
#endif

/* -------------------------------------------------------------------------- */
/* Types minimaux (si non fournis par api.h)                                  */
/* -------------------------------------------------------------------------- */
#ifndef VL_TYPES_MIN
#define VL_TYPES_MIN 1
typedef uint64_t u64;
#endif

/* -------------------------------------------------------------------------- */
/* Attributs                                                                   */
/* -------------------------------------------------------------------------- */
#if !defined(ATTR_NORETURN)
# if defined(__GNUC__) || defined(__clang__)
#  define ATTR_NORETURN __attribute__((noreturn))
# elif defined(_MSC_VER)
#  define ATTR_NORETURN __declspec(noreturn)
# else
#  define ATTR_NORETURN
# endif
#endif

/* -------------------------------------------------------------------------- */
/* Hints prédictifs                                                           */
/* -------------------------------------------------------------------------- */
/* Version fonctionnelle (toujours disponible) */
API_EXPORT int vl_likely(int cond);
API_EXPORT int vl_unlikely(int cond);

/* Macros opportunistes mappant vers les builtins du compilateur */
#if defined(__GNUC__) || defined(__clang__)
# define VL_LIKELY(x)   (__builtin_expect(!!(x), 1))
# define VL_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
# define VL_LIKELY(x)   (!!(x))
# define VL_UNLIKELY(x) (!!(x))
#endif

/* -------------------------------------------------------------------------- */
/* Unreachable / assume                                                       */
/* -------------------------------------------------------------------------- */
API_EXPORT ATTR_NORETURN void vl_builtin_unreachable(const char* why);
API_EXPORT void vl_builtin_assume(int cond);

/* Macros opportunistes */
#if defined(__clang__) || defined(__GNUC__)
# define VL_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
# define VL_UNREACHABLE() __assume(0)
#else
# define VL_UNREACHABLE() ((void)0)
#endif

#if defined(_MSC_VER)
# define VL_ASSUME(c) __assume(c)
#elif defined(__clang__) || defined(__GNUC__)
# define VL_ASSUME(c) do { if (!(c)) __builtin_unreachable(); } while(0)
#else
# define VL_ASSUME(c) do { (void)(c); } while(0)
#endif

/* -------------------------------------------------------------------------- */
/* Bit operations 64-bit                                                      */
/* -------------------------------------------------------------------------- */
API_EXPORT unsigned vl_popcount64(u64 x);
API_EXPORT unsigned vl_ctz64(u64 x);  /* count trailing zeros; x==0 → 64 */
API_EXPORT unsigned vl_clz64(u64 x);  /* count leading  zeros; x==0 → 64 */

/* Rotations 64-bit */
API_EXPORT u64 vl_rotl64(u64 x, unsigned r);
API_EXPORT u64 vl_rotr64(u64 x, unsigned r);

/* Macros opportunistes (optionnelles) */
#if defined(__clang__) || defined(__GNUC__)
# define VL_POPCOUNT64(x) ((unsigned)__builtin_popcountll((unsigned long long)(x)))
# define VL_CTZ64(x)      ((x) ? (unsigned)__builtin_ctzll((unsigned long long)(x)) : 64u)
# define VL_CLZ64(x)      ((x) ? (unsigned)__builtin_clzll((unsigned long long)(x)) : 64u)
#else
# define VL_POPCOUNT64(x) vl_popcount64((u64)(x))
# define VL_CTZ64(x)      vl_ctz64((u64)(x))
# define VL_CLZ64(x)      vl_clz64((u64)(x))
#endif

#define VL_ROTL64(x,r) vl_rotl64((u64)(x),(unsigned)(r))
#define VL_ROTR64(x,r) vl_rotr64((u64)(x),(unsigned)(r))

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_BUILTINS_H */
