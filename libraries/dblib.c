// ldblib.c (stub)
// vitte-light/libraries/dlib.c
// Dynamic library loader (portable) for tools and VM plugins.
// POSIX: dlopen/dlsym/dlclose. Windows: LoadLibrary/GetProcAddress.
// No external deps. Small helpers to try platform extensions.
//
// Public API (header optional; ask for dlib.h if needed):
//   typedef struct VL_DLib VL_DLib;
//   void        vl_dlib_init(VL_DLib *dl);
//   void        vl_dlib_close(VL_DLib *dl);
//   // Open by exact path. now=1 => RTLD_NOW, else RTLD_LAZY (POSIX only)
//   int         vl_dlib_open(VL_DLib *dl, const char *path, int now);
//   // Try platform-specific names for a base (e.g. "mylib" ->
//   libmylib.so/.dylib or mylib.dll) int         vl_dlib_open_best(VL_DLib *dl,
//   const char *base, int now);
//   // Lookup a symbol inside the handle. Returns NULL on error.
//   void*       vl_dlib_sym(VL_DLib *dl, const char *name);
//   // Lookup in the process global scope (RTLD_DEFAULT / main module). May be
//   NULL. void*       vl_dlib_sym_global(const char *name);
//   // Last error string or NULL.
//   const char* vl_dlib_error(const VL_DLib *dl);
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c libraries/dlib.c

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define VL_PATH_SEP \\'\\\\'
#else
#include <dlfcn.h>
#include <unistd.h>
#define VL_PATH_SEP '/'
#endif

#ifndef VL_DLIB_ERRLEN
#define VL_DLIB_ERRLEN 256
#endif

typedef struct VL_DLib {
#if defined(_WIN32)
  HMODULE h;
#else
  void *h;
#endif
  char *path;
  char err[VL_DLIB_ERRLEN];
} VL_DLib;

static void set_err(VL_DLib *dl, const char *fmt, ...) {
  if (!dl) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dl->err, sizeof(dl->err), fmt, ap);
  va_end(ap);
}

void vl_dlib_init(VL_DLib *dl) {
  if (!dl) return;
  memset(dl, 0, sizeof(*dl));
}

static void free_path(char **p) {
  if (*p) {
    free(*p);
    *p = NULL;
  }
}

void vl_dlib_close(VL_DLib *dl) {
  if (!dl) return;
#if defined(_WIN32)
  if (dl->h) FreeLibrary(dl->h);
#else
  if (dl->h) dlclose(dl->h);
#endif
  dl->h = NULL;
  free_path(&dl->path);
  dl->err[0] = '\0';
}

static char *dup_cstr(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *p = (char *)malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

int vl_dlib_open(VL_DLib *dl, const char *path, int now) {
  if (!dl || !path) return 0;
  vl_dlib_close(dl);
#if defined(_WIN32)
  HMODULE h = LoadLibraryA(path);
  if (!h) {
    DWORD ec = GetLastError();
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, ec, 0, (LPSTR)&msg, 0, NULL);
    set_err(dl, "LoadLibrary('%s'): %s", path, msg ? msg : "error");
    if (msg) LocalFree(msg);
    return 0;
  }
  dl->h = h;
#else
  int flags = now ? RTLD_NOW : RTLD_LAZY;
  flags |= RTLD_LOCAL;
  void *h = dlopen(path, flags);
  if (!h) {
    const char *e = dlerror();
    set_err(dl, "dlopen('%s'): %s", path, e ? e : "error");
    return 0;
  }
  dl->h = h;
#endif
  dl->path = dup_cstr(path);
  if (!dl->path) {
    set_err(dl, "OOM path"); /* keep open but record OOM */
  }
  dl->err[0] = '\0';
  return 1;
}

static int ends_with(const char *s, const char *suf) {
  size_t ls = strlen(s), lt = strlen(suf);
  return ls >= lt && memcmp(s + ls - lt, suf, lt) == 0;
}

static int try_open(VL_DLib *dl, const char *p, int now) {
  if (vl_dlib_open(dl, p, now)) return 1;
  return 0;
}

int vl_dlib_open_best(VL_DLib *dl, const char *base, int now) {
  if (!dl || !base) return 0;
  // If base seems to be a path with an extension, try it directly.
  const char *dot = strrchr(base, '.');
  const char *sep1 = strrchr(base, VL_PATH_SEP);
#if defined(_WIN32)
  const char *sep2 = strrchr(base, '/');
  if (sep2 && (!sep1 || sep2 > sep1)) sep1 = sep2;
#endif
  if (dot && (!sep1 || dot > sep1)) {
    return try_open(dl, base, now);
  }

#if defined(__APPLE__)
  char cand1[512];
  snprintf(cand1, sizeof(cand1), "lib%s.dylib", base);
  char cand2[512];
  snprintf(cand2, sizeof(cand2), "%s.dylib", base);
  char cand3[512];
  snprintf(cand3, sizeof(cand3), "lib%s.so", base);
  char cand4[512];
  snprintf(cand4, sizeof(cand4), "%s.so", base);
  if (try_open(dl, cand1, now)) return 1;
  if (try_open(dl, cand2, now)) return 1;
  if (try_open(dl, cand3, now)) return 1;
  if (try_open(dl, cand4, now)) return 1;
#elif defined(_WIN32)
  char cand1[512];
  snprintf(cand1, sizeof(cand1), "%s.dll", base);
  if (try_open(dl, cand1, now)) return 1;
  if (try_open(dl, base, now)) return 1;  // allow explicit full name
#else
  char cand1[512];
  snprintf(cand1, sizeof(cand1), "lib%s.so", base);
  char cand2[512];
  snprintf(cand2, sizeof(cand2), "%s.so", base);
  if (try_open(dl, cand1, now)) return 1;
  if (try_open(dl, cand2, now)) return 1;
#endif
  set_err(dl, "no candidate matched for '%s'", base);
  return 0;
}

void *vl_dlib_sym(VL_DLib *dl, const char *name) {
  if (!dl || !dl->h || !name) return NULL;
  dl->err[0] = '\0';
#if defined(_WIN32)
  FARPROC p = GetProcAddress(dl->h, name);
  if (!p) {
    DWORD ec = GetLastError();
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, ec, 0, (LPSTR)&msg, 0, NULL);
    set_err(dl, "GetProcAddress('%s'): %s", name, msg ? msg : "error");
    if (msg) LocalFree(msg);
    return NULL;
  }
  return (void *)p;
#else
  dlerror();  // clear
  void *p = dlsym(dl->h, name);
  const char *e = dlerror();
  if (e) {
    set_err(dl, "dlsym('%s'): %s", name, e);
    return NULL;
  }
  return p;
#endif
}

void *vl_dlib_sym_global(const char *name) {
  if (!name) return NULL;
#if defined(_WIN32)
  HMODULE hm = GetModuleHandleA(NULL);
  if (!hm) return NULL;
  return (void *)GetProcAddress(hm, name);
#else
  dlerror();
  void *p = dlsym(RTLD_DEFAULT, name);
  (void)dlerror();
  return p;
#endif
}

const char *vl_dlib_error(const VL_DLib *dl) {
  if (!dl) return "invalid";
  return dl->err[0] ? dl->err : NULL;
}

// ───────────────────────── Self-test ─────────────────────────
#ifdef VL_DLIB_TEST_MAIN
int main(int argc, char **argv) {
  VL_DLib dl;
  vl_dlib_init(&dl);
#if defined(_WIN32)
  const char *guess = (argc > 1 ? argv[1] : "kernel32.dll");
  if (!vl_dlib_open_best(&dl, guess, 1)) {
    fprintf(stderr, "open: %s\n", vl_dlib_error(&dl));
    return 1;
  }
  void *sym = vl_dlib_sym(&dl, "GetCurrentProcessId");
#else
  const char *guess = (argc > 1 ? argv[1] : "c");
  if (!vl_dlib_open_best(&dl, guess, 1)) {
    fprintf(stderr, "open: %s\n", vl_dlib_error(&dl));
    return 1;
  }
  void *sym = vl_dlib_sym(&dl, "puts");
#endif
  printf("lib='%s' sym=%p\n", dl.path ? dl.path : "?", sym);
  vl_dlib_close(&dl);
  return 0;
}
#endif
