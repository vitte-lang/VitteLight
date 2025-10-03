/* ============================================================================
   error.h — Gestion unifiée des erreurs (C11, portable)
   - Type vt_err: code logique + os_err + message court
   - Fonctions: construction (newf, wrapf, from_errno, from_win32)
   - Contrôle: ok/fail, strerror portable
   - Thread-local last error (get/set/clear)
   Licence: MIT
   ============================================================================
*/
#ifndef VT_ERROR_H
#define VT_ERROR_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export + attributs
---------------------------------------------------------------------------- */
#ifndef VT_ERROR_API
#define VT_ERROR_API extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VT_PRINTF(i, j) __attribute__((format(printf, i, j)))
#else
#define VT_PRINTF(i, j)
#endif

/* ----------------------------------------------------------------------------
   Codes d’erreur logiques
---------------------------------------------------------------------------- */
typedef enum {
  VT_EOK = 0,
  VT_EINVAL,
  VT_ENOENT,
  VT_EIO,
  VT_EPERM,
  VT_EAGAIN,
  VT_ENOMEM,
  VT_ERANGE,
  VT_EBUSY,
  VT_EEXIST,
  VT_ENOSPC,
  VT_EPIPE,
  VT_ECONN,
  VT_ETIMEDOUT,
  VT_ECANCELED,
  VT_EFAILED
} vt_err_code;

/* ----------------------------------------------------------------------------
   Type d’erreur
---------------------------------------------------------------------------- */
typedef struct {
  vt_err_code code;   /* code logique */
  int         os_err; /* errno ou GetLastError */
  char        msg[256];
} vt_err;

/* ----------------------------------------------------------------------------
   API
---------------------------------------------------------------------------- */
VT_ERROR_API const char* vt_err_code_str(vt_err_code c);
VT_ERROR_API const char* vt_err_strerror(int os_err, char* out, size_t cap);

VT_ERROR_API vt_err vt_err_ok(void);
VT_ERROR_API int    vt_err_is_ok(const vt_err* e);
VT_ERROR_API int    vt_err_is_fail(const vt_err* e);

VT_ERROR_API vt_err vt_err_newf(vt_err_code c, const char* fmt, ...) VT_PRINTF(2,3);
VT_ERROR_API vt_err vt_err_wrapf(vt_err base, const char* fmt, ...)  VT_PRINTF(2,3);
VT_ERROR_API vt_err vt_err_from_errno(int e, const char* ctx_fmt, ...) VT_PRINTF(2,3);
#if defined(_WIN32)
#include <windows.h>
VT_ERROR_API vt_err vt_err_from_win32(DWORD e, const char* ctx_fmt, ...) VT_PRINTF(2,3);
#endif

/* Thread-local last error */
VT_ERROR_API void        vt_err_clear_last(void);
VT_ERROR_API void        vt_err_set_last(vt_err e);
VT_ERROR_API const vt_err* vt_err_last(void);

/* Helpers errno */
VT_ERROR_API int  vt_errno_save(void);
VT_ERROR_API void vt_errno_restore(int saved);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_ERROR_H */
