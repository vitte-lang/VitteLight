// SPDX-License-Identifier: GPL-3.0-or-later
//
// dl.c — Cross-platform dynamic loader for Vitte Light (C17)
//
// Features:
//   - vl_dl_open, vl_dl_open_self, vl_dl_close
//   - vl_dl_sym (typed safe-ish getter), vl_dl_sym_ptr
//   - Portable flags: LAZY/NOW, LOCAL/GLOBAL
//   - Path helpers: try extensions and lib prefixes per OS
//   - Thread-local last error string
//
// Depends: includes/auxlib.h (AuxStatus) and optional includes/dl.h
//
// Build:
//   POSIX:   cc -std=c17 -O2 -Wall -Wextra -pedantic -ldl -c dl.c
//   Windows: cl /std:c17 /O2 dl.c
//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <synchapi.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#endif

// ======================================================================
// Public header fallback
// ======================================================================
#ifndef VITTE_LIGHT_INCLUDES_DL_H
#define VITTE_LIGHT_INCLUDES_DL_H 1

typedef struct VlDl VlDl;

typedef enum {
  VL_DL_LAZY = 1 << 0,   // resolve symbols lazily
  VL_DL_NOW = 1 << 1,    // resolve now
  VL_DL_LOCAL = 1 << 2,  // symbols not made available
  VL_DL_GLOBAL = 1 << 3  // symbols made available for subsequently loaded libs
} VlDlFlags;

// Open/close
AuxStatus vl_dl_open(const char *path, int flags, VlDl **out);
AuxStatus vl_dl_open_self(int flags, VlDl **out);  // main program handle
AuxStatus vl_dl_close(VlDl *h);

// Lookup
AuxStatus vl_dl_sym(VlDl *h, const char *name, void **out);
void *vl_dl_sym_ptr(VlDl *h, const char *name);  // convenience

// Helpers
AuxStatus vl_dl_open_ext(const char *stem, int flags,
                         VlDl **out);  // try platform extensions
const char *vl_dl_last_error(void);    // thread-local error string
void vl_dl_clear_error(void);

#endif  // VITTE_LIGHT_INCLUDES_DL_H

// ======================================================================
// Internal
// ======================================================================

struct VlDl {
#if defined(_WIN32)
  HMODULE h;
#else
  void *h;
#endif
};

#if defined(_WIN32)
#define TLS_SPEC __declspec(thread)
#else
#define TLS_SPEC __thread
#endif

static TLS_SPEC char g_dl_err[512];

static void set_errf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_dl_err, sizeof g_dl_err, fmt, ap);
  va_end(ap);
}

const char *vl_dl_last_error(void) { return g_dl_err[0] ? g_dl_err : NULL; }
void vl_dl_clear_error(void) { g_dl_err[0] = 0; }

// ======================================================================
// Platform shims
// ======================================================================

#if defined(_WIN32)

// Map flags
static DWORD map_load_flags(int flags) {
  (void)flags;
  // LAZY/NOW not meaningful. Prefer default search semantics with safe flags.
  return LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#if _WIN32_WINNT >= 0x0602
         | LOAD_LIBRARY_SEARCH_SYSTEM32
#endif
      ;
}

static AuxStatus open_impl(const char *path, int flags, VlDl **out) {
  if (!out) return AUX_EINVAL;
  vl_dl_clear_error();
  *out = NULL;
  if (!path) return AUX_EINVAL;

  DWORD f = map_load_flags(flags);
  HMODULE h = NULL;

  // Prefer LoadLibraryEx with explicit flags when available
  h = LoadLibraryExA(path, NULL, f);
  if (!h) {
    DWORD e = GetLastError();
    LPSTR msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&msg, 0, NULL);
    set_errf("LoadLibraryEx failed (%lu)%s%s", (unsigned long)e,
             msg ? ": " : "", msg ? msg : "");
    if (msg) LocalFree(msg);
    return AUX_EIO;
  }
  VlDl *obj = (VlDl *)calloc(1, sizeof *obj);
  if (!obj) {
    FreeLibrary(h);
    return AUX_ENOMEM;
  }
  obj->h = h;
  *out = obj;
  return AUX_OK;
}

static AuxStatus open_self_impl(int flags, VlDl **out) {
  (void)flags;
  if (!out) return AUX_EINVAL;
  vl_dl_clear_error();
  // Get module of the current process
  HMODULE h = GetModuleHandleA(NULL);
  if (!h) {
    set_errf("GetModuleHandle(NULL) failed");
    return AUX_EIO;
  }
  VlDl *obj = (VlDl *)calloc(1, sizeof *obj);
  if (!obj) return AUX_ENOMEM;
  obj->h = h;
  *out = obj;
  return AUX_OK;
}

static AuxStatus sym_impl(VlDl *h, const char *name, void **out) {
  if (!h || !name || !out) return AUX_EINVAL;
  vl_dl_clear_error();
  FARPROC p = GetProcAddress(h->h, name);
  if (!p) {
    DWORD e = GetLastError();
    set_errf("GetProcAddress('%s') failed (%lu)", name, (unsigned long)e);
    return AUX_EIO;
  }
  *out = (void *)(uintptr_t)p;
  return AUX_OK;
}

static AuxStatus close_impl(VlDl *h) {
  if (!h) return AUX_OK;
  if (h->h) {
    if (!FreeLibrary(h->h)) {
      set_errf("FreeLibrary failed");
      free(h);
      return AUX_EIO;
    }
  }
  free(h);
  return AUX_OK;
}

static int path_has_ext(const char *p) {
  const char *dot = strrchr(p, '.');
  const char *slash = strrchr(p, '\\');
  if (!slash) slash = strrchr(p, '/');
  return dot && (!slash || dot > slash);
}

static AuxStatus try_candidates_win(const char *stem, int flags, VlDl **out) {
  // If stem already has .dll, try as is
  if (path_has_ext(stem)) {
    return open_impl(stem, flags, out);
  }
  // Try "<stem>.dll"
  char buf[PATH_MAX];
  int w = snprintf(buf, sizeof buf, "%s.dll", stem);
  if (w < 0 || (size_t)w >= sizeof buf) return AUX_ERANGE;
  AuxStatus s = open_impl(buf, flags, out);
  if (s == AUX_OK) return s;
  // Also try with "lib<stem>.dll" if stem has no path sep
  if (!strchr(stem, '\\') && !strchr(stem, '/')) {
    w = snprintf(buf, sizeof buf, "lib%s.dll", stem);
    if (w >= 0 && (size_t)w < sizeof buf) {
      s = open_impl(buf, flags, out);
      if (s == AUX_OK) return s;
    }
  }
  return AUX_EIO;
}

#else  // POSIX

static int map_dl_flags(int flags) {
  int f = 0;
  if (flags & VL_DL_NOW) f |= RTLD_NOW;
  if (flags & VL_DL_LAZY) f |= RTLD_LAZY;
  if (flags & VL_DL_GLOBAL) f |= RTLD_GLOBAL;
  if (flags & VL_DL_LOCAL) f |= RTLD_LOCAL;
  if (f == 0) f = RTLD_NOW | RTLD_LOCAL;
  return f;
}

static AuxStatus open_impl(const char *path, int flags, VlDl **out) {
  if (!out) return AUX_EINVAL;
  *out = NULL;
  vl_dl_clear_error();
  if (!path) return AUX_EINVAL;

  void *h = dlopen(path, map_dl_flags(flags));
  if (!h) {
    const char *e = dlerror();
    set_errf("dlopen('%s') failed: %s", path, e ? e : "unknown");
    return AUX_EIO;
  }
  VlDl *obj = (VlDl *)calloc(1, sizeof *obj);
  if (!obj) {
    dlclose(h);
    return AUX_ENOMEM;
  }
  obj->h = h;
  *out = obj;
  return AUX_OK;
}

static AuxStatus open_self_impl(int flags, VlDl **out) {
  if (!out) return AUX_EINVAL;
  *out = NULL;
  vl_dl_clear_error();
  // POSIX: dlopen(NULL, flags) returns the main program handle
  void *h = dlopen(NULL, map_dl_flags(flags));
  if (!h) {
    const char *e = dlerror();
    set_errf("dlopen(NULL) failed: %s", e ? e : "unknown");
    return AUX_EIO;
  }
  VlDl *obj = (VlDl *)calloc(1, sizeof *obj);
  if (!obj) {
    dlclose(h);
    return AUX_ENOMEM;
  }
  obj->h = h;
  *out = obj;
  return AUX_OK;
}

static AuxStatus sym_impl(VlDl *h, const char *name, void **out) {
  if (!h || !name || !out) return AUX_EINVAL;
  vl_dl_clear_error();
  dlerror();  // clear prior
  void *p = dlsym(h->h, name);
  const char *e = dlerror();
  if (e != NULL) {
    set_errf("dlsym('%s') failed: %s", name, e);
    return AUX_EIO;
  }
  *out = p;
  return AUX_OK;
}

static AuxStatus close_impl(VlDl *h) {
  if (!h) return AUX_OK;
  if (h->h) {
    if (dlclose(h->h) != 0) {
      const char *e = dlerror();
      set_errf("dlclose failed: %s", e ? e : "unknown");
      free(h);
      return AUX_EIO;
    }
  }
  free(h);
  return AUX_OK;
}

static int path_has_ext(const char *p) {
  const char *dot = strrchr(p, '.');
  const char *slash = strrchr(p, '/');
  return dot && (!slash || dot > slash);
}

static AuxStatus try_candidates_unix(const char *stem, int flags, VlDl **out) {
  // If explicit path and extension present: try as is
  if (path_has_ext(stem)) return open_impl(stem, flags, out);

  char buf[PATH_MAX];

#if defined(__APPLE__)
  // Try lib<stem>.dylib then <stem>.dylib then lib<stem>.so
  int w = snprintf(buf, sizeof buf, "lib%s.dylib", stem);
  if (w >= 0 && (size_t)w < sizeof buf && open_impl(buf, flags, out) == AUX_OK)
    return AUX_OK;

  w = snprintf(buf, sizeof buf, "%s.dylib", stem);
  if (w >= 0 && (size_t)w < sizeof buf && open_impl(buf, flags, out) == AUX_OK)
    return AUX_OK;

  w = snprintf(buf, sizeof buf, "lib%s.so", stem);
  if (w >= 0 && (size_t)w < sizeof buf && open_impl(buf, flags, out) == AUX_OK)
    return AUX_OK;
#else
  // Linux/BSD: try lib<stem>.so then <stem>.so
  int w = snprintf(buf, sizeof buf, "lib%s.so", stem);
  if (w >= 0 && (size_t)w < sizeof buf && open_impl(buf, flags, out) == AUX_OK)
    return AUX_OK;

  w = snprintf(buf, sizeof buf, "%s.so", stem);
  if (w >= 0 && (size_t)w < sizeof buf && open_impl(buf, flags, out) == AUX_OK)
    return AUX_OK;
#endif

  return AUX_EIO;
}

#endif  // platform split

// ======================================================================
// Public API
// ======================================================================

AuxStatus vl_dl_open(const char *path, int flags, VlDl **out) {
  if (!path || !*path) return AUX_EINVAL;
  return open_impl(path, flags, out);
}

AuxStatus vl_dl_open_self(int flags, VlDl **out) {
  return open_self_impl(flags, out);
}

AuxStatus vl_dl_close(VlDl *h) { return close_impl(h); }

AuxStatus vl_dl_sym(VlDl *h, const char *name, void **out) {
  if (!h || !name || !out) return AUX_EINVAL;
  return sym_impl(h, name, out);
}

void *vl_dl_sym_ptr(VlDl *h, const char *name) {
  void *p = NULL;
  if (vl_dl_sym(h, name, &p) != AUX_OK) return NULL;
  return p;
}

AuxStatus vl_dl_open_ext(const char *stem, int flags, VlDl **out) {
  if (!stem || !*stem || !out) return AUX_EINVAL;
#if defined(_WIN32)
  return try_candidates_win(stem, flags, out);
#else
  return try_candidates_unix(stem, flags, out);
#endif
}

// ======================================================================
// End
// ======================================================================
