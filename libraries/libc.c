// vitte-light/libraries/lib.c
// Agrégateur de bibliothèques standards pour VitteLight.
// Rôle: exposer des points d'entrée de registration, sélection
// par nom, et chargement optionnel de plugins dynamiques.
//
// Fonctions exportées:
//   void vl_register_stdlibs(struct VL_Context *ctx);
//   int  vl_register_lib_by_name(struct VL_Context *ctx, const char *name);
//   int  vl_register_libs_from_list(struct VL_Context *ctx, const char *csv);
//   int  vl_register_libs_from_env(struct VL_Context *ctx, const char *envvar);
//   int  vl_lib_load_plugins_from_env(struct VL_Context *ctx, const char
//   *envvar, int now); const char* vl_stdlib_version(void);
//
// Conventions de noms (insensibles à la casse):
//   "base"  -> baselib
//   "io"    -> iolib
//   "math"  -> mathlib
//   "str"   -> strlib
//   "os"    -> oslib
//   "crypto"-> cryptolib
//   "http"  -> curl (libcurl/fallback)
//   "ffi"   -> ffilib
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/lib.c

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"  // VL_Context, vl_register_native(...)

#ifndef VITTE_LIGHT_STDLIB_VERSION
#define VITTE_LIGHT_STDLIB_VERSION "0.1-dev"
#endif

// ───────────────────────── Sélection compile-time ─────────────────────────
#ifndef VL_WITH_BASELIB
#define VL_WITH_BASELIB 1
#endif
#ifndef VL_WITH_IOLIB
#define VL_WITH_IOLIB 1
#endif
#ifndef VL_WITH_MATHLIB
#define VL_WITH_MATHLIB 1
#endif
#ifndef VL_WITH_STRLIB
#define VL_WITH_STRLIB 1
#endif
#ifndef VL_WITH_OSLIB
#define VL_WITH_OSLIB 1
#endif
#ifndef VL_WITH_CRYPTOLIB
#define VL_WITH_CRYPTOLIB 1
#endif
#ifndef VL_WITH_CURL
#define VL_WITH_CURL 1
#endif
#ifndef VL_WITH_FFI
#define VL_WITH_FFI 1
#endif
#ifndef VL_WITH_PLUGINS
#define VL_WITH_PLUGINS 1
#endif

// ───────────────────────── Prototypes d'enregistrement
// ───────────────────────── Chaque lib devrait fournir une fonction
// vl_register_*(VL_Context*).
#if VL_WITH_BASELIB
extern void vl_register_baselib(struct VL_Context *ctx);  // baselib.c
#endif
#if VL_WITH_IOLIB
extern void vl_register_iolib(struct VL_Context *ctx);  // iolib.c
#endif
#if VL_WITH_MATHLIB
extern void vl_register_mathlib(struct VL_Context *ctx);  // mathlib.c
#endif
#if VL_WITH_STRLIB
extern void vl_register_strlib(struct VL_Context *ctx);  // strlib.c
#endif
#if VL_WITH_OSLIB
extern void vl_register_oslib(struct VL_Context *ctx);  // oslib.c
#endif
#if VL_WITH_CRYPTOLIB
extern void vl_register_cryptolib(struct VL_Context *ctx);  // crypto.c
#endif
#if VL_WITH_CURL
extern void vl_register_curl(struct VL_Context *ctx);  // curl.c
#endif
#if VL_WITH_FFI
extern void vl_register_ffi(struct VL_Context *ctx);  // ffi.c
#endif

// ───────────────────────── Plugins dynamiques (dl.c) ─────────────────────────
#if VL_WITH_PLUGINS
struct VL_DL_Manager;  // opaque
extern void vl_dl_mgr_init(struct VL_DL_Manager *m);
extern void vl_dl_mgr_free(struct VL_DL_Manager *m);
extern int vl_dl_plugin_load(struct VL_DL_Manager *m, struct VL_Context *ctx,
                             const char *name, int now);
extern void vl_dl_plugin_unload_all(struct VL_DL_Manager *m,
                                    struct VL_Context *ctx);
#endif

// ───────────────────────── Helpers internes ─────────────────────────
static int ascii_ieq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    unsigned ca = (unsigned char)*a++;
    unsigned cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca += ('a' - 'A');
    if (cb >= 'A' && cb <= 'Z') cb += ('a' - 'A');
    if (ca != cb) return 0;
  }
  return *a == '\0' && *b == '\0';
}
static int ascii_streq_n(const char *a, const char *b, size_t n) {
  if (!a || !b) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned ca = (unsigned char)a[i];
    unsigned cb = (unsigned char)b[i];
    if (ca >= 'A' && ca <= 'Z') ca += ('a' - 'A');
    if (cb >= 'A' && cb <= 'Z') cb += ('a' - 'A');
    if (ca != cb) return 0;
    if (ca == '\0') return 1;
  }
  return 1;
}
static const char *skip_ws(const char *s) {
  while (s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
  return s;
}

// Split par [,;: ] en ignorant espaces; appelle cb(seg, len, ud).
typedef void (*seg_cb)(const char *, size_t, void *);
static void split_list(const char *csv, seg_cb cb, void *ud) {
  if (!csv || !*csv) return;
  const char *p = csv;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',' ||
           *p == ';' || *p == ':')
      p++;
    if (!*p) break;
    const char *b = p;
    while (*p && *p != ',' && *p != ';' && *p != ':' && *p != '\n' &&
           *p != '\r')
      p++;
    size_t n = (size_t)(p - b);  // trim fin
    while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '\t')) n--;
    if (n > 0) cb(b, n, ud);
  }
}

// ───────────────────────── Carte nom → fonction ─────────────────────────
typedef void (*vl_reg_fn)(struct VL_Context *);

typedef struct {
  const char *name;
  vl_reg_fn fn;
} Map;

static int reg_by_key(struct VL_Context *ctx, const char *name) {
  // Aliases courants pour ergonomie.
  static const Map M[] = {
#if VL_WITH_BASELIB
      {"base", vl_register_baselib},
#endif
#if VL_WITH_IOLIB
      {"io", vl_register_iolib},         {"fs", vl_register_iolib},
#endif
#if VL_WITH_MATHLIB
      {"math", vl_register_mathlib},
#endif
#if VL_WITH_STRLIB
      {"str", vl_register_strlib},       {"string", vl_register_strlib},
#endif
#if VL_WITH_OSLIB
      {"os", vl_register_oslib},
#endif
#if VL_WITH_CRYPTOLIB
      {"crypto", vl_register_cryptolib}, {"hash", vl_register_cryptolib},
#endif
#if VL_WITH_CURL
      {"http", vl_register_curl},        {"curl", vl_register_curl},
#endif
#if VL_WITH_FFI
      {"ffi", vl_register_ffi},
#endif
  };
  for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
    if (ascii_ieq(name, M[i].name)) {
      M[i].fn(ctx);
      return 1;
    }
  }
  return 0;
}

// ───────────────────────── API publique ─────────────────────────
void vl_register_stdlibs(struct VL_Context *ctx) {
  if (!ctx) return;
#if VL_WITH_BASELIB
  vl_register_baselib(ctx);
#endif
#if VL_WITH_IOLIB
  vl_register_iolib(ctx);
#endif
#if VL_WITH_MATHLIB
  vl_register_mathlib(ctx);
#endif
#if VL_WITH_STRLIB
  vl_register_strlib(ctx);
#endif
#if VL_WITH_OSLIB
  vl_register_oslib(ctx);
#endif
#if VL_WITH_CRYPTOLIB
  vl_register_cryptolib(ctx);
#endif
#if VL_WITH_CURL
  vl_register_curl(ctx);
#endif
#if VL_WITH_FFI
  vl_register_ffi(ctx);
#endif
}

int vl_register_lib_by_name(struct VL_Context *ctx, const char *name) {
  if (!ctx || !name) return 0;
  return reg_by_key(ctx, name);
}

// csv peut contenir des séparateurs ',', ';', ':', et espaces
static void reg_cb(const char *s, size_t n, void *ud) {
  struct {
    struct VL_Context *ctx;
    int *ok;
  } *U = ud;
  char tmp[64];
  if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
  memcpy(tmp, s, n);
  tmp[n] = '\0';
  if (!reg_by_key(U->ctx, tmp)) *(U->ok) = 0;
}
int vl_register_libs_from_list(struct VL_Context *ctx, const char *csv) {
  if (!ctx || !csv) return 0;
  int ok = 1;
  struct {
    struct VL_Context *ctx;
    int *ok;
  } ud = {ctx, &ok};
  split_list(csv, reg_cb, &ud);
  return ok;
}

int vl_register_libs_from_env(struct VL_Context *ctx, const char *envvar) {
  if (!ctx) return 0;
  const char *ev = getenv(envvar ? envvar : "VITTE_LIBS");
  if (!ev || !*ev) return 1;
  return vl_register_libs_from_list(ctx, ev);
}

const char *vl_stdlib_version(void) { return VITTE_LIGHT_STDLIB_VERSION; }

#if VL_WITH_PLUGINS
// ex: VITTE_PLUGINS="mylib;otherlib" -> charge via dl.c
static void plug_cb(const char *s, size_t n, void *ud) {
  struct {
    struct VL_Context *ctx;
    int now;
    int *ok;
  } *U = ud;
  char tmp[256];
  if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
  memcpy(tmp, s, n);
  tmp[n] = '\0';
  struct VL_DL_Manager M;
  vl_dl_mgr_init(&M);
  int r = vl_dl_plugin_load(&M, U->ctx, tmp, U->now);
  if (!r) *(U->ok) = 0;
  vl_dl_plugin_unload_all(&M, U->ctx);
  vl_dl_mgr_free(&M);
}
int vl_lib_load_plugins_from_env(struct VL_Context *ctx, const char *envvar,
                                 int now) {
  if (!ctx) return 0;
  const char *ev = getenv(envvar ? envvar : "VITTE_PLUGINS");
  if (!ev || !*ev) return 1;
  int ok = 1;
  struct {
    struct VL_Context *ctx;
    int now;
    int *ok;
  } ud = {ctx, now ? 1 : 0, &ok};
  split_list(ev, plug_cb, &ud);
  return ok;
}
#else
int vl_lib_load_plugins_from_env(struct VL_Context *ctx, const char *envvar,
                                 int now) {
  (void)ctx;
  (void)envvar;
  (void)now;
  return 0;
}
#endif

// ───────────────────────── Self‑test (optionnel) ─────────────────────────
#ifdef VL_LIB_TEST_MAIN
int main(void) {
  (void)ascii_streq_n;  // keep static linked
  printf("stdlib version: %s\n", vl_stdlib_version());
  // Pas de VM ici; l'API est destinée à être appelée par l'init runtime.
  return 0;
}
#endif
