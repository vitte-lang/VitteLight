/* ============================================================================
   json.h — JSON minimal DOM (C17, MIT)
   - Types: null, bool, number (double), string (UTF-8), array, object
   - Création, accès, modification
   - Parse depuis chaîne ou fichier
   - Stringify compact ou pretty
   Lier avec json.c
   ============================================================================
*/
#ifndef VT_JSON_H
#define VT_JSON_H
#pragma once

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VT_JSON_API
#define VT_JSON_API extern
#endif

/* ----------------------------------------------------------------------------
   Types
---------------------------------------------------------------------------- */
typedef enum {
  VTJ_NULL = 0, VTJ_BOOL, VTJ_NUM, VTJ_STR, VTJ_ARR, VTJ_OBJ
} vtj_type;

typedef struct vt_json vt_json;

/* ----------------------------------------------------------------------------
   Constructeurs
---------------------------------------------------------------------------- */
VT_JSON_API vt_json* vtj_null(void);
VT_JSON_API vt_json* vtj_bool(int b);
VT_JSON_API vt_json* vtj_num(double x);
VT_JSON_API vt_json* vtj_str(const char* s);   /* duplique s */
VT_JSON_API vt_json* vtj_arr(void);
VT_JSON_API vt_json* vtj_obj(void);

VT_JSON_API int vtj_arr_push(vt_json* a, vt_json* v);                /* 0/-1 */
VT_JSON_API int vtj_obj_put(vt_json* o, const char* k, vt_json* v);  /* 0/-1 */

/* ----------------------------------------------------------------------------
   Accès
---------------------------------------------------------------------------- */
VT_JSON_API vtj_type vtj_typeof(const vt_json* v);
VT_JSON_API size_t   vtj_len(const vt_json* v); /* arr/obj */
VT_JSON_API double   vtj_as_num(const vt_json* v, int* ok);
VT_JSON_API int      vtj_as_bool(const vt_json* v, int* ok);
VT_JSON_API const char* vtj_as_str(const vt_json* v, int* ok);

VT_JSON_API vt_json* vtj_obj_get(const vt_json* o, const char* key);
VT_JSON_API vt_json* vtj_arr_get(const vt_json* a, size_t idx);

/* Libération */
VT_JSON_API void vtj_free(vt_json* v);

/* ----------------------------------------------------------------------------
   Parsing
---------------------------------------------------------------------------- */
typedef struct {
  const char* msg;  /* NULL si OK */
  size_t line, col; /* 1-based */
  size_t byte_off;  /* 0-based */
} vtj_error;

VT_JSON_API vt_json* vtj_parse(const char* json, vtj_error* err);
VT_JSON_API vt_json* vtj_parse_n(const char* json, size_t n, vtj_error* err);
VT_JSON_API vt_json* vtj_parse_file(const char* path, vtj_error* err);

/* ----------------------------------------------------------------------------
   Stringify
---------------------------------------------------------------------------- */
typedef struct {
  int pretty;       /* 0=compact, 1=pretty */
  int indent;       /* espaces par niveau */
  int ascii_only;   /* 1 = escape non-ASCII */
} vtj_write_opts;

VT_JSON_API char* vtj_stringify(const vt_json* v, const vtj_write_opts* opt);
/* retourne malloc()+NUL, à free() */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_JSON_H */
