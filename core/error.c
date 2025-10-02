/* ============================================================================
   error.c — Gestion d’erreurs ultra complète (C11, portable)
   - Type unifié vt_err (code + errno/GetLastError + message court)
   - Buffer thread-local pour le dernier vt_err
   - Fabrication: from_errno, from_win32, newf, wrapf
   - Conversion: code -> texte, strerror portable (POSIX/Windows)
   - Outils: sauvegarde/restore errno, clear, ok/fail
   - Intégration autonome: fournit l’API si error.h absent
   Licence: MIT
   ============================================================================
*/

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN 1
#  include <windows.h>
#endif

/* ----------------------------------------------------------------------------
   API publique (si error.h absent)
---------------------------------------------------------------------------- */
#ifndef VT_ERROR_HAVE_HEADER
#ifndef VT_ERROR_API
#define VT_ERROR_API extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VT_PRINTF(i, j) __attribute__((format(printf, i, j)))
#else
#define VT_PRINTF(i, j)
#endif

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
  VT_EFAILED /* inconnu/générique */
} vt_err_code;

typedef struct {
  vt_err_code code;   /* code logique */
  int         os_err; /* errno ou GetLastError */
  char        msg[256];
} vt_err;

VT_ERROR_API const char* vt_err_code_str(vt_err_code c);
VT_ERROR_API const char* vt_err_strerror(int os_err, char* out, size_t cap);

VT_ERROR_API vt_err vt_err_ok(void);
VT_ERROR_API int    vt_err_is_ok(const vt_err* e);
VT_ERROR_API int    vt_err_is_fail(const vt_err* e);

VT_ERROR_API vt_err vt_err_newf(vt_err_code c, const char* fmt, ...) VT_PRINTF(2,3);
VT_ERROR_API vt_err vt_err_wrapf(vt_err base, const char* fmt, ...)  VT_PRINTF(2,3);
VT_ERROR_API vt_err vt_err_from_errno(int e, const char* ctx_fmt, ...) VT_PRINTF(2,3);
#if defined(_WIN32)
VT_ERROR_API vt_err vt_err_from_win32(DWORD e, const char* ctx_fmt, ...) VT_PRINTF(2,3);
#endif

/* Thread-local last error */
VT_ERROR_API void        vt_err_clear_last(void);
VT_ERROR_API void        vt_err_set_last(vt_err e);
VT_ERROR_API const vt_err* vt_err_last(void);

/* Helpers errno */
VT_ERROR_API int  vt_errno_save(void);
VT_ERROR_API void vt_errno_restore(int saved);

#endif /* VT_ERROR_HAVE_HEADER */

/* ----------------------------------------------------------------------------
   Thread-local storage
---------------------------------------------------------------------------- */
#if defined(_WIN32)
#  define VT_TLS __declspec(thread)
#else
#  if defined(__STDC_NO_THREADS__)
#    define VT_TLS __thread
#  else
#    define VT_TLS _Thread_local
#  endif
#endif

static VT_TLS vt_err g_last_err;

/* ----------------------------------------------------------------------------
   Map errno -> vt_err_code
---------------------------------------------------------------------------- */
static vt_err_code vt__map_errno(int e) {
  switch (e) {
    case 0:          return VT_EOK;
#ifdef EINVAL
    case EINVAL:     return VT_EINVAL;
#endif
#ifdef ENOENT
    case ENOENT:     return VT_ENOENT;
#endif
#ifdef EIO
    case EIO:        return VT_EIO;
#endif
#ifdef EPERM
    case EPERM:      return VT_EPERM;
#endif
#ifdef EACCES
    case EACCES:     return VT_EPERM;
#endif
#ifdef EAGAIN
    case EAGAIN:     return VT_EAGAIN;
#endif
#ifdef ENOMEM
    case ENOMEM:     return VT_ENOMEM;
#endif
#ifdef ERANGE
    case ERANGE:     return VT_ERANGE;
#endif
#ifdef EBUSY
    case EBUSY:      return VT_EBUSY;
#endif
#ifdef EEXIST
    case EEXIST:     return VT_EEXIST;
#endif
#ifdef ENOSPC
    case ENOSPC:     return VT_ENOSPC;
#endif
#ifdef EPIPE
    case EPIPE:      return VT_EPIPE;
#endif
#ifdef ETIMEDOUT
    case ETIMEDOUT:  return VT_ETIMEDOUT;
#endif
#ifdef ECANCELED
    case ECANCELED:  return VT_ECANCELED;
#endif
#ifdef ECONNREFUSED
    case ECONNREFUSED: return VT_ECONN;
#endif
#ifdef ECONNRESET
    case ECONNRESET:   return VT_ECONN;
#endif
#ifdef ENETDOWN
    case ENETDOWN:     return VT_ECONN;
#endif
#ifdef ENETUNREACH
    case ENETUNREACH:  return VT_ECONN;
#endif
    default:         return VT_EFAILED;
  }
}

/* ----------------------------------------------------------------------------
   Code -> string
---------------------------------------------------------------------------- */
const char* vt_err_code_str(vt_err_code c) {
  switch (c) {
    case VT_EOK:       return "OK";
    case VT_EINVAL:    return "EINVAL";
    case VT_ENOENT:    return "ENOENT";
    case VT_EIO:       return "EIO";
    case VT_EPERM:     return "EPERM";
    case VT_EAGAIN:    return "EAGAIN";
    case VT_ENOMEM:    return "ENOMEM";
    case VT_ERANGE:    return "ERANGE";
    case VT_EBUSY:     return "EBUSY";
    case VT_EEXIST:    return "EEXIST";
    case VT_ENOSPC:    return "ENOSPC";
    case VT_EPIPE:     return "EPIPE";
    case VT_ECONN:     return "ECONN";
    case VT_ETIMEDOUT: return "ETIMEDOUT";
    case VT_ECANCELED: return "ECANCELED";
    default:           return "EFAILED";
  }
}

/* ----------------------------------------------------------------------------
   strerror portable (thread-safe)
---------------------------------------------------------------------------- */
const char* vt_err_strerror(int os_err, char* out, size_t cap) {
  if (!out || cap == 0) return "";
#if defined(_WIN32)
  DWORD code = (DWORD) (os_err ? os_err : GetLastError());
  DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, code, 0, out, (DWORD)cap, NULL);
  if (!n) {
    (void)snprintf(out, cap, "Win32 error %lu", (unsigned long)code);
  } else {
    /* strip CR/LF */
    for (char* p = out; *p; ++p) if (*p == '\r' || *p == '\n') { *p = 0; break; }
  }
  return out;
#else
  /* POSIX: strerror_r a 2 signatures. On gère les deux. */
  #if ((_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && !defined(__GLIBC__)) \
      || (defined(__APPLE__) && defined(_DARWIN_C_SOURCE))
    /* XSI-compliant: int strerror_r(int, char*, size_t) */
    if (os_err == 0) os_err = errno;
    int rc = strerror_r(os_err, out, cap);
    if (rc != 0) (void)snprintf(out, cap, "errno %d", os_err);
    return out;
  #else
    /* GNU: char* strerror_r(int, char*, size_t) */
    if (os_err == 0) os_err = errno;
    char* s = strerror_r(os_err, out, cap);
    if (s != out) (void)snprintf(out, cap, "%s", s ? s : "unknown");
    return out;
  #endif
#endif
}

/* ----------------------------------------------------------------------------
   Helpers internes
---------------------------------------------------------------------------- */
static void vt__vfmt(char* dst, size_t cap, const char* fmt, va_list ap) {
  if (!dst || cap == 0) return;
  if (!fmt || !*fmt) { dst[0] = 0; return; }
  (void)vsnprintf(dst, cap, fmt, ap);
}

/* ----------------------------------------------------------------------------
   Construction de vt_err
---------------------------------------------------------------------------- */
vt_err vt_err_ok(void) {
  vt_err e; e.code = VT_EOK; e.os_err = 0; e.msg[0] = 0; return e;
}
int vt_err_is_ok(const vt_err* e)   { return e && e->code == VT_EOK; }
int vt_err_is_fail(const vt_err* e) { return !vt_err_is_ok(e); }

vt_err vt_err_newf(vt_err_code c, const char* fmt, ...) {
  vt_err e; e.code = c; e.os_err = 0; e.msg[0] = 0;
  va_list ap; va_start(ap, fmt);
  vt__vfmt(e.msg, sizeof e.msg, fmt, ap);
  va_end(ap);
  return e;
}

vt_err vt_err_wrapf(vt_err base, const char* fmt, ...) {
  vt_err e = base;
  char extra[160] = {0};
  if (fmt && *fmt) {
    va_list ap; va_start(ap, fmt);
    vt__vfmt(extra, sizeof extra, fmt, ap);
    va_end(ap);
  }
  if (extra[0]) {
    if (e.msg[0]) {
      char buf[256];
      (void)snprintf(buf, sizeof buf, "%s: %s", extra, e.msg);
      (void)snprintf(e.msg, sizeof e.msg, "%s", buf);
    } else {
      (void)snprintf(e.msg, sizeof e.msg, "%s", extra);
    }
  }
  return e;
}

vt_err vt_err_from_errno(int e, const char* ctx_fmt, ...) {
  vt_err out; out.code = vt__map_errno(e ? e : errno); out.os_err = e ? e : errno; out.msg[0] = 0;

  /* compose message: "<ctx>: <strerror>" */
  char ctx[128] = {0};
  if (ctx_fmt && *ctx_fmt) {
    va_list ap; va_start(ap, ctx_fmt);
    vt__vfmt(ctx, sizeof ctx, ctx_fmt, ap);
    va_end(ap);
  }
  char serr[128];
  vt_err_strerror(out.os_err, serr, sizeof serr);

  if (ctx[0]) (void)snprintf(out.msg, sizeof out.msg, "%s: %s", ctx, serr);
  else        (void)snprintf(out.msg, sizeof out.msg, "%s", serr);
  return out;
}

#if defined(_WIN32)
vt_err vt_err_from_win32(DWORD e, const char* ctx_fmt, ...) {
  vt_err out; out.code = (e == 0) ? VT_EOK : VT_EFAILED; out.os_err = (int)e; out.msg[0] = 0;

  char ctx[128] = {0};
  if (ctx_fmt && *ctx_fmt) {
    va_list ap; va_start(ap, ctx_fmt);
    vt__vfmt(ctx, sizeof ctx, ctx_fmt, ap);
    va_end(ap);
  }
  char serr[160];
  vt_err_strerror((int)e, serr, sizeof serr);

  if (ctx[0]) (void)snprintf(out.msg, sizeof out.msg, "%s: %s", ctx, serr);
  else        (void)snprintf(out.msg, sizeof out.msg, "%s", serr);

  /* Hints de mapping rudimentaire si besoin */
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: out.code = VT_ENOENT; break;
    case ERROR_ACCESS_DENIED:  out.code = VT_EPERM;  break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:    out.code = VT_EEXIST; break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:    out.code = VT_ENOMEM; break;
    case ERROR_BUSY:           out.code = VT_EBUSY;  break;
    case ERROR_INVALID_PARAMETER: out.code = VT_EINVAL; break;
    case ERROR_DISK_FULL:      out.code = VT_ENOSPC; break;
    case ERROR_BROKEN_PIPE:    out.code = VT_EPIPE;  break;
    case WAIT_TIMEOUT:         out.code = VT_ETIMEDOUT; break;
    default: break;
  }
  return out;
}
#endif

/* ----------------------------------------------------------------------------
   Dernière erreur thread-local
---------------------------------------------------------------------------- */
void vt_err_clear_last(void) { g_last_err = vt_err_ok(); }
void vt_err_set_last(vt_err e) { g_last_err = e.code ? e : vt_err_ok(); }
const vt_err* vt_err_last(void) { return &g_last_err; }

/* ----------------------------------------------------------------------------
   Helpers errno
---------------------------------------------------------------------------- */
int vt_errno_save(void) { return errno; }
void vt_errno_restore(int saved) { errno = saved; }

/* ----------------------------------------------------------------------------
   Section de test (désactivée)
   cc -std=c11 -DVT_ERROR_TEST error.c
---------------------------------------------------------------------------- */
#ifdef VT_ERROR_TEST
#include <stdio.h>
int main(void) {
  vt_err_clear_last();

  FILE* f = fopen("/definitely/not/here", "rb");
  if (!f) {
    vt_err e = vt_err_from_errno(errno, "open");
    vt_err_set_last(e);
  }
  const vt_err* last = vt_err_last();
  printf("last: code=%s os=%d msg=\"%s\"\n",
         vt_err_code_str(last->code), last->os_err, last->msg);

  vt_err custom = vt_err_newf(VT_EINVAL, "bad argument: %s", "size");
  custom = vt_err_wrapf(custom, "parse config");
  printf("wrap: code=%s msg=\"%s\"\n", vt_err_code_str(custom.code), custom.msg);

#if defined(_WIN32)
  vt_err we = vt_err_from_win32(ERROR_FILE_NOT_FOUND, "CreateFileA");
  printf("win:  code=%s os=%d msg=\"%s\"\n", vt_err_code_str(we.code), we.os_err, we.msg);
#endif
  return 0;
}
#endif
