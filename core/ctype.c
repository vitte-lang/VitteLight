/* ============================================================================
   ctype.c — Implémentation ASCII, sûre et portable, des prédicats ctype et cas
   - Couverture : isalnum, isalpha, isascii, isblank, iscntrl, isdigit, isgraph,
                  islower, isprint, ispunct, isspace, isupper, isxdigit,
                  tolower, toupper, toascii.
   - Semantique : locale-indépendante, ASCII strict (0x00..0x7F). Pour 8 bits
                  étendus (ISO-8859-1, etc.), le comportement est non défini
                  comme dans la norme si l’impl appelle <ctype.h> sans cast.
   - Sûreté   : prend un int « style C » (peut valoir EOF = -1). Aucune UB :
                  on borne et on cast en unsigned avant comparaison.
   - Intégration : par défaut les symboles sont préfixés vt_. Définir
                  VT_CTYPE_OVERRIDE_STANDARD avant l’inclusion pour mapper
                  vt_isalpha → isalpha, etc., et vt_tolower → tolower, etc.
   - Licence : MIT.
   ============================================================================
 */

#include <limits.h> /* CHAR_BIT */
#include <stdint.h> /* uint32_t */

/* -------------------------------------------------------------------------- */
/* Helpers internes                                                           */
/* -------------------------------------------------------------------------- */

#ifndef VT_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define VT_INLINE static __inline__ __attribute__((always_inline))
#else
#define VT_INLINE static inline
#endif
#endif

/* Retourne 1 si c est dans [lo, hi], sinon 0. c est supposé 0..255 ici. */
VT_INLINE int vt__in_range(unsigned c, unsigned lo, unsigned hi) {
  return (c - lo) <= (hi - lo);
}

/* Normalise l’entrée : EOF → 0xFFFFFFFF, <0 ou >255 → 0xFFFFFFFF, sinon 0..255.
 */
VT_INLINE unsigned vt__u8_or_invalid(int c) { return (unsigned)(c); }

/* Borne l’entrée à 0..255, sinon marque « invalide » via 0xFFFFFFFF. */
VT_INLINE unsigned vt__sanitize(int c) {
  unsigned u = vt__u8_or_invalid(c);
  return (c >= 0 && c <= 255) ? u : 0xFFFFFFFFu;
}

/* -------------------------------------------------------------------------- */
/* Prédicats ASCII                                                            */
/* -------------------------------------------------------------------------- */

int vt_isascii(int c) { return (c >= 0 && c <= 0x7F); }

int vt_isdigit(int c) {
  unsigned u = vt__sanitize(c);
  return (u <= 255u) && vt__in_range(u, '0', '9');
}

int vt_isxdigit(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return vt__in_range(u, '0', '9') || vt__in_range(u, 'a', 'f') ||
         vt__in_range(u, 'A', 'F');
}

int vt_isalpha(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return vt__in_range(u, 'A', 'Z') || vt__in_range(u, 'a', 'z');
}

int vt_isalnum(int c) { return vt_isalpha(c) || vt_isdigit(c); }

int vt_islower(int c) {
  unsigned u = vt__sanitize(c);
  return (u <= 255u) && vt__in_range(u, 'a', 'z');
}

int vt_isupper(int c) {
  unsigned u = vt__sanitize(c);
  return (u <= 255u) && vt__in_range(u, 'A', 'Z');
}

int vt_isblank(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return (u == ' ') || (u == '\t');
}

int vt_isspace(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  /* ' ' 0x20, '\t' 0x09, '\n' 0x0A, '\v' 0x0B, '\f' 0x0C, '\r' 0x0D */
  return (u == ' ') || vt__in_range(u, 0x09u, 0x0Du);
}

int vt_iscntrl(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return (u <= 0x1Fu) || (u == 0x7Fu);
}

int vt_isprint(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  /* 0x20 (space) .. 0x7E (~) */
  return vt__in_range(u, 0x20u, 0x7Eu);
}

int vt_isgraph(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  /* 0x21 (!) .. 0x7E (~) */
  return vt__in_range(u, 0x21u, 0x7Eu);
}

int vt_ispunct(int c) {
  /* graphique mais pas alnum */
  return vt_isgraph(c) && !vt_isalnum(c);
}

/* -------------------------------------------------------------------------- */
/* Transformations de casse (ASCII)                                           */
/* -------------------------------------------------------------------------- */

int vt_tolower(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return c;
  return vt__in_range(u, 'A', 'Z') ? (int)(u + ('a' - 'A')) : c;
}

int vt_toupper(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return c;
  return vt__in_range(u, 'a', 'z') ? (int)(u - ('a' - 'A')) : c;
}

int vt_toascii(int c) { return c & 0x7F; }

/* -------------------------------------------------------------------------- */
/* Option : Remapper vers les noms standard (attention aux collisions libc)   */
/* -------------------------------------------------------------------------- */
#ifdef VT_CTYPE_OVERRIDE_STANDARD
#define isascii vt_isascii
#define isdigit vt_isdigit
#define isxdigit vt_isxdigit
#define isalpha vt_isalpha
#define isalnum vt_isalnum
#define islower vt_islower
#define isupper vt_isupper
#define isblank vt_isblank
#define isspace vt_isspace
#define iscntrl vt_iscntrl
#define isprint vt_isprint
#define isgraph vt_isgraph
#define ispunct vt_ispunct
#define tolower vt_tolower
#define toupper vt_toupper
#define toascii vt_toascii
#endif

/* -------------------------------------------------------------------------- */
/* Auto-tests basiques (activer avec -DVT_CTYPE_TEST) */
/* -------------------------------------------------------------------------- */
#ifdef VT_CTYPE_TEST
#include <assert.h>
#include <stdio.h>
static void vt__selftest(void) {
  assert(vt_isdigit('0') && vt_isdigit('9') && !vt_isdigit('a'));
  assert(vt_isalpha('a') && vt_isalpha('Z') && !vt_isalpha('1'));
  assert(vt_isalnum('a') && vt_isalnum('7') && !vt_isalnum('@'));
  assert(vt_isxdigit('F') && vt_isxdigit('f') && vt_isxdigit('9') &&
         !vt_isxdigit('G'));
  assert(vt_islower('z') && !vt_islower('Z'));
  assert(vt_isupper('Z') && !vt_isupper('z'));
  assert(vt_isblank(' ') && vt_isblank('\t') && !vt_isblank('\n'));
  assert(vt_isspace(' ') && vt_isspace('\n') && vt_isspace('\r') &&
         !vt_isspace('A'));
  assert(vt_isprint(' ') && vt_isprint('~') && !vt_isprint('\x1F'));
  assert(vt_isgraph('!') && !vt_isgraph(' ') && !vt_isgraph('\n'));
  assert(vt_iscntrl('\x00') && vt_iscntrl('\x1F') && vt_iscntrl('\x7F') &&
         !vt_iscntrl('A'));
  assert(vt_ispunct('!') && vt_ispunct('/') && !vt_ispunct('A') &&
         !vt_ispunct('1'));
  assert(vt_tolower('A') == 'a' && vt_tolower('a') == 'a');
  assert(vt_toupper('z') == 'Z' && vt_toupper('Z') == 'Z');
  assert(vt_isascii(0x7F) && !vt_isascii(0x80));
  assert(vt_isdigit(-1) == 0);  /* EOF safe */
  assert(vt_tolower(-1) == -1); /* passthrough for invalid */
  puts("vt_ctype: OK");
}
int main(void) {
  vt__selftest();
  return 0;
}
#endif
