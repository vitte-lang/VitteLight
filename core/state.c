/* ============================================================================
   state.c — État global du compilateur/VM Vitte/Vitl
   - Gestion de configuration, interning, sources, parsing, diagnostics.
   - Thread-safe (mutex léger). C17. UTF-8. Licence MIT.
   - Dépendances facultatives: mem.h (arène), gc.h (GC), lex.h, parser.h,
   debug.h.
   - Se compile même sans state.h via définitions locales (voir blocs #ifndef).
   ============================================================================
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
   En-têtes facultatifs du projet
--------------------------------------------------------------------------- */
#ifdef __has_include
#if __has_include("debug.h")
#include "debug.h"
#endif
#if __has_include("mem.h")
#include "mem.h"
#endif
#if __has_include("gc.h")
#include "gc.h"
#endif
#if __has_include("lex.h")
#include "lex.h"
#endif
#if __has_include("parser.h")
#include "parser.h"
#endif
#if __has_include("state.h")
#include "state.h"
#endif
#endif

/* ---------------------------------------------------------------------------
   Sentinelles en l’absence de state.h (API publique minimale)
--------------------------------------------------------------------------- */
#ifndef VT_STATE_H_SENTINEL
typedef struct vt_state vt_state;

typedef struct vt_state_config {
  int log_level;                  /* 0..5 → TRACE..FATAL */
  int use_color;                  /* 1=SGR */
  const char* module_search_path; /* ex: "std:.;lib" */
  size_t arena_reserve; /* octets arène (si mem.h non présent ignore) */
  size_t interner_init; /* nb entrées initial */
} vt_state_config;

/* Fonctions clés exposées par state.c */
vt_state* vt_state_create(const vt_state_config* cfg);
void vt_state_destroy(vt_state* st);

int vt_state_add_source(vt_state* st, const char* path,
                        const char* contents_opt_utf8);
int vt_state_parse_all(vt_state* st); /* renvoie 0 si OK */
void vt_state_dump_ast(FILE* out, vt_state* st);

const char* vt_intern_cstr(vt_state* st, const char* s);
size_t vt_intern_id(vt_state* st, const char* s, size_t n);
#endif /* VT_STATE_H_SENTINEL */

/* ---------------------------------------------------------------------------
   Log macros fallback
--------------------------------------------------------------------------- */
#ifndef VT_DEBUG_H
#define VT_TRACE(...) ((void)0)
#define VT_DEBUG(...) ((void)0)
#define VT_INFO(...) ((void)0)
#define VT_WARN(...) ((void)0)
#define VT_ERROR(...) ((void)0)
#define VT_FATAL(...)                          \
  do {                                         \
    fprintf(stderr, "[FATAL] state: abort\n"); \
    abort();                                   \
  } while (0)
#endif

/* ---------------------------------------------------------------------------
   Arène fallback (très simple) si mem.h absent
--------------------------------------------------------------------------- */
#ifndef VT_MEM_H_SENTINEL
typedef struct vt_arena {
  unsigned char* base;
  size_t used;
  size_t cap;
} vt_arena;

static vt_arena* arena_create(size_t cap) {
  if (cap < (1u << 16)) cap = (1u << 16);
  vt_arena* a = (vt_arena*)calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->base = (unsigned char*)malloc(cap);
  if (!a->base) {
    free(a);
    return NULL;
  }
  a->cap = cap;
  a->used = 0;
  return a;
}
static void arena_destroy(vt_arena* a) {
  if (!a) return;
  free(a->base);
  free(a);
}
static void* arena_alloc(vt_arena* a, size_t n, size_t align) {
  size_t off = (a->used + (align - 1)) & ~(align - 1);
  if (off + n > a->cap) {
    size_t ncap = a->cap * 2;
    while (off + n > ncap) ncap *= 2;
    unsigned char* nb = (unsigned char*)realloc(a->base, ncap);
    if (!nb) return NULL;
    a->base = nb;
    a->cap = ncap;
  }
  void* p = a->base + off;
  a->used = off + n;
  return p;
}
static char* arena_strdup(vt_arena* a, const char* s) {
  size_t n = strlen(s);
  char* p = (char*)arena_alloc(a, n + 1, 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}
#endif

/* Uniformisation du type d’arène côté parser.h */
#ifdef VT_PARSER_H_SENTINEL
typedef parser_arena_t vt_parser_arena_t;
#else
typedef vt_arena vt_parser_arena_t;
#endif

/* ---------------------------------------------------------------------------
   Mutex portable
--------------------------------------------------------------------------- */
typedef struct vt_mutex {
#ifdef _WIN32
  CRITICAL_SECTION cs;
#else
  pthread_mutex_t mtx;
#endif
} vt_mutex;

static int vt_mutex_init(vt_mutex* m) {
#ifdef _WIN32
  InitializeCriticalSection(&m->cs);
  return 0;
#else
  return pthread_mutex_init(&m->mtx, NULL);
#endif
}
static void vt_mutex_destroy(vt_mutex* m) {
#ifdef _WIN32
  DeleteCriticalSection(&m->cs);
#else
  pthread_mutex_destroy(&m->mtx);
#endif
}
static void vt_mutex_lock(vt_mutex* m) {
#ifdef _WIN32
  EnterCriticalSection(&m->cs);
#else
  pthread_mutex_lock(&m->mtx);
#endif
}
static void vt_mutex_unlock(vt_mutex* m) {
#ifdef _WIN32
  LeaveCriticalSection(&m->cs);
#else
  pthread_mutex_unlock(&m->mtx);
#endif
}

/* ---------------------------------------------------------------------------
   Utils
--------------------------------------------------------------------------- */
static void* xmalloc(size_t n) {
  void* p = malloc(n);
  if (!p) {
    VT_FATAL("OOM");
  }
  return p;
}
static void* xcalloc(size_t n, size_t s) {
  void* p = calloc(n, s);
  if (!p) {
    VT_FATAL("OOM");
  }
  return p;
}
static char* xstrdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* p = (char*)xmalloc(n);
  memcpy(p, s, n);
  return p;
}

/* FNV-1a 64 */
static uint64_t fnv1a(const void* key, size_t len) {
  const unsigned char* p = (const unsigned char*)key;
  uint64_t h = 1469598103934665603ull;
  while (len--) {
    h ^= *p++;
    h *= 1099511628211ull;
  }
  return h ? h : 0x9e3779b97f4a7c15ull;
}

/* Lecture de fichier entier (UTF-8 non vérifié) */
static char* read_all_utf8(const char* path, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char* buf = (char*)xmalloc((size_t)n + 1);
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[rd] = 0;
  if (out_len) *out_len = rd;
  return buf;
}

/* Normalisation sommaire des chemins: remplace '\\' → '/' */
static void path_normalize(char* s) {
  for (; *s; ++s)
    if (*s == '\\') *s = '/';
}

/* ---------------------------------------------------------------------------
   Interner (open addressing, power-of-two capacity)
--------------------------------------------------------------------------- */
typedef struct intern_entry {
  const char* s;
  size_t len;
  uint64_t h;
} intern_entry;

typedef struct interner {
  intern_entry* tab;
  size_t cap;      /* power of two, >= 8 */
  size_t len;      /* occupancy */
  vt_arena* store; /* stockage des bytes (arène locale) */
} interner;

static size_t round_pow2(size_t x) {
  size_t p = 8;
  while (p < x) p <<= 1;
  return p;
}

static interner* interner_create(size_t init_cap) {
  interner* in = (interner*)xcalloc(1, sizeof(*in));
  in->cap = round_pow2(init_cap ? init_cap : 64);
  in->tab = (intern_entry*)xcalloc(in->cap, sizeof(intern_entry));
#ifdef VT_MEM_H_SENTINEL
  in->store = vt_arena_create(in->cap * 16);
#else
  in->store = arena_create(in->cap * 16);
#endif
  if (!in->store) VT_FATAL("OOM interner arena");
  return in;
}
static void interner_destroy(interner* in) {
  if (!in) return;
  free(in->tab);
#ifdef VT_MEM_H_SENTINEL
  vt_arena_destroy(in->store);
#else
  arena_destroy(in->store);
#endif
  free(in);
}
static void interner_rehash(interner* in, size_t ncap) {
  intern_entry* n = (intern_entry*)xcalloc(ncap, sizeof(*n));
  for (size_t i = 0; i < in->cap; i++) {
    intern_entry e = in->tab[i];
    if (!e.s) continue;
    size_t mask = ncap - 1;
    size_t j = (size_t)e.h & mask;
    while (n[j].s) j = (j + 1) & mask;
    n[j] = e;
  }
  free(in->tab);
  in->tab = n;
  in->cap = ncap;
}
static const char* interner_put(interner* in, const char* s, size_t len,
                                uint64_t h) {
  if ((in->len * 10) >= (in->cap * 7)) { /* 70% */
    interner_rehash(in, in->cap << 1);
  }
  size_t mask = in->cap - 1;
  size_t i = (size_t)h & mask;
  while (in->tab[i].s) {
    if (in->tab[i].h == h && in->tab[i].len == len &&
        memcmp(in->tab[i].s, s, len) == 0) {
      return in->tab[i].s;
    }
    i = (i + 1) & mask;
  }
#ifdef VT_MEM_H_SENTINEL
  char* dst = (char*)vt_arena_alloc(in->store, len + 1, 1);
#else
  char* dst = (char*)arena_alloc(in->store, len + 1, 1);
#endif
  if (!dst) VT_FATAL("OOM interner store");
  memcpy(dst, s, len);
  dst[len] = 0;
  in->tab[i].s = dst;
  in->tab[i].len = len;
  in->tab[i].h = h;
  in->len++;
  return dst;
}
static const char* interner_get(interner* in, const char* s, size_t len,
                                uint64_t h) {
  size_t mask = in->cap - 1;
  size_t i = (size_t)h & mask;
  while (in->tab[i].s) {
    if (in->tab[i].h == h && in->tab[i].len == len &&
        memcmp(in->tab[i].s, s, len) == 0) {
      return in->tab[i].s;
    }
    i = (i + 1) & mask;
  }
  return NULL;
}

/* ---------------------------------------------------------------------------
   Source + modules + diagnostics
--------------------------------------------------------------------------- */
typedef struct vt_source {
  const char* path; /* interné */
  char* text;       /* propriété state */
  size_t size;      /* bytes */
#ifdef VT_PARSER_H_SENTINEL
  vt_parse_result parse; /* AST et diags */
#else
  void* parse; /* opaque si parser absent */
#endif
  int loaded; /* 1 si text != NULL */
} vt_source;

typedef struct vec_src {
  vt_source* data;
  size_t len, cap;
} vec_src;

static void vec_src_push(vec_src* v, vt_source s) {
  if (v->len == v->cap) {
    size_t ncap = v->cap ? v->cap * 2 : 8;
    v->data = (vt_source*)realloc(v->data, ncap * sizeof(*v->data));
    if (!v->data) VT_FATAL("OOM vec_src");
    v->cap = ncap;
  }
  v->data[v->len++] = s;
}

/* ---------------------------------------------------------------------------
   État global
--------------------------------------------------------------------------- */
struct vt_state {
  vt_state_config cfg;
  vt_mutex lock;

  /* Allocateurs */
#ifdef VT_MEM_H_SENTINEL
  vt_arena* arena; /* utilitaires internes */
#else
  vt_arena* arena;
#endif
#ifdef VT_GC_H_SENTINEL
  vt_gc* gc; /* optionnel */
#endif

  /* Interner */
  interner* atoms;

  /* Sources */
  vec_src sources;

  /* Stats */
  size_t n_parsed;
  size_t n_errors;
};

/* ---------------------------------------------------------------------------
   API: création/destruction
--------------------------------------------------------------------------- */
vt_state* vt_state_create(const vt_state_config* user_cfg) {
  vt_state_config dflt = {/* log_level */ 1, /* DEBUG */
                          /* use_color */ 1,
                          /* module_search_path */ "std:.;lib",
                          /* arena_reserve */ (size_t)1 << 20,
                          /* interner_init */ 256};
  vt_state* st = (vt_state*)xcalloc(1, sizeof(*st));
  st->cfg = user_cfg ? *user_cfg : dflt;

  if (vt_mutex_init(&st->lock) != 0) {
    free(st);
    return NULL;
  }

#ifdef VT_MEM_H_SENTINEL
  st->arena = vt_arena_create(st->cfg.arena_reserve ? st->cfg.arena_reserve
                                                    : ((size_t)1 << 20));
#else
  st->arena = arena_create(st->cfg.arena_reserve ? st->cfg.arena_reserve
                                                 : ((size_t)1 << 20));
#endif
  if (!st->arena) {
    vt_mutex_destroy(&st->lock);
    free(st);
    return NULL;
  }

  st->atoms = interner_create(st->cfg.interner_init);

#ifdef VT_GC_H_SENTINEL
  st->gc = vt_gc_create(/*heap_hint*/ 0);
#endif

#ifdef VT_DEBUG_H
  vt_log_config lc = {.level = (vt_log_level)(st->cfg.log_level),
                      .format = VT_FMT_TEXT,
                      .use_color = st->cfg.use_color,
                      .file_path = NULL,
                      .rotate_bytes = 0,
                      .capture_crash = 0};
  vt_log_init(&lc);
  VT_INFO("vt_state: created");
#endif
  return st;
}

void vt_state_destroy(vt_state* st) {
  if (!st) return;

  vt_mutex_lock(&st->lock);

  /* Free sources */
  for (size_t i = 0; i < st->sources.len; i++) {
    vt_source* s = &st->sources.data[i];
#ifdef VT_PARSER_H_SENTINEL
    if (s->parse.arena) {
      vt_parse_free(&s->parse);
    }
#endif
    free(s->text);
  }
  free(st->sources.data);

  /* Interner + allocs */
  interner_destroy(st->atoms);

#ifdef VT_GC_H_SENTINEL
  vt_gc_destroy(st->gc);
#endif

#ifdef VT_MEM_H_SENTINEL
  vt_arena_destroy(st->arena);
#else
  arena_destroy(st->arena);
#endif

  vt_mutex_unlock(&st->lock);
  vt_mutex_destroy(&st->lock);

#ifdef VT_DEBUG_H
  VT_INFO("vt_state: destroyed");
  vt_log_shutdown();
#endif

  free(st);
}

/* ---------------------------------------------------------------------------
   Interner API publique
--------------------------------------------------------------------------- */
const char* vt_intern_cstr(vt_state* st, const char* s) {
  if (!s) return "";
  size_t n = strlen(s);
  uint64_t h = fnv1a(s, n);
  vt_mutex_lock(&st->lock);
  const char* out = interner_get(st->atoms, s, n, h);
  if (!out) out = interner_put(st->atoms, s, n, h);
  vt_mutex_unlock(&st->lock);
  return out;
}

size_t vt_intern_id(vt_state* st, const char* s, size_t n) {
  if (!s) return 0;
  uint64_t h = fnv1a(s, n);
  vt_mutex_lock(&st->lock);
  const char* out = interner_get(st->atoms, s, n, h);
  if (!out) out = interner_put(st->atoms, s, n, h);
  /* hash comme id stable (troncature possible côté appelant si besoin) */
  size_t id = (size_t)h;
  vt_mutex_unlock(&st->lock);
  return id;
}

/* ---------------------------------------------------------------------------
   Gestion des sources
--------------------------------------------------------------------------- */
static ssize_t find_source_idx(vt_state* st, const char* norm_path) {
  for (size_t i = 0; i < st->sources.len; i++) {
    if (strcmp(st->sources.data[i].path, norm_path) == 0) return (ssize_t)i;
  }
  return -1;
}

int vt_state_add_source(vt_state* st, const char* path,
                        const char* contents_opt_utf8) {
  if (!path || !*path) return -1;

  char* tmp = xstrdup(path);
  path_normalize(tmp);
  const char* ipath = vt_intern_cstr(st, tmp);
  free(tmp);

  vt_mutex_lock(&st->lock);
  if (find_source_idx(st, ipath) >= 0) {
    vt_mutex_unlock(&st->lock);
    return 1;
  }

  vt_source s = {0};
  s.path = ipath;
  if (contents_opt_utf8) {
    s.size = strlen(contents_opt_utf8);
    s.text = (char*)xmalloc(s.size + 1);
    memcpy(s.text, contents_opt_utf8, s.size + 1);
    s.loaded = 1;
  } else {
    size_t n = 0;
    s.text = read_all_utf8(ipath, &n);
    if (!s.text) {
      vt_mutex_unlock(&st->lock);
      VT_ERROR("vt_state_add_source: lecture impossible: %s", ipath);
      return -2;
    }
    s.size = n;
    s.loaded = 1;
  }
  vec_src_push(&st->sources, s);
  vt_mutex_unlock(&st->lock);
#ifdef VT_DEBUG_H
  VT_DEBUG("source ajoutée: %s (%zu bytes)", ipath, (size_t)s.size);
#endif
  return 0;
}

/* ---------------------------------------------------------------------------
   Parsing de toutes les sources
--------------------------------------------------------------------------- */
int vt_state_parse_all(vt_state* st) {
#ifndef VT_PARSER_H_SENTINEL
  VT_ERROR("parser.h absent: vt_state_parse_all indisponible");
  return -1;
#else
  vt_mutex_lock(&st->lock);
  st->n_parsed = 0;
  st->n_errors = 0;

  for (size_t i = 0; i < st->sources.len; i++) {
    vt_source* s = &st->sources.data[i];

    if (!s->loaded) {
      continue;
    }

    /* Si déjà parsé, libère ancien résultat */
    if (s->parse.arena) {
      vt_parse_free(&s->parse);
      memset(&s->parse, 0, sizeof(s->parse));
    }

    /* Parse mémoire pour cohérence avec cache */
    s->parse = vt_parse_source(s->text, s->path);
    st->n_parsed++;

    /* Comptage diagnostics (heuristique: niveau non exposé → on compte tout
     * comme err<=WARN?) */
    size_t errs = 0;
    for (size_t d = 0; d < s->parse.ndiags; d++) {
      /* Convention simple: messages contenant "error" → erreurs */
      const char* m = s->parse.diags[d].msg ? s->parse.diags[d].msg : "";
      if (strstr(m, "error") || strstr(m, "Error") || strstr(m, "ERROR"))
        errs++;
    }
    st->n_errors += errs;

#ifdef VT_DEBUG_H
    if (errs) {
      VT_WARN("parse: %s — %zu diagnostic(s), %zu erreur(s)", s->path,
              (size_t)s->parse.ndiags, errs);
    } else {
      VT_INFO("parse: %s — OK (%zu diag)", s->path, (size_t)s->parse.ndiags);
    }
#endif
  }

  vt_mutex_unlock(&st->lock);
  return st->n_errors ? -2 : 0;
#endif
}

/* ---------------------------------------------------------------------------
   Dump ASTs
--------------------------------------------------------------------------- */
void vt_state_dump_ast(FILE* out, vt_state* st) {
#ifndef VT_PARSER_H_SENTINEL
  (void)out;
  (void)st;
  VT_ERROR("parser.h absent: vt_state_dump_ast indisponible");
#else
  if (!out) out = stderr;
  vt_mutex_lock(&st->lock);
  for (size_t i = 0; i < st->sources.len; i++) {
    vt_source* s = &st->sources.data[i];
    fprintf(out, "=== AST: %s ===\n", s->path);
    vt_ast_dump(out, &s->parse);
    fprintf(out, "\n");
  }
  vt_mutex_unlock(&st->lock);
#endif
}

/* ---------------------------------------------------------------------------
   Extensions futures (stubs non fatales)
--------------------------------------------------------------------------- */
int vt_state_lower(vt_state* st) {
  (void)st; /* IR/SSA */
  return 0;
}
int vt_state_codegen(vt_state* st) {
  (void)st; /* bytecode/obj */
  return 0;
}

/* ---------------------------------------------------------------------------
   Fin
--------------------------------------------------------------------------- */
