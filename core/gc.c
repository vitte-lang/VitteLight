/* ============================================================================
   gc.c — Collecteur mark-sweep précis avec racines explicites (C17).
   Caractéristiques:
     - API simple: create/destroy, alloc(trace,finalizer), collect, stats.
     - Tracing précis via callback utilisateur (pas de scan conservatif).
     - Racines explicites: enregistrer des pointeurs (void**) modifiables.
     - Finalizers optionnels, pin/unpin, limite d’heap, stats.
     - Thread-safe si <threads.h> dispo, sinon mono-thread.
   Auteur: MIT.
   ============================================================================
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#include <threads.h>
#define VTGC_HAS_THREADS 1
#else
#define VTGC_HAS_THREADS 0
#endif

/* -------------------------------- API publique (protos) ------------------- */
typedef struct vt_gc vt_gc;

typedef void (*vt_gc_visit_fn)(void* child, void* ctx);
typedef void (*vt_gc_trace_fn)(void* obj, vt_gc_visit_fn visit, void* ctx);
typedef void (*vt_gc_finalizer)(void* obj);

typedef struct {
  size_t heap_limit_bytes; /* 0 = auto (8 Mo) */
  int enable_logging;      /* 1 = logs sur stderr */
} vt_gc_config;

vt_gc* vt_gc_create(const vt_gc_config* cfg);
void vt_gc_destroy(vt_gc* gc);

void* vt_gc_alloc(vt_gc* gc, size_t size, vt_gc_trace_fn trace,
                  vt_gc_finalizer fin, uint32_t tag);

void vt_gc_collect(vt_gc* gc, const char* reason);

void vt_gc_add_root(vt_gc* gc, void** slot);
void vt_gc_remove_root(vt_gc* gc, void** slot);

void vt_gc_pin(void* obj);
void vt_gc_unpin(void* obj);

void vt_gc_set_limit(vt_gc* gc, size_t bytes);

size_t vt_gc_bytes_live(vt_gc* gc);
size_t vt_gc_object_count(vt_gc* gc);
uint32_t vt_gc_tag_of(const void* obj);
void vt_gc_set_tag(void* obj, uint32_t tag);

void vt_gc_dump(vt_gc* gc, FILE* out); /* debug */

/* -------------------------------- Implémentation -------------------------- */
#ifndef alignof
#define alignof _Alignof
#endif

#include <stddef.h>

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define VT_MAX_ALIGN max_align_t
#else
typedef struct {
  long double __vt_align;
} vt_max_align;
#define VT_MAX_ALIGN vt_max_align
#endif

typedef struct vt_gc_obj vt_gc_obj;

typedef struct {
  void*** v; /* tableau de slots racines (adresses de pointeurs) */
  size_t n, cap;
} vt_gc_roots;

struct vt_gc_obj {
  vt_gc_obj* next_all;
  vt_gc_obj* next_gray;
  size_t size;         /* taille du payload */
  uint32_t tag;        /* libre pour l’utilisateur */
  uint32_t pin;        /* >0 => non libérable */
  uint32_t mark_epoch; /* dernière époque marquée */
  vt_gc_trace_fn trace;
  vt_gc_finalizer fin;
  VT_MAX_ALIGN _align_guard; /* force l’alignement du payload */
                            /* payload suit immédiatement */
};

struct vt_gc {
  vt_gc_obj* all;    /* liste simplement chaînée de tous les objets */
  vt_gc_obj* gray;   /* pile gris pour le marquage */
  vt_gc_roots roots; /* racines explicites */
  size_t bytes_live;
  size_t obj_count;
  size_t heap_limit;
  size_t bytes_since_gc;
  uint32_t epoch;
  int logging;
#if VTGC_HAS_THREADS
  mtx_t lock;
#endif
};

/* --------- Utils lock --------- */
static inline void vtgc_lock(vt_gc* gc) {
#if VTGC_HAS_THREADS
  mtx_lock(&gc->lock);
#else
  (void)gc;
#endif
}
static inline void vtgc_unlock(vt_gc* gc) {
#if VTGC_HAS_THREADS
  mtx_unlock(&gc->lock);
#else
  (void)gc;
#endif
}

/* --------- Journal minimal --------- */
static void vtgc_log(vt_gc* gc, const char* level, const char* fmt, ...) {
  if (!gc || !gc->logging) return;
  char ts[32];
  {
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);
  }
  fprintf(stderr, "[%s] GC %-5s | ", ts, level);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

/* --------- Helpers racines --------- */
static int vtgc_roots_find(vt_gc_roots* R, void** slot, size_t* idx_out) {
  for (size_t i = 0; i < R->n; ++i) {
    if (R->v[i] == slot) {
      if (idx_out) *idx_out = i;
      return 1;
    }
  }
  return 0;
}
static int vtgc_roots_push(vt_gc_roots* R, void** slot) {
  if (R->n == R->cap) {
    size_t ncap = R->cap ? R->cap * 2 : 32;
    void*** nv = (void***)realloc(R->v, ncap * sizeof(void**));
    if (!nv) return 0;
    R->v = nv;
    R->cap = ncap;
  }
  R->v[R->n++] = slot;
  return 1;
}
static void vtgc_roots_remove_at(vt_gc_roots* R, size_t i) {
  if (i < R->n - 1) R->v[i] = R->v[R->n - 1];
  R->n--;
}

/* --------- Header <-> payload --------- */
static inline vt_gc_obj* vtgc_hdr_from_ptr(const void* p) {
  if (!p) return NULL;
  return (vt_gc_obj*)((uint8_t*)p - sizeof(vt_gc_obj));
}
static inline void* vtgc_ptr_from_hdr(vt_gc_obj* h) {
  return (void*)((uint8_t*)h + sizeof(vt_gc_obj));
}

/* --------- Marquage --------- */
static inline void vtgc_mark_hdr(vt_gc* gc, vt_gc_obj* h) {
  if (!h) return;
  if (h->mark_epoch == gc->epoch) return;
  h->mark_epoch = gc->epoch;
  h->next_gray = gc->gray;
  gc->gray = h;
}
static void vtgc_visit_child(void* child, void* ctx) {
  if (!child) return;
  vt_gc* gc = (vt_gc*)ctx;
  vtgc_mark_hdr(gc, vtgc_hdr_from_ptr(child));
}

/* --------- Sweep --------- */
static void vtgc_sweep(vt_gc* gc) {
  vt_gc_obj* prev = NULL;
  vt_gc_obj* cur = gc->all;
  size_t new_live = 0, new_count = 0;
  while (cur) {
    vt_gc_obj* next = cur->next_all;
    const int alive = (cur->mark_epoch == gc->epoch) || (cur->pin > 0);
    if (alive) {
      new_live += cur->size;
      new_count += 1;
      prev = cur;
    } else {
      if (cur->fin) {
        /* Le finalizer ne doit pas ressusciter l’objet. */
        cur->fin(vtgc_ptr_from_hdr(cur));
      }
      if (prev)
        prev->next_all = next;
      else
        gc->all = next;
      free(cur);
    }
    cur = next;
  }
  gc->bytes_live = new_live;
  gc->obj_count = new_count;
}

/* --------- Collect --------- */
static void vtgc_mark_from_roots(vt_gc* gc) {
  /* 1) Marquer racines */
  for (size_t i = 0; i < gc->roots.n; ++i) {
    void** slot = gc->roots.v[i];
    if (!slot) continue;
    vtgc_mark_hdr(gc, vtgc_hdr_from_ptr(*slot));
  }
  /* 2) Propager */
  while (gc->gray) {
    vt_gc_obj* h = gc->gray;
    gc->gray = h->next_gray;
    h->next_gray = NULL;
    if (h->trace) {
      h->trace(vtgc_ptr_from_hdr(h), vtgc_visit_child, gc);
    }
  }
}

void vt_gc_collect(vt_gc* gc, const char* reason) {
  if (!gc) return;
  vtgc_lock(gc);
  gc->epoch++;
  vtgc_log(gc, "INFO",
           "collect start (epoch=%u, reason=%s, objs=%zu, live=%zu)", gc->epoch,
           reason ? reason : "manual", gc->obj_count, gc->bytes_live);
  vtgc_mark_from_roots(gc);
  vtgc_sweep(gc);
  gc->bytes_since_gc = 0;
  vtgc_log(gc, "INFO", "collect end   (objs=%zu, live=%zu)", gc->obj_count,
           gc->bytes_live);
  vtgc_unlock(gc);
}

/* --------- Politique de GC --------- */
static void vtgc_maybe_collect(vt_gc* gc, size_t just_alloc) {
  gc->bytes_since_gc += just_alloc;
  const size_t limit =
      gc->heap_limit ? gc->heap_limit : (8u << 20); /* 8 MiB par défaut */
  if (gc->bytes_live > limit || gc->bytes_since_gc > limit / 2) {
    vt_gc_collect(
        gc, gc->bytes_live > limit ? "heap_limit" : "allocation_pressure");
  }
}

/* --------- API publique --------- */
vt_gc* vt_gc_create(const vt_gc_config* cfg) {
  vt_gc* gc = (vt_gc*)calloc(1, sizeof *gc);
  if (!gc) return NULL;
  gc->heap_limit =
      (cfg && cfg->heap_limit_bytes) ? cfg->heap_limit_bytes : (8u << 20);
  gc->logging = (cfg && cfg->enable_logging) ? 1 : 0;
  gc->epoch = 1; /* éviter zéro */
#if VTGC_HAS_THREADS
  if (mtx_init(&gc->lock, mtx_plain) != thrd_success) {
    free(gc);
    return NULL;
  }
#endif
  vtgc_log(gc, "INFO", "gc created (limit=%zu)", gc->heap_limit);
  return gc;
}

void vt_gc_destroy(vt_gc* gc) {
  if (!gc) return;
  vtgc_lock(gc);
  /* libère tout sans marquage */
  vt_gc_obj* cur = gc->all;
  while (cur) {
    vt_gc_obj* next = cur->next_all;
    if (cur->fin) cur->fin(vtgc_ptr_from_hdr(cur));
    free(cur);
    cur = next;
  }
  free(gc->roots.v);
  vtgc_unlock(gc);
#if VTGC_HAS_THREADS
  mtx_destroy(&gc->lock);
#endif
  vtgc_log(gc, "INFO", "gc destroyed");
  free(gc);
}

void* vt_gc_alloc(vt_gc* gc, size_t size, vt_gc_trace_fn trace,
                  vt_gc_finalizer fin, uint32_t tag) {
  if (!gc || size == 0) return NULL;
  /* on s’assure d’un header aligné pour payload */
  size_t total = sizeof(vt_gc_obj) + size;
  vt_gc_obj* h = (vt_gc_obj*)malloc(total);
  if (!h) return NULL;

  h->next_all = gc->all;
  gc->all = h;
  h->next_gray = NULL;
  h->size = size;
  h->tag = tag;
  h->pin = 0;
  h->mark_epoch = 0; /* pas marqué */
  h->trace = trace;
  h->fin = fin;

  void* p = vtgc_ptr_from_hdr(h);

  vtgc_lock(gc);
  gc->bytes_live += size;
  gc->obj_count += 1;
  vtgc_unlock(gc);

  vtgc_maybe_collect(gc, size);
  return p;
}

void vt_gc_add_root(vt_gc* gc, void** slot) {
  if (!gc || !slot) return;
  vtgc_lock(gc);
  size_t idx;
  if (!vtgc_roots_find(&gc->roots, slot, &idx)) {
    (void)vtgc_roots_push(&gc->roots, slot);
    vtgc_log(gc, "TRACE", "root + %p", (void*)slot);
  }
  vtgc_unlock(gc);
}
void vt_gc_remove_root(vt_gc* gc, void** slot) {
  if (!gc || !slot) return;
  vtgc_lock(gc);
  size_t idx;
  if (vtgc_roots_find(&gc->roots, slot, &idx)) {
    vtgc_roots_remove_at(&gc->roots, idx);
    vtgc_log(gc, "TRACE", "root - %p", (void*)slot);
  }
  vtgc_unlock(gc);
}

void vt_gc_pin(void* obj) {
  vt_gc_obj* h = vtgc_hdr_from_ptr(obj);
  if (!h) return;
  h->pin++;
}
void vt_gc_unpin(void* obj) {
  vt_gc_obj* h = vtgc_hdr_from_ptr(obj);
  if (!h) return;
  if (h->pin > 0) h->pin--;
}

void vt_gc_set_limit(vt_gc* gc, size_t bytes) {
  if (!gc) return;
  vtgc_lock(gc);
  gc->heap_limit = bytes;
  vtgc_unlock(gc);
}

size_t vt_gc_bytes_live(vt_gc* gc) { return gc ? gc->bytes_live : 0; }
size_t vt_gc_object_count(vt_gc* gc) { return gc ? gc->obj_count : 0; }

uint32_t vt_gc_tag_of(const void* obj) {
  const vt_gc_obj* h = vtgc_hdr_from_ptr(obj);
  return h ? h->tag : 0;
}
void vt_gc_set_tag(void* obj, uint32_t tag) {
  vt_gc_obj* h = vtgc_hdr_from_ptr(obj);
  if (h) h->tag = tag;
}

/* --------- Dump debug --------- */
void vt_gc_dump(vt_gc* gc, FILE* out) {
  if (!gc) return;
  if (!out) out = stderr;
  fprintf(out, "GC dump: objs=%zu live=%zuB epoch=%u roots=%zu limit=%zuB\n",
          gc->obj_count, gc->bytes_live, gc->epoch, gc->roots.n,
          gc->heap_limit);
  size_t i = 0;
  for (vt_gc_obj* h = gc->all; h; h = h->next_all, ++i) {
    fprintf(out, "  #%zu obj=%p size=%zu tag=%u pin=%u mark=%u\n", i,
            (void*)vtgc_ptr_from_hdr(h), h->size, h->tag, h->pin,
            h->mark_epoch);
  }
}

/* --------- Exemple d’adaptateur de trace (optionnel, non exporté) ---------
   Exposez ce genre de helper dans votre code applicatif:

   typedef struct Node { struct Node* left; struct Node* right; int v; } Node;
   static void node_trace(void* obj, vt_gc_visit_fn visit, void* ctx) {
       Node* n = (Node*)obj;
       visit(n->left, ctx);
       visit(n->right, ctx);
   }
   Node* n = (Node*)vt_gc_alloc(gc, sizeof(Node), node_trace, NULL, 0);
   ------------------------------------------------------------------------- */
