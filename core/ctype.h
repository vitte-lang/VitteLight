/* ============================================================================
   ctype.h — Predicats et transformations ASCII, sûrs et portables.
   API par défaut: préfixe vt_. Options de configuration ci-dessous.
   Licence: MIT.
   ============================================================================
 */
#ifndef VT_CTYPE_H
#define VT_CTYPE_H
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Configuration
   ----------------------------------------------------------------------------
   - Définir VT_CTYPE_API pour le marquage d’export (ex: __declspec(dllexport)).
     Par défaut: extern.
   - Définir VT_CTYPE_HEADER_ONLY pour une implémentation header-only inline.
     Sinon, lier avec ctype.c.
   - Définir VT_CTYPE_OVERRIDE_STANDARD pour mapper vt_* vers les noms standard
     (isalnum, tolower, etc.). À utiliser uniquement si vous n’incluez pas
     <ctype.h> ou si vous isolez ce header.
----------------------------------------------------------------------------- */
#ifndef VT_CTYPE_API
#define VT_CTYPE_API extern
#endif

/* ----------------------------------------------------------------------------
   Prototypes (ASCII strict, locale-indépendant)
   ----------------------------------------------------------------------------
 */
VT_CTYPE_API int vt_isascii(int c);
VT_CTYPE_API int vt_isdigit(int c);
VT_CTYPE_API int vt_isxdigit(int c);
VT_CTYPE_API int vt_isalpha(int c);
VT_CTYPE_API int vt_isalnum(int c);
VT_CTYPE_API int vt_islower(int c);
VT_CTYPE_API int vt_isupper(int c);
VT_CTYPE_API int vt_isblank(int c);
VT_CTYPE_API int vt_isspace(int c);
VT_CTYPE_API int vt_iscntrl(int c);
VT_CTYPE_API int vt_isprint(int c);
VT_CTYPE_API int vt_isgraph(int c);
VT_CTYPE_API int vt_ispunct(int c);

VT_CTYPE_API int vt_tolower(int c);
VT_CTYPE_API int vt_toupper(int c);
VT_CTYPE_API int vt_toascii(int c);

/* ----------------------------------------------------------------------------
   Header-only (optionnel) : définir VT_CTYPE_HEADER_ONLY
   Implémentation identique à ctype.c, en inline.
----------------------------------------------------------------------------- */
#ifdef VT_CTYPE_HEADER_ONLY

#ifndef VT_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define VT_INLINE static __inline__ __attribute__((always_inline))
#else
#define VT_INLINE static inline
#endif
#endif

VT_INLINE int vt__in_range(unsigned c, unsigned lo, unsigned hi) {
  return (c - lo) <= (hi - lo);
}
VT_INLINE unsigned vt__sanitize(int c) {
  return (c >= 0 && c <= 255) ? (unsigned)c : 0xFFFFFFFFu;
}

VT_INLINE int vt_isascii(int c) { return (c >= 0 && c <= 0x7F); }
VT_INLINE int vt_isdigit(int c) {
  unsigned u = vt__sanitize(c);
  return (u <= 255u) && vt__in_range(u, '0', '9');
}
VT_INLINE int vt_isxdigit(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return vt__in_range(u, '0', '9') || vt__in_range(u, 'a', 'f') ||
         vt__in_range(u, 'A', 'F');
}
VT_INLINE int vt_isalpha(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return vt__in_range(u, 'A', 'Z') || vt__in_range(u, 'a', 'z');
}
VT_INLINE int vt_isalnum(int c) { return vt_isalpha(c) || vt_isdigit(c); }
VT_INLINE int vt_islower(int c) {
  unsigned u = vt__sanitize(c);
  return (u <= 255u) && vt__in_range(u, 'a', 'z');
}
VT_INLINE int vt_isupper(int c) {
  unsigned u = vt__sanitize(c);
  return (u <= 255u) && vt__in_range(u, 'A', 'Z');
}
VT_INLINE int vt_isblank(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return (u == ' ') || (u == '\t');
}
VT_INLINE int vt_isspace(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return (u == ' ') || vt__in_range(u, 0x09u, 0x0Du);
}
VT_INLINE int vt_iscntrl(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return (u <= 0x1Fu) || (u == 0x7Fu);
}
VT_INLINE int vt_isprint(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return vt__in_range(u, 0x20u, 0x7Eu);
}
VT_INLINE int vt_isgraph(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return 0;
  return vt__in_range(u, 0x21u, 0x7Eu);
}
VT_INLINE int vt_ispunct(int c) { return vt_isgraph(c) && !vt_isalnum(c); }

VT_INLINE int vt_tolower(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return c;
  return vt__in_range(u, 'A', 'Z') ? (int)(u + ('a' - 'A')) : c;
}
VT_INLINE int vt_toupper(int c) {
  unsigned u = vt__sanitize(c);
  if (u > 255u) return c;
  return vt__in_range(u, 'a', 'z') ? (int)(u - ('a' - 'A')) : c;
}
VT_INLINE int vt_toascii(int c) { return c & 0x7F; }

#endif /* VT_CTYPE_HEADER_ONLY */

/* ----------------------------------------------------------------------------
   Remapping optionnel vers les noms standard
   (à activer si vous ne mélangez pas avec <ctype.h>)
----------------------------------------------------------------------------- */
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

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_CTYPE_H */
