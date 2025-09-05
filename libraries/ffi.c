// vitte-light/libraries/dl.c
// Gestionnaire de bibliothèques dynamiques et plugins pour VitteLight.
// Surcouche haut niveau: chemins de recherche, nommage portable, cache,
// init/fini des plugins, erreurs détaillées. C99, sans dépendances externes.
//
// API (demande dl.h si besoin d'un en-tête public):
//   typedef struct VL_DL VL_DL;                // handle opaque d'une DSO
//   typedef struct VL_DL_Manager VL_DL_Manager;// gestionnaire
//   void vl_dl_mgr_init(VL_DL_Manager *m);
//   void vl_dl_mgr_free(VL_DL_Manager *m);
//   void vl_dl_paths_reset(VL_DL_Manager *m);
//   int  vl_dl_paths_add(VL_DL_Manager *m, const char *dir);
//   int  vl_dl_set_env_paths(VL_DL_Manager *m, const char *envvar); // parse
//   ':' ou ';' VL_DL* vl_dl_open_best(VL_DL_Manager *m, const char *name, int
//   now); void   vl_dl_close(VL_DL *h); void*  vl_dl_sym(VL_DL *h, const char
//   *name); const char* vl_dl_last_error(const VL_DL_Manager *m); // NULL si
//   aucune
//
//   // Plugins: convention symboles
//   //   int  vl_plugin_init(struct VL_Context*);
//   //   void vl_plugin_fini(struct VL_Context*); // optionnel
//   int  vl_dl_plugin_load(VL_DL_Manager *m, struct VL_Context *ctx,
//                          const char *name, int now);
//   void vl_dl_plugin_unload_all(VL_DL_Manager *m, struct VL_Context *ctx);
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/dl.c

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"  // VL_Context

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VL_PATH_SEP '\\'
#define VL_PATH_SEP_STR "\\"
#else
#include <dlfcn.h>
#include <unistd.h>
#define VL_PATH_SEP '/'
#define VL_PATH_SEP_STR "/"
#endif

#ifndef VL_DL_ERRLEN
#define VL_DL_ERRLEN 256
#endif

// ───────────────────────── Utils ─────────────────────────
static size_t vl_strlcpy(char *dst, const char *src, size_t n) {
  size_t L = src ? strlen(src) : 0;
  if (n) {
    size_t m = (L >= n) ? n - 1 : L;
    if (m) memcpy(dst, src, m);
    dst[m] = '\0';
  }
  return L;
}
static size_t vl_strlcat(char *dst, const char *src, size_t n) {
  size_t d = dst ? strlen(dst) : 0;
  if (d >= n) return d + (src ? strlen(src) : 0);
  return d + vl_strlcpy(dst + d, src, n - d);
}
static int is_pathsep(char c) { return c == '/' || c == '\\'; }
static char *dup_cstr(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *p = (char *)malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

// ───────────────────────── OS glue ─────────────────────────
typedef struct VL_DL {
#if defined(_WIN32)
  HMODULE h;
#else
  void *h;
#endif
  char *path;  // canonicalisé si possible
  unsigned refc;
} VL_DL;

static void *os_dlsym(VL_DL *h, const char *name) {
  if (!h || !h->h || !name) return NULL;
#if defined(_WIN32)
  FARPROC p = GetProcAddress(h->h, name);
  return (void *)p;
#else
  dlerror();
  void *p = dlsym(h->h, name);
  (void)dlerror();
  return p;
#endif
}

static int os_dlopen_exact(VL_DL *h, const char *path, int now, char *err,
                           size_t errn) {
  if (!h || !path) return 0;
  memset(h, 0, sizeof(*h));
#if defined(_WIN32)
  HMODULE mh = LoadLibraryA(path);
  if (!mh) {
    DWORD ec = GetLastError();
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, ec, 0, (LPSTR)&msg, 0, NULL);
    if (err && errn) {
      snprintf(err, errn, "LoadLibrary('%s'): %s", path, msg ? msg : "error");
    }
    if (msg) LocalFree(msg);
    return 0;
  }
  h->h = mh;
#else
  int flags = now ? RTLD_NOW : RTLD_LAZY;
  flags |= RTLD_LOCAL;
  void *mh = dlopen(path, flags);
  if (!mh) {
    const char *e = dlerror();
    if (err && errn) vl_strlcpy(err, e ? e : "dlopen error", errn);
    return 0;
  }
  h->h = mh;
#endif
  h->path = dup_cstr(path);
  h->refc = 1;
  return 1;
}

static void os_dlclose(VL_DL *h) {
  if (!h || !h->h) return;
#if defined(_WIN32)
  FreeLibrary(h->h);
#else
  dlclose(h->h);
#endif
  h->h = NULL;
  free(h->path);
  h->path = NULL;
  h->refc = 0;
}

// ───────────────────────── Manager ─────────────────────────
typedef struct VL_DL_Plugin {
  VL_DL *dl;
  char *name;
  void (*fini)(struct VL_Context *);
} VL_DL_Plugin;

typedef struct VL_DL_Manager {
  VL_DL **open;
  size_t n_open, cap_open;
  char **paths;
  size_t n_paths, cap_paths;
  VL_DL_Plugin *pl;
  size_t n_pl, cap_pl;
  char err[VL_DL_ERRLEN];
} VL_DL_Manager;

static int ensure_cap(void **ptr, size_t *cap, size_t need, size_t esz) {
  if (need <= *cap) return 1;
  size_t nc = (*cap) ? (*cap * 2) : 8;
  while (nc < need) nc += nc / 2;
  void *np = realloc(*ptr, nc * esz);
  if (!np) return 0;
  *ptr = np;
  *cap = nc;
  return 1;
}

static void set_err(VL_DL_Manager *m, const char *fmt, ...) {
  if (!m) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m->err, sizeof(m->err), fmt, ap);
  va_end(ap);
}

void vl_dl_mgr_init(VL_DL_Manager *m) {
  if (!m) return;
  memset(m, 0, sizeof(*m));
  m->err[0] = '\0';
}

void vl_dl_mgr_free(VL_DL_Manager *m) {
  if (!m) return;
  for (size_t i = 0; i < m->n_open; i++) {
    os_dlclose(m->open[i]);
    free(m->open[i]);
  }
  free(m->open);
  m->open = NULL;
  m->n_open = m->cap_open = 0;
  for (size_t i = 0; i < m->n_paths; i++) {
    free(m->paths[i]);
  }
  free(m->paths);
  m->paths = NULL;
  m->n_paths = m->cap_paths = 0;
  for (size_t i = 0; i < m->n_pl; i++) {
    free(m->pl[i].name);
  }
  free(m->pl);
  m->pl = NULL;
  m->n_pl = m->cap_pl = 0;
  m->err[0] = '\0';
}

const char *vl_dl_last_error(const VL_DL_Manager *m) {
  if (!m) return "invalid";
  return m->err[0] ? m->err : NULL;
}

void vl_dl_paths_reset(VL_DL_Manager *m) {
  if (!m) return;
  for (size_t i = 0; i < m->n_paths; i++) free(m->paths[i]);
  m->n_paths = 0;
}

int vl_dl_paths_add(VL_DL_Manager *m, const char *dir) {
  if (!m || !dir || !*dir) return 0;
  if (!ensure_cap((void **)&m->paths, &m->cap_paths, m->n_paths + 1,
                  sizeof(char *)))
    return 0;
  m->paths[m->n_paths++] = dup_cstr(dir);
  return 1;
}

static int split_next(const char **ps, char *out, size_t n) {
  const char *s = *ps;
  if (!s || !*s) return 0;
  size_t k = 0;
  while (*s && *s != ':' && *s != ';') {
    if (k + 1 < n) out[k++] = *s;
    s++;
  }
  out[k] = '\0';
  if (*s == ':' || *s == ';') s++;
  *ps = s;
  return k > 0;
}

int vl_dl_set_env_paths(VL_DL_Manager *m, const char *envvar) {
  if (!m) return 0;
  if (!envvar || !*envvar) envvar = "VITTE_PLUGINS";
  const char *v = getenv(envvar);
  if (!v || !*v) return 1;
  char buf[512];
  while (split_next(&v, buf, sizeof(buf))) {
    vl_dl_paths_add(m, buf);
  }
  return 1;
}

// ───────────────────────── Nomination portable ─────────────────────────
static int has_ext(const char *name) {
  const char *d = strrchr(name, '.');
  const char *s1 = strrchr(name, '/');
  const char *s2 = strrchr(name, '\\');
  const char *s = (s1 && s2) ? (s1 > s2 ? s1 : s2) : (s1 ? s1 : s2);
  return d && (!s || d > s);
}
static int has_sep(const char *name) {
  for (const char *p = name; *p; ++p)
    if (is_pathsep(*p)) return 1;
  return 0;
}

static void make_candidates(const char *base, char cand[][256], size_t *out_n) {
  size_t n = 0;
#if defined(__APPLE__)
  snprintf(cand[n++], 256, "lib%s.dylib", base);
  snprintf(cand[n++], 256, "%s.dylib", base);
  snprintf(cand[n++], 256, "lib%s.so", base);
  snprintf(cand[n++], 256, "%s.so", base);
#elif defined(_WIN32)
  snprintf(cand[n++], 256, "%s.dll", base);
  snprintf(cand[n++], 256, "%s", base);
#else
  snprintf(cand[n++], 256, "lib%s.so", base);
  snprintf(cand[n++], 256, "%s.so", base);
#endif
  *out_n = n;
}

static int join_path(char *out, size_t n, const char *dir, const char *file) {
  if (!out || n == 0) return 0;
  out[0] = '\0';
  if (!dir || !*dir) {
    return (int)(vl_strlcpy(out, file ? file : "", n) < n);
  }
  vl_strlcpy(out, dir, n);
  size_t L = strlen(out);
  if (L && out[L - 1] != VL_PATH_SEP && out[L - 1] != '/')
    vl_strlcat(out, VL_PATH_SEP_STR, n);
  vl_strlcat(out, file ? file : "", n);
  return (int)(strlen(out) < n);
}

// ───────────────────────── Open/search ─────────────────────────
static VL_DL *mgr_cache_get(VL_DL_Manager *m, const char *path) {
  for (size_t i = 0; i < m->n_open; i++) {
    if (m->open[i]->path && strcmp(m->open[i]->path, path) == 0) {
      m->open[i]->refc++;
      return m->open[i];
    }
  }
  return NULL;
}
static int mgr_cache_put(VL_DL_Manager *m, VL_DL *h) {
  if (!ensure_cap((void **)&m->open, &m->cap_open, m->n_open + 1,
                  sizeof(VL_DL *)))
    return 0;
  m->open[m->n_open++] = h;
  return 1;
}

VL_DL *vl_dl_open_best(VL_DL_Manager *m, const char *name, int now) {
  if (!m || !name || !*name) return NULL;
  m->err[0] = '\0';
  // Chemin explicite ou nom avec extension => essai direct
  if (has_sep(name) || has_ext(name)) {
    VL_DL *h = (VL_DL *)calloc(1, sizeof(VL_DL));
    if (!h) {
      set_err(m, "OOM");
      return NULL;
    }
    if (!os_dlopen_exact(h, name, now, m->err, sizeof(m->err))) {
      free(h);
      return NULL;
    }
    if (!mgr_cache_put(m, h)) {
      os_dlclose(h);
      free(h);
      set_err(m, "OOM");
      return NULL;
    }
    return h;
  }
  // Construire candidats + chemins
  char cand[6][256];
  size_t cn = 0;
  make_candidates(name, cand, &cn);
  // 1) chemins configurés
  for (size_t d = 0; d < m->n_paths; d++) {
    for (size_t i = 0; i < cn; i++) {
      char full[512];
      join_path(full, sizeof(full), m->paths[d], cand[i]);
      VL_DL *cached = mgr_cache_get(m, full);
      if (cached) return cached;
      VL_DL *h = (VL_DL *)calloc(1, sizeof(VL_DL));
      if (!h) {
        set_err(m, "OOM");
        return NULL;
      }
      if (os_dlopen_exact(h, full, now, m->err, sizeof(m->err))) {
        if (!mgr_cache_put(m, h)) {
          os_dlclose(h);
          free(h);
          set_err(m, "OOM");
          return NULL;
        }
        return h;
      }
      free(h);
    }
  }
  // 2) répertoire courant
  for (size_t i = 0; i < cn; i++) {
    VL_DL *h = (VL_DL *)calloc(1, sizeof(VL_DL));
    if (!h) {
      set_err(m, "OOM");
      return NULL;
    }
    if (os_dlopen_exact(h, cand[i], now, m->err, sizeof(m->err))) {
      if (!mgr_cache_put(m, h)) {
        os_dlclose(h);
        free(h);
        set_err(m, "OOM");
        return NULL;
      }
      return h;
    }
    free(h);
  }
  if (m->err[0] == '\0') set_err(m, "not found: %s", name);
  return NULL;
}

void vl_dl_close(VL_DL *h) {
  if (!h) return;
  if (h->refc > 1) {
    h->refc--;
    return;
  }
  os_dlclose(h);
  free(h);
}

void *vl_dl_sym(VL_DL *h, const char *name) { return os_dlsym(h, name); }

// ───────────────────────── Plugins ─────────────────────────
int vl_dl_plugin_load(VL_DL_Manager *m, struct VL_Context *ctx,
                      const char *name, int now) {
  if (!m || !ctx || !name) return 0;
  VL_DL *h = vl_dl_open_best(m, name, now);
  if (!h) return 0;
  typedef int (*initfn)(struct VL_Context *);
  typedef void (*finifn)(struct VL_Context *);
  initfn init = (initfn)vl_dl_sym(h, "vl_plugin_init");
  if (!init) {
    set_err(m, "symbol vl_plugin_init not found in %s",
            h->path ? h->path : name);
    vl_dl_close(h);
    return 0;
  }
  finifn fini = (finifn)vl_dl_sym(h, "vl_plugin_fini");
  int ok = init(ctx);
  if (!ok) {
    set_err(m, "vl_plugin_init failed in %s", h->path ? h->path : name);
    vl_dl_close(h);
    return 0;
  }
  // Enregistrer pour déchargement ultérieur
  if (!ensure_cap((void **)&m->pl, &m->cap_pl, m->n_pl + 1,
                  sizeof(VL_DL_Plugin))) {
    set_err(m, "OOM");
    return 0;
  }
  m->pl[m->n_pl].dl = h;
  m->pl[m->n_pl].name = dup_cstr(name);
  m->pl[m->n_pl].fini = fini;
  m->n_pl++;
  return 1;
}

void vl_dl_plugin_unload_all(VL_DL_Manager *m, struct VL_Context *ctx) {
  if (!m) return;
  for (size_t i = m->n_pl; i > 0; i--) {
    VL_DL_Plugin *p = &m->pl[i - 1];
    if (p->fini) p->fini(ctx);
    if (p->dl) {
      vl_dl_close(p->dl);
      p->dl = NULL;
    }
    free(p->name);
    p->name = NULL;
  }
  m->n_pl = 0;
}

// ───────────────────────── Autotest (optionnel) ─────────────────────────
#ifdef VL_DL_TEST_MAIN
#include <inttypes.h>
int main(int argc, char **argv) {
  VL_DL_Manager M;
  vl_dl_mgr_init(&M);
  vl_dl_set_env_paths(&M, NULL);
  if (argc > 2) vl_dl_paths_add(&M, argv[2]);
  VL_DL *h = vl_dl_open_best(&M, argc > 1 ? argv[1] : "c", 1);
  if (!h) {
    fprintf(stderr, "open: %s\n", vl_dl_last_error(&M));
    vl_dl_mgr_free(&M);
    return 1;
  }
#if defined(_WIN32)
  void *sym = vl_dl_sym(h, "GetCurrentProcessId");
#else
  void *sym = vl_dl_sym(h, "puts");
#endif
  printf("lib='%s' sym=%p refc=%u\n", h->path ? h->path : "?", sym, h->refc);
  vl_dl_close(h);
  vl_dl_mgr_free(&M);
  return 0;
}
#endif
