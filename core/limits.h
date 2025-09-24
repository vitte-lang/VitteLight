/* ============================================================================
   limits.h — Wrapper/compat ultra complet pour constantes de limites (C17).
   - Utilise include_next si dispo pour charger le <limits.h> système, puis
     complète les manquants de façon portable.
   - Ne dépend que de <stddef.h>, <stdint.h>, <wchar.h>.
   - Définit aussi des bornes sûres pour size_t, ptrdiff_t, wchar_t, wint_t.
   - Licence: MIT.
   ============================================================================
 */
#ifndef VT_LIMITS_H
#define VT_LIMITS_H
#pragma once

/* Assure la définition des macros _CTYPE_* avant wchar.h */
#include <ctype.h>

/* Charger la version système si possible (GCC/Clang). */
#if defined(__has_include_next)
#  if __has_include_next(<limits.h>)
#    include_next <limits.h>
#  endif
#endif

#include <stddef.h> /* size_t, ptrdiff_t */
#include <stdint.h> /* SIZE_MAX (C99), largeur entiers fixes */
#if defined(__APPLE__)
#  include <_ctype.h>
#endif
#include <wchar.h>  /* wchar_t, wint_t */

/* ---------------------------------------------------------------------------
   Hypothèses prudentes
--------------------------------------------------------------------------- */
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

/* Largeur binaire générique */
#define VT_WIDTHOF(T) ((int)(sizeof(T) * CHAR_BIT))

/* Constructeurs bornes (const-expr) */
#define VT_U_MAX(W) ((W) >= 64 ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << (W)) - 1ULL))

#define VT_S_MAX(W)                              \
  ((long long)((W) >= 64 ? 0x7FFFFFFFFFFFFFFFULL \
                         : ((1ULL << ((W) - 1)) - 1ULL)))

#define VT_S_MIN(W) (-(VT_S_MAX(W)) - 1LL)

/* ---------------------------------------------------------------------------
   signed char / unsigned char / char
--------------------------------------------------------------------------- */
#ifndef SCHAR_MAX
#define SCHAR_MAX ((signed char)VT_S_MAX(VT_WIDTHOF(signed char)))
#endif
#ifndef SCHAR_MIN
#define SCHAR_MIN ((signed char)VT_S_MIN(VT_WIDTHOF(signed char)))
#endif

#ifndef UCHAR_MAX
#define UCHAR_MAX ((unsigned char)VT_U_MAX(VT_WIDTHOF(unsigned char)))
#endif

#ifndef CHAR_MAX
#if defined(__CHAR_UNSIGNED__) || defined(_CHAR_UNSIGNED)
#define CHAR_MAX ((char)UCHAR_MAX)
#define CHAR_MIN ((char)0)
#else
#define CHAR_MAX ((char)SCHAR_MAX)
#define CHAR_MIN ((char)SCHAR_MIN)
#endif
#endif

/* ---------------------------------------------------------------------------
   short / int / long / long long
--------------------------------------------------------------------------- */
#ifndef SHRT_MAX
#define SHRT_MAX ((short)VT_S_MAX(VT_WIDTHOF(short)))
#define SHRT_MIN ((short)VT_S_MIN(VT_WIDTHOF(short)))
#endif

#ifndef USHRT_MAX
#define USHRT_MAX ((unsigned short)VT_U_MAX(VT_WIDTHOF(unsigned short)))
#endif

#ifndef INT_MAX
#define INT_MAX ((int)VT_S_MAX(VT_WIDTHOF(int)))
#define INT_MIN ((int)VT_S_MIN(VT_WIDTHOF(int)))
#endif

#ifndef UINT_MAX
#define UINT_MAX ((unsigned int)VT_U_MAX(VT_WIDTHOF(unsigned int)))
#endif

#ifndef LONG_MAX
#define LONG_MAX ((long)VT_S_MAX(VT_WIDTHOF(long)))
#define LONG_MIN ((long)VT_S_MIN(VT_WIDTHOF(long)))
#endif

#ifndef ULONG_MAX
#define ULONG_MAX ((unsigned long)VT_U_MAX(VT_WIDTHOF(unsigned long)))
#endif

#ifndef LLONG_MAX
#define LLONG_MAX ((long long)VT_S_MAX(VT_WIDTHOF(long long)))
#define LLONG_MIN ((long long)VT_S_MIN(VT_WIDTHOF(long long)))
#endif

#ifndef ULLONG_MAX
#define ULLONG_MAX \
  ((unsigned long long)VT_U_MAX(VT_WIDTHOF(unsigned long long)))
#endif

/* ---------------------------------------------------------------------------
   size_t / ptrdiff_t
--------------------------------------------------------------------------- */
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)~(size_t)0)
#endif

#ifndef PTRDIFF_MAX
#define PTRDIFF_MAX ((ptrdiff_t)VT_S_MAX(VT_WIDTHOF(ptrdiff_t)))
#define PTRDIFF_MIN ((ptrdiff_t)VT_S_MIN(VT_WIDTHOF(ptrdiff_t)))
#endif

/* ---------------------------------------------------------------------------
   wchar_t / wint_t
--------------------------------------------------------------------------- */
#ifndef WCHAR_MAX
#define WCHAR_MAX                                             \
  ((wchar_t) - 1 > 0 ? (wchar_t)VT_U_MAX(VT_WIDTHOF(wchar_t)) \
                     : (wchar_t)VT_S_MAX(VT_WIDTHOF(wchar_t)))
#endif
#ifndef WCHAR_MIN
#define WCHAR_MIN \
  ((wchar_t) - 1 > 0 ? (wchar_t)0 : (wchar_t)VT_S_MIN(VT_WIDTHOF(wchar_t)))
#endif

#ifndef WINT_MAX
#define WINT_MAX                                           \
  ((wint_t) - 1 > 0 ? (wint_t)VT_U_MAX(VT_WIDTHOF(wint_t)) \
                    : (wint_t)VT_S_MAX(VT_WIDTHOF(wint_t)))
#endif
#ifndef WINT_MIN
#define WINT_MIN \
  ((wint_t) - 1 > 0 ? (wint_t)0 : (wint_t)VT_S_MIN(VT_WIDTHOF(wint_t)))
#endif

/* ---------------------------------------------------------------------------
   Divers standard
--------------------------------------------------------------------------- */
#ifndef MB_LEN_MAX
#define MB_LEN_MAX 16
#endif

/* WORD_BIT est facultatif (POSIX XSI). Définir si absent. */
#ifndef WORD_BIT
#define WORD_BIT (int)(sizeof(int) * CHAR_BIT)
#endif

/* LONG_BIT n’est pas standard mais fréquent. */
#ifndef LONG_BIT
#define LONG_BIT (int)(sizeof(long) * CHAR_BIT)
#endif

/* ---------------------------------------------------------------------------
   Sanity checks (désactivables via VT_LIMITS_NO_STATIC_ASSERT)
--------------------------------------------------------------------------- */
#ifndef VT_LIMITS_NO_STATIC_ASSERT
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(CHAR_BIT >= 8, "CHAR_BIT < 8 non supporté");
_Static_assert(sizeof(unsigned char) == sizeof(char), "hypothèse char");
_Static_assert(sizeof(unsigned short) >= 2, "short >= 16 bits attendu");
_Static_assert(sizeof(unsigned int) >= 2, "int >= 16 bits attendu");
#endif
#endif

#endif /* VT_LIMITS_H */
