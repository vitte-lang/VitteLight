// SPDX-License-Identifier: MIT
/* ============================================================================
   core/bitset.c — Implémentation C11 d’un bitset dynamique pour Vitte/Vitl
   API prévue dans core/bitset.h
   Fonctionnalités :
     - bitset_new / bitset_free
     - bitset_resize
     - bitset_set / bitset_clear / bitset_flip
     - bitset_test
     - bitset_count (popcount)
     - bitset_fill / bitset_zero
     - bitset_next_set / bitset_next_clear
     - itérateur simple
   Dépendances : libc uniquement.
   ============================================================================
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#include <intrin.h>
#endif

#include "bitset.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------- */
/* Helpers internes                                                           */
/* -------------------------------------------------------------------------- */

static inline usize word_index(usize bit) { return bit >> 6; }
static inline u64   bit_mask(usize bit)   { return 1ull << (bit & 63); }

/* nombre de mots nécessaires pour n bits */
static inline usize words_for_bits(usize nbits) {
  return (nbits + 63) / 64;
}

/* popcount portable */
static inline u32 popcount64(u64 x) {
#if defined(__GNUC__) || defined(__clang__)
  return (u32)__builtin_popcountll(x);
#elif defined(_MSC_VER)
  return (u32)__popcnt64(x);
#else
  /* fallback */
  u32 c = 0;
  while (x) { c += (u32)(x & 1ull); x >>= 1; }
  return c;
#endif
}

/* -------------------------------------------------------------------------- */
/* Implémentation                                                             */
/* -------------------------------------------------------------------------- */

API_EXPORT BitSet bitset_new(usize nbits) {
  BitSet bs;
  bs.nbits = nbits;
  bs.nwords = words_for_bits(nbits);
  bs.words = (u64*)calloc(bs.nwords ? bs.nwords : 1, sizeof(u64));
  if (!bs.words) {
    fprintf(stderr, "bitset_new: OOM (%zu bits)\n", (size_t)nbits);
    bs.nwords = 0; bs.nbits = 0;
  }
  return bs;
}

API_EXPORT void bitset_free(BitSet* bs) {
  if (!bs) return;
  free(bs->words);
  bs->words = NULL;
  bs->nwords = 0;
  bs->nbits = 0;
}

API_EXPORT void bitset_resize(BitSet* bs, usize nbits, bool clear_new) {
  if (!bs) return;
  usize old_words = bs->nwords;
  usize new_words = words_for_bits(nbits);
  if (new_words != old_words) {
    u64* nw = (u64*)realloc(bs->words, new_words * sizeof(u64));
    if (!nw) {
      fprintf(stderr, "bitset_resize: OOM (%zu bits)\n", (size_t)nbits);
      return;
    }
    if (clear_new && new_words > old_words) {
      memset(nw + old_words, 0, (new_words - old_words) * sizeof(u64));
    }
    bs->words = nw;
    bs->nwords = new_words;
  }
  bs->nbits = nbits;
}

API_EXPORT void bitset_zero(BitSet* bs) {
  if (!bs || !bs->words) return;
  memset(bs->words, 0, bs->nwords * sizeof(u64));
}

API_EXPORT void bitset_fill(BitSet* bs) {
  if (!bs || !bs->words) return;
  memset(bs->words, 0xFF, bs->nwords * sizeof(u64));
  /* masquer les bits hors plage */
  usize excess = (bs->nwords * 64) - bs->nbits;
  if (excess) {
    u64 mask = ~0ull >> excess;
    bs->words[bs->nwords - 1] = mask;
  }
}

API_EXPORT void bitset_set(BitSet* bs, usize i) {
  if (!bs || i >= bs->nbits) return;
  bs->words[word_index(i)] |= bit_mask(i);
}

API_EXPORT void bitset_clear(BitSet* bs, usize i) {
  if (!bs || i >= bs->nbits) return;
  bs->words[word_index(i)] &= ~bit_mask(i);
}

API_EXPORT void bitset_flip(BitSet* bs, usize i) {
  if (!bs || i >= bs->nbits) return;
  bs->words[word_index(i)] ^= bit_mask(i);
}

API_EXPORT bool bitset_test(const BitSet* bs, usize i) {
  if (!bs || i >= bs->nbits) return false;
  return (bs->words[word_index(i)] & bit_mask(i)) != 0;
}

API_EXPORT usize bitset_count(const BitSet* bs) {
  if (!bs || !bs->words) return 0;
  usize sum = 0;
  for (usize i = 0; i < bs->nwords; i++) sum += popcount64(bs->words[i]);
  return sum;
}

API_EXPORT isize bitset_next_set(const BitSet* bs, usize from) {
  if (!bs || !bs->words || from >= bs->nbits) return -1;
  usize wi = word_index(from);
  u64 w = bs->words[wi] & (~0ull << (from & 63));
  for (;;) {
    if (w) {
      int idx = __builtin_ctzll(w);
      return (isize)((wi << 6) + idx);
    }
    wi++;
    if (wi >= bs->nwords) break;
    w = bs->words[wi];
  }
  return -1;
}

API_EXPORT isize bitset_next_clear(const BitSet* bs, usize from) {
  if (!bs || !bs->words || from >= bs->nbits) return -1;
  usize wi = word_index(from);
  u64 w = ~bs->words[wi] & (~0ull << (from & 63));
  for (;;) {
    if (w) {
      int idx = __builtin_ctzll(w);
      usize pos = (wi << 6) + (usize)idx;
      if (pos < bs->nbits) return (isize)pos;
      else return -1;
    }
    wi++;
    if (wi >= bs->nwords) break;
    w = ~bs->words[wi];
  }
  return -1;
}

/* -------------------------------------------------------------------------- */
/* Itérateur simple                                                           */
/* -------------------------------------------------------------------------- */
API_EXPORT void bitset_iter_init(BitSetIter* it, const BitSet* bs) {
  it->bs = bs;
  it->idx = 0;
}

API_EXPORT bool bitset_iter_next(BitSetIter* it, usize* out_idx) {
  if (!it->bs) return false;
  isize n = bitset_next_set(it->bs, it->idx);
  if (n < 0) return false;
  *out_idx = (usize)n;
  it->idx = (usize)n + 1;
  return true;
}

/* -------------------------------------------------------------------------- */
/* Démo optionnelle                                                           */
/* -------------------------------------------------------------------------- */
#ifdef BITSET_DEMO_MAIN
int main(void) {
  BitSet bs = bitset_new(130);
  bitset_set(&bs, 5);
  bitset_set(&bs, 64);
  bitset_set(&bs, 129);
  printf("count=%zu\n", bitset_count(&bs));

  BitSetIter it; bitset_iter_init(&it, &bs);
  usize idx;
  while (bitset_iter_next(&it, &idx)) {
    printf("set bit %zu\n", idx);
  }
  bitset_free(&bs);
  return 0;
}
#endif
