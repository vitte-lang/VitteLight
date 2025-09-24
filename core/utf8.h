// SPDX-License-Identifier: MIT
/* ============================================================================
   core/utf8.h — Primitives UTF-8 pour VitteLight
   ============================================================================
*/
#ifndef CORE_UTF8_H
#define CORE_UTF8_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Alias sûrs si non fournis ailleurs */
#ifndef U32_DEFINED
#define U32_DEFINED
typedef uint32_t u32;
#endif

/* Décode un seul caractère UTF-8.
   - s: pointeur sur le début de la séquence
   - n: octets disponibles
   - adv: nombre d’octets consommés (0 si rien de valide)
   Retourne le code point (U+FFFD sur erreur). */
u32 utf8_decode_1(const char* s, size_t n, size_t* adv);

/* Valide une chaîne UTF-8.
   Retourne 1 si valide, 0 sinon. */
int utf8_validate(const char* s, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_UTF8_H */
