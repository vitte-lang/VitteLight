// SPDX-License-Identifier: MIT
/* ============================================================================
   core/bitset.h — En-tête C11 pour bitset dynamique (Vitte/Vitl)
   Public API, compatible C/C++.
   ============================================================================
 */
#ifndef CORE_BITSET_H
#define CORE_BITSET_H

#if defined(_MSC_VER)
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#endif

#include <stddef.h>   /* size_t, ptrdiff_t */
#include <stdint.h>   /* uint64_t          */
#include <stdbool.h>  /* bool              */

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
/* Types de base minimaux (si api.h non inclus)                               */
/* -------------------------------------------------------------------------- */
#ifndef VL_TYPES_MIN
#define VL_TYPES_MIN 1
typedef uint64_t u64;
typedef size_t   usize;
typedef ptrdiff_t isize;
#endif

/* -------------------------------------------------------------------------- */
/* Structures                                                                 */
/* -------------------------------------------------------------------------- */
typedef struct {
  u64*  words;   /* tableau de mots 64-bit */
  usize nwords;  /* nombre de mots alloués */
  usize nbits;   /* nombre total de bits suivis */
} BitSet;

typedef struct {
  const BitSet* bs;
  usize         idx;   /* position de départ pour l’itération */
} BitSetIter;

/* -------------------------------------------------------------------------- */
/* API                                                                         */
/* -------------------------------------------------------------------------- */
/* Création / destruction / redimensionnement */
API_EXPORT BitSet bitset_new(usize nbits);
API_EXPORT void   bitset_free(BitSet* bs);
API_EXPORT void   bitset_resize(BitSet* bs, usize nbits, bool clear_new);

/* Remplissage */
API_EXPORT void   bitset_zero(BitSet* bs); /* met tous les bits à 0 */
API_EXPORT void   bitset_fill(BitSet* bs); /* met tous les bits à 1 (tronque le dernier mot) */

/* Opérations élémentaires */
API_EXPORT void   bitset_set  (BitSet* bs, usize i);
API_EXPORT void   bitset_clear(BitSet* bs, usize i);
API_EXPORT void   bitset_flip (BitSet* bs, usize i);
API_EXPORT bool   bitset_test (const BitSet* bs, usize i);

/* Agrégats */
API_EXPORT usize  bitset_count(const BitSet* bs);  /* popcount total */

/* Recherche séquentielle
   Retourne l’indice du prochain bit à 1/0 ≥ from, ou -1 si introuvable. */
API_EXPORT isize  bitset_next_set  (const BitSet* bs, usize from);
API_EXPORT isize  bitset_next_clear(const BitSet* bs, usize from);

/* Itérateur simple sur bits à 1 */
API_EXPORT void   bitset_iter_init(BitSetIter* it, const BitSet* bs);
API_EXPORT bool   bitset_iter_next(BitSetIter* it, usize* out_idx);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_BITSET_H */
