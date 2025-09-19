// SPDX-License-Identifier: MIT
/* ============================================================================
   core/utf8.c — Primitives UTF-8 pour VitteLight
   ============================================================================
*/

#include "core/utf8.h"
#include <stddef.h>
#include <stdint.h>

/* Types courts si non inclus via api.h */
#ifndef U32_DEFINED
#define U32_DEFINED
typedef uint32_t u32;
#endif

/* -------------------------------------------------------------------------- */
/* Décodage UTF-8 basique                                                     */
/* -------------------------------------------------------------------------- */
u32 utf8_decode_1(const char* s, size_t n, size_t* adv) {
  if (!s || n == 0) {
    if (adv) *adv = 0;
    return 0xFFFD; /* replacement char */
  }

  const unsigned char* p = (const unsigned char*)s;
  unsigned char c = p[0];

  if (c < 0x80) {
    if (adv) *adv = 1;
    return (u32)c;
  }

  /* 2 bytes */
  if ((c & 0xE0) == 0xC0 && n >= 2) {
    u32 cp = ((u32)(c & 0x1F) << 6) | (u32)(p[1] & 0x3F);
    if (cp >= 0x80) {
      if (adv) *adv = 2;
      return cp;
    }
  }

  /* 3 bytes */
  if ((c & 0xF0) == 0xE0 && n >= 3) {
    u32 cp = ((u32)(c & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6) |
             (u32)(p[2] & 0x3F);
    if (cp >= 0x800) {
      if (adv) *adv = 3;
      return cp;
    }
  }

  /* 4 bytes */
  if ((c & 0xF8) == 0xF0 && n >= 4) {
    u32 cp = ((u32)(c & 0x07) << 18) | ((u32)(p[1] & 0x3F) << 12) |
             ((u32)(p[2] & 0x3F) << 6) | (u32)(p[3] & 0x3F);
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      if (adv) *adv = 4;
      return cp;
    }
  }

  /* Séquence invalide : fallback */
  if (adv) *adv = 1;
  return 0xFFFD;
}

int utf8_validate(const char* s, size_t n) {
  size_t i = 0, adv = 0;
  while (i < n) {
    u32 cp = utf8_decode_1(s + i, n - i, &adv);
    if (adv == 0) return 0;
    (void)cp; /* on ignore juste la valeur */
    i += adv;
  }
  return 1;
}