/* ============================================================================
   func.h — API C17 multi-plateforme pour utilitaires système/chaînes/fichiers.
   S’associe à func.c. Licence: MIT.
   ============================================================================
 */
#ifndef VT_FUNC_H
#define VT_FUNC_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint*_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Export */
#ifndef VT_FUNC_API
#define VT_FUNC_API extern
#endif

/* Attribut printf-like */
#if defined(__GNUC__) || defined(__clang__)
#define VT_FUNC_PRINTF(fmt_idx, va_idx) \
  __attribute__((format(printf, fmt_idx, va_idx)))
#else
#define VT_FUNC_PRINTF(fmt_idx, va_idx)
#endif

/* ============================================================================
   1) Chaînes
   ============================================================================
 */
VT_FUNC_API char* vt_str_dup(const char* s); /* malloc */
VT_FUNC_API char* vt_str_trim(char* s);      /* in-place */
VT_FUNC_API char* vt_str_ltrim(char* s);     /* in-place */
VT_FUNC_API char* vt_str_rtrim(char* s);     /* in-place */
VT_FUNC_API void vt_str_tolower(char* s);    /* ASCII */
VT_FUNC_API void vt_str_toupper(char* s);    /* ASCII */
VT_FUNC_API char* vt_str_replace_all(const char* s, const char* from,
                                     const char* to); /* malloc */

/* Liste de tokens (pointeurs dans un buffer interne alloué) */
typedef struct {
  char** v;      /* pointeurs vers tokens (dans storage) */
  size_t n;      /* nombre de tokens */
  size_t cap;    /* capacité interne */
  char* storage; /* buffer propriétaire */
} vt_strlist;

VT_FUNC_API void vt_strlist_free(vt_strlist* L);
VT_FUNC_API vt_strlist vt_str_split(const char* s, char sep, int keep_empty);

/* ============================================================================
   2) Encodages & UTF-8
   ============================================================================
 */
VT_FUNC_API char* vt_hex_encode(const void* data, size_t n); /* malloc */
VT_FUNC_API unsigned char* vt_hex_decode(const char* s,
                                         size_t* out_len); /* malloc */

VT_FUNC_API char* vt_base64_encode(const void* data, size_t n); /* malloc */
VT_FUNC_API unsigned char* vt_base64_decode(const char* s,
                                            size_t* out_len); /* malloc */

/* UTF-8: itération sûre. Renvoie 1 si ok, 0 sinon. Avance *i, écrit codepoint.
 */
VT_FUNC_API int vt_utf8_next(const unsigned char* s, size_t n, size_t* i,
                             uint32_t* cp);
VT_FUNC_API int vt_utf8_is_valid(const unsigned char* s, size_t n);

/* ============================================================================
   3) Fichiers & chemins
   ============================================================================
 */
VT_FUNC_API int vt_file_exists(const char* path);              /* bool */
VT_FUNC_API int vt_file_size(const char* path, uint64_t* out); /* 0 ok */
VT_FUNC_API unsigned char* vt_file_read_all(const char* path,
                                            size_t* n); /* malloc */
VT_FUNC_API int vt_file_write_all(const char* path, const void* data, size_t n);
VT_FUNC_API int vt_file_copy(const char* src, const char* dst);
VT_FUNC_API int vt_mkdir_p(const char* path);

VT_FUNC_API int vt_path_is_abs(const char* p);                /* bool */
VT_FUNC_API char* vt_path_join(const char* a, const char* b); /* malloc */
VT_FUNC_API char* vt_path_basename(const char* p);            /* malloc */
VT_FUNC_API char* vt_path_dirname(const char* p);             /* malloc */
VT_FUNC_API char* vt_path_normalize(const char* p);           /* malloc */

/* ============================================================================
   4) Environnement
   ============================================================================
 */
VT_FUNC_API const char* vt_env_get(const char* key);          /* ptr env */
VT_FUNC_API int vt_env_set(const char* key, const char* val); /* 0 ok */

/* ============================================================================
   5) Temps / sommeil
   ============================================================================
 */
VT_FUNC_API uint64_t vt_time_now_ns(void);          /* horloge murale (ns) */
VT_FUNC_API void vt_time_utc_iso8601(char out[32]); /* "YYYY-MM-DDThh:mm:ssZ" */
VT_FUNC_API void vt_sleep_ms(uint32_t ms);

/* ============================================================================
   6) RNG fort
   ============================================================================
 */
VT_FUNC_API int vt_random_bytes(void* out, size_t n); /* 0 ok */
VT_FUNC_API uint64_t vt_random_u64(void);

/* ============================================================================
   7) Buffer dynamique (string builder)
   ============================================================================
 */
typedef struct {
  char* data;
  size_t len;
  size_t cap;
} vt_buf;

VT_FUNC_API void vt_buf_init(vt_buf* b);
VT_FUNC_API void vt_buf_free(vt_buf* b);
VT_FUNC_API int vt_buf_append(vt_buf* b, const void* data, size_t n);
VT_FUNC_API int vt_buf_append_str(vt_buf* b, const char* s);
VT_FUNC_API int vt_buf_printf(vt_buf* b, const char* fmt, ...)
    VT_FUNC_PRINTF(2, 3);

/* ============================================================================
   8) Hash
   ============================================================================
 */
VT_FUNC_API uint64_t vt_hash_fnv1a64(const void* data, size_t n);

/* ============================================================================
   9) Parsing numérique
   ============================================================================
 */
VT_FUNC_API int vt_parse_u64(const char* s, uint64_t* out); /* 0 ok */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_FUNC_H */
