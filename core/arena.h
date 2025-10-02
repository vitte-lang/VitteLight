// SPDX-License-Identifier: MIT
/* ============================================================================
   core/arena.h — En-tête C11 pour l’allocateur « bump » Arena
   Public API, compatible C/C++.
   ============================================================================
 */
#ifndef CORE_ARENA_H
#define CORE_ARENA_H

#if defined(_MSC_VER)
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#endif

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
/* Types de base minimaux (si non fournis par api.h)                          */
/* -------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef VL_TYPES_MIN
#define VL_TYPES_MIN 1
typedef uint8_t  u8;
typedef size_t   usize;
#endif

/* -------------------------------------------------------------------------- */
/* Arena                                                                      */
/* -------------------------------------------------------------------------- */
typedef struct Arena {
  u8*   base;
  usize cap;
  usize off;
} Arena;

/* Création / destruction */
API_EXPORT Arena arena_new(usize cap);  /* cap==0 → valeur par défaut */
API_EXPORT void  arena_free(Arena* a);

/* Réinitialisation et allocation */
API_EXPORT void  arena_reset(Arena* a);
API_EXPORT void* arena_alloc(Arena* a, usize n, usize align); /* align=0 → _Alignof(max_align_t) */
API_EXPORT char* arena_strdup(Arena* a, const char* s);

/* Diagnostics utilitaires */
API_EXPORT usize arena_capacity(const Arena* a);
API_EXPORT usize arena_offset  (const Arena* a);
API_EXPORT bool  arena_valid   (const Arena* a);

/* Stats optionnelles (exposées si compilé avec ARENA_STATS=1) */
#if defined(ARENA_STATS) && ARENA_STATS
API_EXPORT void arena_stats_get(usize* peak, usize* total, usize* calls, usize* resets);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_ARENA_H */
