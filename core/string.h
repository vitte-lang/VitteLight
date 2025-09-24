/* ============================================================================
   string.h — Bibliothèque chaînes Vitte/Vitl (C17, UTF-8, portable)
   - Déclare vt_sv (string view) et vt_str (chaîne dynamique).
   - API: append/insert/erase/replace, trim/case, recherche (KMP), split/join,
          wildcard (*, ?), UTF-8 (validation/itération), conversions num.,
          hex/base64, hash FNV-1a, helpers de chemins.
   - Lier avec string.c. Licence: MIT.
   ============================================================================
 */
#ifndef VT_STRING_H
#define VT_STRING_H
#pragma once

#if defined(__has_include_next)
#  if __has_include_next(<string.h>)
#    include_next <string.h>
#  elif __has_include(<string.h>)
#    include <string.h>
#  endif
#elif defined(__clang__) || defined(__GNUC__)
#  include_next <string.h>
#else
#  include <string.h>
#endif

#include <stdbool.h> /* bool */
#include <stddef.h>  /* size_t, ptrdiff_t */
#include <stdint.h>  /* uint64_t, int64_t, uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Compatibilité ssize_t (MSVC)
---------------------------------------------------------------------------- */
#if defined(_WIN32) && !defined(_SSIZE_T_) && !defined(_SSIZE_T_DEFINED)
typedef ptrdiff_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* ----------------------------------------------------------------------------
   Export & annotations printf-like
---------------------------------------------------------------------------- */
#ifndef VT_STRING_API
#define VT_STRING_API extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VT_STR_PRINTF(fmt_idx, va_idx) \
  __attribute__((format(printf, fmt_idx, va_idx)))
#else
#define VT_STR_PRINTF(fmt_idx, va_idx)
#endif

/* ----------------------------------------------------------------------------
   Types publics
---------------------------------------------------------------------------- */
typedef struct vt_sv {
  const char* data;
  size_t len;
} vt_sv;

typedef struct vt_str {
  char* data;
  size_t len;
  size_t cap;
} vt_str;

/* Vector de vues (pour split) */
typedef struct vt_sv_vec {
  vt_sv* v;
  size_t len;
  size_t cap;
} vt_sv_vec;

/* ----------------------------------------------------------------------------
   vt_str — cycle de vie
---------------------------------------------------------------------------- */
VT_STRING_API void vt_str_init(vt_str* s);
VT_STRING_API void vt_str_with_cap(vt_str* s, size_t cap_hint);
VT_STRING_API void vt_str_free(vt_str* s);
VT_STRING_API void vt_str_clear(vt_str* s);
VT_STRING_API void vt_str_shrink(vt_str* s);

/* ----------------------------------------------------------------------------
   Vues & sous-chaînes
---------------------------------------------------------------------------- */
VT_STRING_API vt_sv vt_sv_from_cstr(const char* z);
VT_STRING_API vt_sv vt_sv_sub(vt_sv v, size_t pos, size_t n);

/* ----------------------------------------------------------------------------
   Réservation & append
---------------------------------------------------------------------------- */
VT_STRING_API void vt_str_reserve(vt_str* s, size_t need);
VT_STRING_API void vt_str_push_char(vt_str* s, char c);
VT_STRING_API void vt_str_append(vt_str* s, const char* z);
VT_STRING_API void vt_str_append_n(vt_str* s, const char* p, size_t n);
VT_STRING_API void vt_str_append_sv(vt_str* s, vt_sv v);
VT_STRING_API void vt_str_append_fmt(vt_str* s, const char* fmt, ...)
    VT_STR_PRINTF(2, 3);

/* ----------------------------------------------------------------------------
   Edition
---------------------------------------------------------------------------- */
VT_STRING_API void vt_str_insert(vt_str* s, size_t pos, const char* z);
VT_STRING_API void vt_str_erase(vt_str* s, size_t pos, size_t n);
VT_STRING_API size_t vt_str_replace_all(vt_str* s, vt_sv from, vt_sv to);

/* ----------------------------------------------------------------------------
   Trim & casse (ASCII)
---------------------------------------------------------------------------- */
VT_STRING_API void vt_str_ltrim_ws(vt_str* s);
VT_STRING_API void vt_str_rtrim_ws(vt_str* s);
VT_STRING_API void vt_str_trim_ws(vt_str* s);
VT_STRING_API void vt_str_to_lower_ascii(vt_str* s);
VT_STRING_API void vt_str_to_upper_ascii(vt_str* s);

/* ----------------------------------------------------------------------------
   Recherche / comparaisons
---------------------------------------------------------------------------- */
VT_STRING_API ssize_t vt_sv_find(vt_sv hay, vt_sv nee);  /* KMP */
VT_STRING_API ssize_t vt_sv_rfind(vt_sv hay, vt_sv nee); /* reverse naïf */
VT_STRING_API bool vt_sv_starts_with(vt_sv s, vt_sv pre);
VT_STRING_API bool vt_sv_ends_with(vt_sv s, vt_sv suf);
VT_STRING_API int vt_sv_cmp(vt_sv a, vt_sv b);           /* binaire */
VT_STRING_API int vt_sv_casecmp_ascii(vt_sv a, vt_sv b); /* ASCII only */

/* ----------------------------------------------------------------------------
   Split / Join
---------------------------------------------------------------------------- */
VT_STRING_API void vt_sv_vec_free(vt_sv_vec* out);
VT_STRING_API void vt_sv_split_char(vt_sv s, char sep, vt_sv_vec* out);
VT_STRING_API void vt_sv_split_sv(vt_sv s, vt_sv sep, vt_sv_vec* out);
VT_STRING_API void vt_str_join_sv(vt_str* dst, vt_sv glue, const vt_sv* parts,
                                  size_t n);

/* ----------------------------------------------------------------------------
   Wildcard (ASCII) — '*' et '?'
---------------------------------------------------------------------------- */
VT_STRING_API bool vt_wildcard_match_ascii(vt_sv text, vt_sv pat);

/* ----------------------------------------------------------------------------
   UTF-8
---------------------------------------------------------------------------- */
VT_STRING_API bool vt_utf8_valid(vt_sv s);
VT_STRING_API size_t
vt_utf8_count(vt_sv s); /* nb codepoints; (size_t)-1 si invalide */
VT_STRING_API int32_t vt_utf8_next(const char* p, size_t n,
                                   size_t* adv); /* -1 si invalide */

/* ----------------------------------------------------------------------------
   Conversions numériques
---------------------------------------------------------------------------- */
VT_STRING_API bool vt_parse_i64(vt_sv s, int base, int64_t* out);
VT_STRING_API bool vt_parse_u64(vt_sv s, int base, uint64_t* out);
VT_STRING_API bool vt_parse_f64(vt_sv s, double* out);

/* ----------------------------------------------------------------------------
   Hex / Base64
---------------------------------------------------------------------------- */
VT_STRING_API void vt_hex_encode(vt_str* out, const void* data, size_t n,
                                 bool upper);
VT_STRING_API bool vt_hex_decode(vt_str* out_bin, vt_sv hex);

VT_STRING_API void vt_base64_encode(vt_str* out, const void* data, size_t n);
VT_STRING_API bool vt_base64_decode(vt_str* out_bin, vt_sv b64);

/* ----------------------------------------------------------------------------
   Hash
---------------------------------------------------------------------------- */
VT_STRING_API uint64_t vt_hash_fnv1a64(const void* p, size_t n);

/* ----------------------------------------------------------------------------
   Helpers de chemins (style POSIX: normalise '\\' → '/')
---------------------------------------------------------------------------- */
VT_STRING_API void vt_path_normalize_slashes(vt_str* s);
VT_STRING_API vt_sv vt_path_basename(vt_sv s);
VT_STRING_API vt_sv vt_path_dirname(vt_sv s);
VT_STRING_API void vt_path_join(vt_str* out, vt_sv a, vt_sv b);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_STRING_H */
