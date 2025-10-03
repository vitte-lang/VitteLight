/* ============================================================================
   dl.h — Dynamic loader portable (C11)
   - Unifie POSIX (dlopen/dlsym/dlclose) et Windows (LoadLibrary/GetProcAddress).
   - Fonctions : open, sym, close, error, helpers (prefix/ext), search dirs.
   - Licence : MIT
   ============================================================================
*/
#ifndef VT_DL_H
#define VT_DL_H
#pragma once

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export macro
---------------------------------------------------------------------------- */
#ifndef VT_DL_API
#define VT_DL_API extern
#endif

/* ----------------------------------------------------------------------------
   Flags
---------------------------------------------------------------------------- */
enum {
  VT_DL_LAZY     = 1 << 0,
  VT_DL_NOW      = 1 << 1,
  VT_DL_LOCAL    = 1 << 2,
  VT_DL_GLOBAL   = 1 << 3,
  VT_DL_NOLOAD   = 1 << 4,
  VT_DL_NODELETE = 1 << 5
};

/* ----------------------------------------------------------------------------
   API principale
---------------------------------------------------------------------------- */
VT_DL_API const char* vt_dl_error(void);

VT_DL_API void* vt_dl_open(const char* path);
VT_DL_API void* vt_dl_open2(const char* path, int flags);
VT_DL_API void* vt_dl_open_with_ext(const char* base, int flags);
VT_DL_API void* vt_dl_open_name(const char* soname, int flags);

VT_DL_API int   vt_dl_close(void* handle);

VT_DL_API void* vt_dl_self(void);
#if !defined(_WIN32)
VT_DL_API void* vt_dl_next(void);
#endif

VT_DL_API void* vt_dl_sym(void* handle, const char* name);
#if !defined(_WIN32)
VT_DL_API void* vt_dl_symv(void* handle, const char* name, const char* version);
#endif

/* ----------------------------------------------------------------------------
   Helpers nommage / chemins
---------------------------------------------------------------------------- */
VT_DL_API const char* vt_dl_default_ext(void);
VT_DL_API const char* vt_dl_default_prefix(void);
VT_DL_API int  vt_dl_add_ext_if_missing(const char* in, char* out, size_t cap);
VT_DL_API int  vt_dl_add_prefix_if_missing(const char* in, char* out, size_t cap);
VT_DL_API int  vt_dl_build_name(const char* base, char* out, size_t cap);

VT_DL_API int  vt_dl_get_module_path(void* handle, char* out, size_t cap);
VT_DL_API int  vt_dl_get_module_dir(void* handle, char* out, size_t cap);

/* ----------------------------------------------------------------------------
   Répertoires de recherche
---------------------------------------------------------------------------- */
VT_DL_API void vt_dl_clear_search_dirs(void);
VT_DL_API int  vt_dl_push_search_dir(const char* dir);
VT_DL_API int  vt_dl_pop_search_dir(void);
VT_DL_API void* vt_dl_search_open(const char* base_or_path, int flags);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_DL_H */
