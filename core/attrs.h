// SPDX-License-Identifier: MIT
/* ============================================================================
   core/attrs.h — En-tête C11 pour utilitaires d’attributs et plateforme
   Public API, compatible C/C++.
   ============================================================================
 */
#ifndef CORE_ATTRS_H
#define CORE_ATTRS_H

#if defined(_MSC_VER)
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#endif

#include <stddef.h>  /* size_t, max_align_t */
#include <stdbool.h> /* bool                */
#include <stdint.h>  /* uint*_t             */

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
/* Attributs et hints (macros portables)                                      */
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

#if !defined(ATTR_ASSUME)
# if defined(__clang__) || defined(__GNUC__)
#  define ATTR_ASSUME(expr) do { if (!(expr)) __builtin_unreachable(); } while (0)
# elif defined(_MSC_VER)
#  define ATTR_ASSUME(expr) __assume(expr)
# else
#  define ATTR_ASSUME(expr) do { (void)0; } while (0)
# endif
#endif

#if !defined(ATTR_LIKELY)
# if defined(__GNUC__) || defined(__clang__)
#  define ATTR_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define ATTR_UNLIKELY(x) __builtin_expect(!!(x), 0)
# else
#  define ATTR_LIKELY(x)   (x)
#  define ATTR_UNLIKELY(x) (x)
# endif
#endif

#if !defined(ATTR_PREFETCH_R)
# if defined(__GNUC__) || defined(__clang__)
#  define ATTR_PREFETCH_R(p, l) __builtin_prefetch((p), 0, (l))
#  define ATTR_PREFETCH_W(p, l) __builtin_prefetch((p), 1, (l))
# elif defined(_MSC_VER)
#  include <mmintrin.h>
#  define ATTR_PREFETCH_R(p, l) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#  define ATTR_PREFETCH_W(p, l) do { (void)(p); (void)(l); } while (0)
# else
#  define ATTR_PREFETCH_R(p, l) do { (void)(p); (void)(l); } while (0)
#  define ATTR_PREFETCH_W(p, l) do { (void)(p); (void)(l); } while (0)
# endif
#endif

/* -------------------------------------------------------------------------- */
/* Structures de capacités et d’infos build                                   */
/* -------------------------------------------------------------------------- */
typedef struct {
  /* lang/cc */
  unsigned c11:1;
  unsigned gnu:1;
  unsigned msvc:1;
  unsigned threads:1;
  unsigned builtin_expect:1;
  unsigned builtin_unreachable:1;
  unsigned builtin_prefetch:1;

  /* cpu features best-effort */
  unsigned sse2:1;
  unsigned sse4_2:1;
  unsigned avx2:1;
  unsigned neon:1;
} VlAttrCaps;

typedef struct {
  char   compiler[64];
  char   os[32];
  char   arch[32];
  size_t cacheline;
  VlAttrCaps caps;
} VlBuildInfo;

/* -------------------------------------------------------------------------- */
/* API                                                                         */
/* -------------------------------------------------------------------------- */
/* Identité de build */
API_EXPORT const char* vl_compiler(void);
API_EXPORT const char* vl_os(void);
API_EXPORT const char* vl_arch(void);

/* Capacités et caractéristiques système */
API_EXPORT VlAttrCaps vl_attr_caps(void);
API_EXPORT size_t     vl_cacheline_size(void);

/* Hints CPU */
API_EXPORT void vl_prefetch_ro(const void* p, int locality /*0..3*/);
API_EXPORT void vl_prefetch_rw(const void* p, int locality /*0..3*/);

/* Contrats */
API_EXPORT ATTR_NORETURN void vl_unreachable(const char* why);
API_EXPORT void vl_assume(bool cond);

/* Align/arith helpers */
API_EXPORT size_t vl_align_up(size_t x, size_t a);
API_EXPORT bool   vl_is_pow2(size_t x);

/* Build info agrégée */
API_EXPORT void vl_build_info(VlBuildInfo* out);

/* Stats optionnelles: aucune ici (spécifiques à arena.c si ARENA_STATS=1)   */

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_ATTRS_H */
