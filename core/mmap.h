/* ============================================================================
 * mmap.h — Portable memory mapping API (C11)
 * VitteLight / Vitl runtime
 * ========================================================================== */
#ifndef VITTELIGHT_MMAP_H
#define VITTELIGHT_MMAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility */
#ifndef MM_API
#  if defined(_WIN32) && defined(MM_DLL)
#    ifdef MM_BUILD
#      define MM_API __declspec(dllexport)
#    else
#      define MM_API __declspec(dllimport)
#    endif
#  else
#    define MM_API
#  endif
#endif

#include <stddef.h>
#include <stdbool.h>

/* Protections */
enum {
    MM_PROT_READ  = 1,
    MM_PROT_WRITE = 2,
    MM_PROT_EXEC  = 4
};

/* Region handle */
typedef struct {
    void*  ptr;
    size_t size;
    int    writable;
#if defined(_WIN32)
    void*  hFile; /* HANDLE */
    void*  hMap;  /* HANDLE */
#else
    int    fd;
#endif
} mm_region;

/* Map a file. shared: 0 private, 1 shared. Returns 0/-1. */
MM_API int  mm_map_file(const char* path, mm_region* out, int prot, int shared);

/* Map anonymous memory of given size. Returns 0/-1. */
MM_API int  mm_map_anon(mm_region* out, size_t size, int prot);

/* Flush changes in [off, off+len). async=true uses async flush when available. */
MM_API int  mm_sync(mm_region* r, size_t off, size_t len, bool async);

/* Change protections on region. Returns 0/-1. */
MM_API int  mm_protect(mm_region* r, int prot);

/* Unmap and release resources. Safe with NULL. */
MM_API void mm_unmap(mm_region* r);

/* Last error message (static buffer). Empty string if none. */
MM_API const char* mm_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITTELIGHT_MMAP_H */
