/* ============================================================================
 * loader.h — Public API for portable file and dynamic library loading (C11)
 * VitteLight / Vitl runtime
 * ============================================================================
 */
#ifndef VITTELIGHT_LOADER_H
#define VITTELIGHT_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility */
#ifndef LOADER_API
#  if defined(_WIN32) && defined(LOADER_DLL)
#    ifdef LOADER_BUILD
#      define LOADER_API __declspec(dllexport)
#    else
#      define LOADER_API __declspec(dllimport)
#    endif
#  else
#    define LOADER_API
#  endif
#endif

#include <stddef.h>
#include <stdbool.h>

/* Platform path separator (compile-time) */
#if defined(_WIN32)
#  define LOADER_PATH_SEP '\\'
#else
#  define LOADER_PATH_SEP '/'
#endif

/* ===== Types ============================================================== */

/* Whole-file buffer (heap-owned). */
typedef struct {
    void*  data;
    size_t size;
} loader_blob;

/* File mapping or heap fallback. */
typedef struct {
    void*  ptr;
    size_t size;
    bool   is_mapped;
#if defined(_WIN32)
    /* Internal handles (do not touch). */
    void*  hFile;
    void*  hMap;
#else
    int    fd;
#endif
} loader_map;

/* Dynamic library handle + cached path. */
typedef struct {
#if defined(_WIN32)
    void*  handle;      /* HMODULE */
#else
    void*  handle;      /* void* from dlopen */
#endif
    char   path[512];
} loader_dylib;

/* ===== Files ============================================================== */

/* Read entire file into memory. Returns 0 on success, -1 on error. */
LOADER_API int  loader_read_all(const char* path, loader_blob* out);

/* Free memory from loader_read_all. Safe with NULL. */
LOADER_API void loader_blob_free(loader_blob* b);

/* Map file read-only when possible. Falls back to heap. 0 on success. */
LOADER_API int  loader_map_open(const char* path, loader_map* out);

/* Unmap/close or free resources from loader_map_open. Safe with NULL. */
LOADER_API void loader_map_close(loader_map* m);

/* Check file existence. 1 yes, 0 no, -1 error. */
LOADER_API int  loader_file_exists(const char* path);

/* Join a + b into dst with platform separator.
 * Returns the length that would have been written (excluding NUL). */
LOADER_API size_t loader_join_path(char* dst, size_t cap,
                                   const char* a, const char* b);

/* ===== Dynamic libraries ================================================== */

/* Open a dynamic library at path. 0 on success. */
LOADER_API int   loader_dylib_open(const char* path, loader_dylib* lib);

/* Resolve a symbol. NULL on error. */
LOADER_API void* loader_dylib_sym(const loader_dylib* lib, const char* name);

/* Close a dynamic library. Safe with NULL. */
LOADER_API void  loader_dylib_close(loader_dylib* lib);

/* ===== Errors ============================================================= */

/* Last error message (thread-unsafe static buffer). Empty string if none. */
LOADER_API const char* loader_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITTELIGHT_LOADER_H */
