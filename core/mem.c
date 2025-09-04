// vitte-light/core/mem.c
// Allocateurs utilitaires pour VitteLight: wrappers sûrs, arène (bump), pool
// fixe, buffer dynamique, alignement, stats et hooks OOM. Indépendant d'api.c.
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/mem.c
//
// Si vous voulez un header dédié, demandez `mem.h`. Les prototypes clés sont
// rappelés dans les sections.

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <malloc.h>
#endif

#ifndef VL_DEFAULT_ALIGN
#define VL_DEFAULT_ALIGN \
  ((size_t)(sizeof(void *) * 2u))  // 8 sur 32-bit, 16 sur 64-bit
#endif

// ───────────────────────── Alignement ─────────────────────────
static inline size_t vl_align_up(size_t x, size_t a) {
  size_t m = a - 1;
  return (x + m) & ~m;
}

// ───────────────────────── Hooks OOM / Stats ─────────────────────────

typedef void (*VL_OOM_Handler)(size_t bytes);
static VL_OOM_Handler g_oom = NULL;

static size_t g_bytes_alloc = 0,
              g_bytes_freed = 0;  // indicatifs, non thread-safe

static void vl__oom(size_t n) {
  if (g_oom) {
    g_oom(n);
  }
  fprintf(stderr, "[vitte-light/mem] OOM while requesting %zu bytes\n", n);
  abort();
}

void vl_mem_set_oom_handler(VL_OOM_Handler fn) { g_oom = fn; }
void vl_mem_stats(size_t *allocd, size_t *freed, size_t *live) {
  if (allocd) *allocd = g_bytes_alloc;
  if (freed) *freed = g_bytes_freed;
  if (live) *live = (g_bytes_alloc - g_bytes_freed);
}

// ───────────────────────── Wrappers génériques ─────────────────────────
// API:
//   void *vl_malloc_s(size_t n);   // renvoie NULL si OOM
//   void *vl_calloc_s(size_t c, size_t n);
//   void *vl_realloc_s(void *p, size_t n);
//   void  vl_free(void *p, size_t n_hint);
//   void *vl_malloc_x(size_t n);   // OOM -> abort via handler
//   ... idem pour calloc/realloc (_x)

void *vl_malloc_s(size_t n) {
  void *p = malloc(n ? n : 1);
  if (p) g_bytes_alloc += n;
  return p;
}
void *vl_calloc_s(size_t c, size_t n) {
  size_t t = c * n;
  if (c && t / c != n) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = calloc(c ? n ? c : 1 : 1, n ? n : 1);
  if (p) g_bytes_alloc += t;
  return p;
}
void *vl_realloc_s(void *p, size_t n) {
  size_t old = 0;  // old inconnu ici
  void *q = realloc(p, n ? n : 1);
  if (q) {
    if (p == NULL)
      g_bytes_alloc += n;
    else { /* heuristique: ne pas modifier stats */
    }
  }
  return q;
}
void vl_free(void *p, size_t n_hint) {
  if (!p) return;
  free(p);
  if (n_hint) g_bytes_freed += n_hint;
}

void *vl_malloc_x(size_t n) {
  void *p = vl_malloc_s(n);
  if (!p) vl__oom(n);
  return p;
}
void *vl_calloc_x(size_t c, size_t n) {
  void *p = vl_calloc_s(c, n);
  if (!p) vl__oom((size_t)c * n);
  return p;
}
void *vl_realloc_x(void *p, size_t n) {
  void *q = vl_realloc_s(p, n);
  if (!q) vl__oom(n);
  return q;
}

// ───────────────────────── Aligned allocation ─────────────────────────
// API:
//   void *vl_aligned_alloc(size_t align, size_t n);
//   void  vl_aligned_free(void *p);

void *vl_aligned_alloc(size_t align, size_t n) {
  if (align < sizeof(void *)) align = sizeof(void *);
#if defined(_WIN32)
  void *p = _aligned_malloc(n ? n : 1, align);
  if (!p) return NULL;
  g_bytes_alloc += n;
  return p;
#elif defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L)
  void *p = NULL;
  if (posix_memalign(&p, align, n ? n : 1) != 0) return NULL;
  g_bytes_alloc += n;
  return p;
#else
  // fallback: over-allocate et ranger le pointeur d'origine
  size_t total = n + align + sizeof(void *);
  uint8_t *raw = (uint8_t *)malloc(total);
  if (!raw) return NULL;
  g_bytes_alloc += total;
  uintptr_t base = (uintptr_t)(raw + sizeof(void *));
  uintptr_t aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
  void **slot = (void **)(aligned - sizeof(void *));
  *slot = raw;
  return (void *)aligned;
#endif
}

void vl_aligned_free(void *p) {
  if (!p) return;
#if defined(_WIN32)
  _aligned_free(p);
#else
#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L)
  free(p);
#else
  // récupérer le bloc d'origine
  void **slot = (void **)((uintptr_t)p - sizeof(void *));
  free(*slot);
#endif
#endif
}

// ───────────────────────── Arène (bump allocator) ─────────────────────────
// API:
//   typedef struct VL_Arena VL_Arena;
//   void   vl_arena_init(VL_Arena *A, size_t chunk_size, size_t align);
//   void   vl_arena_release(VL_Arena *A);
//   void  *vl_arena_alloc(VL_Arena *A, size_t n);
//   void  *vl_arena_alloc_aligned(VL_Arena *A, size_t n, size_t align);
//   char  *vl_arena_strdup(VL_Arena *A, const char *s);
//   char  *vl_arena_strndup(VL_Arena *A, const char *s, size_t n);
//   typedef struct { void *chunk; size_t used; } VL_ArenaMark;
//   VL_ArenaMark vl_arena_mark(const VL_Arena *A);
//   void   vl_arena_reset_to(VL_Arena *A, VL_ArenaMark m);

typedef struct VL_ArenaChunk {
  struct VL_ArenaChunk *next;
  size_t cap, used;
  uint8_t data[];
} VL_ArenaChunk;

typedef struct VL_Arena {
  VL_ArenaChunk *head;
  size_t chunk_size;
  size_t align;
  size_t total_bytes;  // stats
  size_t peak_used;
  size_t n_chunks;
} VL_Arena;

static VL_ArenaChunk *vl__arena_new_chunk(size_t need, size_t chunk_size) {
  size_t csz = (need > chunk_size ? need : chunk_size);
  size_t bytes = sizeof(VL_ArenaChunk) + csz;
  VL_ArenaChunk *ck = (VL_ArenaChunk *)vl_malloc_s(bytes);
  if (!ck) return NULL;
  ck->next = NULL;
  ck->cap = csz;
  ck->used = 0;
  return ck;
}

void vl_arena_init(VL_Arena *A, size_t chunk_size, size_t align) {
  memset(A, 0, sizeof(*A));
  A->chunk_size = (chunk_size ? chunk_size : (size_t)32 * 1024);
  A->align = align ? align : VL_DEFAULT_ALIGN;
}

void vl_arena_release(VL_Arena *A) {
  VL_ArenaChunk *c = A->head;
  while (c) {
    VL_ArenaChunk *n = c->next;
    vl_free(c, sizeof(VL_ArenaChunk) + c->cap);
    c = n;
  }
  memset(A, 0, sizeof(*A));
}

static inline void *vl__arena_alloc_from(VL_Arena *A, VL_ArenaChunk *ck,
                                         size_t n, size_t align) {
  size_t off = vl_align_up(ck->used, align);
  if (off + n <= ck->cap) {
    void *p = ck->data + off;
    ck->used = off + n;
    if (ck->used > A->peak_used) A->peak_used = ck->used;
    return p;
  }
  return NULL;
}

void *vl_arena_alloc_aligned(VL_Arena *A, size_t n, size_t align) {
  if (!A) return NULL;
  if (align == 0) align = A->align;
  if (!A->head) {
    A->head = vl__arena_new_chunk(n + align, A->chunk_size);
    if (!A->head) return NULL;
    A->n_chunks = 1;
    A->total_bytes += sizeof(VL_ArenaChunk) + A->head->cap;
  }
  void *p = vl__arena_alloc_from(A, A->head, n, align);
  if (p) return p;
  // nouveau chunk en tête
  VL_ArenaChunk *ck = vl__arena_new_chunk(n + align, A->chunk_size);
  if (!ck) return NULL;
  ck->next = A->head;
  A->head = ck;
  A->n_chunks++;
  A->total_bytes += sizeof(VL_ArenaChunk) + ck->cap;
  return vl__arena_alloc_from(A, ck, n, align);
}

void *vl_arena_alloc(VL_Arena *A, size_t n) {
  return vl_arena_alloc_aligned(A, n, A ? A->align : VL_DEFAULT_ALIGN);
}

char *vl_arena_strndup(VL_Arena *A, const char *s, size_t n) {
  if (!s) return NULL;
  char *p = (char *)vl_arena_alloc_aligned(A, n + 1, 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}
char *vl_arena_strdup(VL_Arena *A, const char *s) {
  return s ? vl_arena_strndup(A, s, strlen(s)) : NULL;
}

typedef struct {
  VL_ArenaChunk *chunk;
  size_t used;
} VL_ArenaMark;
VL_ArenaMark vl_arena_mark(const VL_Arena *A) {
  VL_ArenaMark m;
  m.chunk = A ? A->head : NULL;
  m.used = (A && A->head) ? A->head->used : 0;
  return m;
}
void vl_arena_reset_to(VL_Arena *A, VL_ArenaMark m) {
  if (!A) return;
  while (A->head && A->head != m.chunk) {
    VL_ArenaChunk *n = A->head->next;
    vl_free(A->head, sizeof(VL_ArenaChunk) + A->head->cap);
    A->head = n;
    A->n_chunks--;
  }
  if (A->head) A->head->used = m.used;
}

// ───────────────────────── Pool fixe (slab) ─────────────────────────
// API:
//   typedef struct VL_Pool VL_Pool;
//   void   vl_pool_init(VL_Pool *P, size_t item_size, size_t items_per_slab);
//   void  *vl_pool_alloc(VL_Pool *P);
//   void   vl_pool_free(VL_Pool *P, void *ptr);
//   void   vl_pool_release(VL_Pool *P);

typedef struct VL_PoolSlab {
  struct VL_PoolSlab *next;
  uint8_t data[];
} VL_PoolSlab;

typedef struct VL_Pool {
  VL_PoolSlab *slabs;
  void *free_list;        // single-linked via *next stocké en tête de bloc
  size_t item_size;       // taille arrondie à l'alignement
  size_t items_per_slab;  // nombre d'objets par slab
  size_t slab_bytes;      // taille réelle de chaque slab
  size_t n_items;         // stats
} VL_Pool;

void vl_pool_init(VL_Pool *P, size_t item_size, size_t items_per_slab) {
  memset(P, 0, sizeof(*P));
  size_t isz = vl_align_up(item_size ? item_size : 1, VL_DEFAULT_ALIGN);
  P->item_size = isz;
  P->items_per_slab = items_per_slab ? items_per_slab : 256;
  P->slab_bytes = sizeof(VL_PoolSlab) + isz * P->items_per_slab;
}

static void vl__pool_push_free(VL_Pool *P, void *blk) {
  *(void **)blk = P->free_list;
  P->free_list = blk;
}
static void *vl__pool_pop_free(VL_Pool *P) {
  void *blk = P->free_list;
  if (blk) P->free_list = *(void **)blk;
  return blk;
}

static int vl__pool_grow(VL_Pool *P) {
  VL_PoolSlab *sl = (VL_PoolSlab *)vl_malloc_s(P->slab_bytes);
  if (!sl) return 0;
  sl->next = P->slabs;
  P->slabs = sl;
  uint8_t *p = sl->data;
  for (size_t i = 0; i < P->items_per_slab; i++) {
    vl__pool_push_free(P, p + i * P->item_size);
  }
  return 1;
}

void *vl_pool_alloc(VL_Pool *P) {
  void *b = vl__pool_pop_free(P);
  if (!b) {
    if (!vl__pool_grow(P)) return NULL;
    b = vl__pool_pop_free(P);
  }
  P->n_items++;
  return b;
}
void vl_pool_free(VL_Pool *P, void *ptr) {
  if (!ptr) return;
  P->n_items--;
  vl__pool_push_free(P, ptr);
}
void vl_pool_release(VL_Pool *P) {
  VL_PoolSlab *s = P->slabs;
  while (s) {
    VL_PoolSlab *n = s->next;
    vl_free(s, P->slab_bytes);
    s = n;
  }
  memset(P, 0, sizeof(*P));
}

// ───────────────────────── Buffer dynamique (byte builder)
// ───────────────────────── API:
//   typedef struct VL_Buffer { uint8_t *d; size_t n, cap; } VL_Buffer;
//   void   vl_buf_init(VL_Buffer *b);
//   void   vl_buf_free(VL_Buffer *b);
//   int    vl_buf_reserve(VL_Buffer *b, size_t need);
//   int    vl_buf_append(VL_Buffer *b, const void *src, size_t n);
//   int    vl_buf_putc(VL_Buffer *b, int c);
//   int    vl_buf_printf(VL_Buffer *b, const char *fmt, ...);
//   char  *vl_buf_take_cstr(VL_Buffer *b); // prend possession, NUL-terminé

typedef struct VL_Buffer {
  uint8_t *d;
  size_t n, cap;
} VL_Buffer;

void vl_buf_init(VL_Buffer *b) {
  b->d = NULL;
  b->n = b->cap = 0;
}
void vl_buf_free(VL_Buffer *b) {
  if (!b) return;
  if (b->d) {
    vl_free(b->d, b->cap);
  }
  b->d = NULL;
  b->n = b->cap = 0;
}

int vl_buf_reserve(VL_Buffer *b, size_t need) {
  if (need <= b->cap) return 1;
  size_t cap = b->cap ? b->cap : 64;
  while (cap < need) cap = cap + cap / 2 + 8;
  uint8_t *p = (uint8_t *)vl_realloc_s(b->d, cap);
  if (!p) return 0;
  b->d = p;
  b->cap = cap;
  return 1;
}
int vl_buf_append(VL_Buffer *b, const void *src, size_t n) {
  if (!vl_buf_reserve(b, b->n + n + 1)) return 0;
  memcpy(b->d + b->n, src, n);
  b->n += n;
  b->d[b->n] = '\0';
  return 1;
}
int vl_buf_putc(VL_Buffer *b, int c) {
  uint8_t ch = (uint8_t)c;
  return vl_buf_append(b, &ch, 1);
}

int vl_buf_vprintf(VL_Buffer *b, const char *fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  char tmp[256];
  int k = vsnprintf(tmp, sizeof(tmp), fmt, cp);
  va_end(cp);
  if (k < 0) return 0;
  if ((size_t)k < sizeof(tmp)) return vl_buf_append(b, tmp, (size_t)k);
  size_t need = (size_t)k + 1;
  if (!vl_buf_reserve(b, b->n + need)) return 0;
  vsnprintf((char *)b->d + b->n, need, fmt, ap);
  b->n += (size_t)k;
  return 1;
}
int vl_buf_printf(VL_Buffer *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ok = vl_buf_vprintf(b, fmt, ap);
  va_end(ap);
  return ok;
}

char *vl_buf_take_cstr(VL_Buffer *b) {
  if (!vl_buf_reserve(b, b->n + 1)) return NULL;
  b->d[b->n] = '\0';
  char *s = (char *)b->d;
  b->d = NULL;
  b->cap = 0;
  b->n = 0;
  return s;
}

// ───────────────────────── Utilitaires fichiers ─────────────────────────
int vl_write_file(const char *path, const void *data, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  size_t wr = fwrite(data, 1, n, f);
  fclose(f);
  return wr == n;
}

// ───────────────────────── Tests unitaires simples ─────────────────────────
#ifdef VL_MEM_TEST_MAIN
static void test_arena(void) {
  VL_Arena A;
  vl_arena_init(&A, 1024, 8);
  char *s = vl_arena_strdup(&A, "hello");
  (void)s;
  for (int i = 0; i < 1000; i++) {
    (void)vl_arena_alloc(&A, 37);
  }
  VL_ArenaMark m = vl_arena_mark(&A);
  (void)vl_arena_alloc(&A, 4096);
  vl_arena_reset_to(&A, m);
  vl_arena_release(&A);
}
static void test_pool(void) {
  typedef struct {
    int x, y;
  } Node;
  VL_Pool P;
  vl_pool_init(&P, sizeof(Node), 64);
  Node *v[1000];
  for (int i = 0; i < 1000; i++) {
    v[i] = (Node *)vl_pool_alloc(&P);
    v[i]->x = i;
    v[i]->y = i * i;
  }
  for (int i = 0; i < 1000; i += 2) {
    vl_pool_free(&P, v[i]);
  }
  for (int i = 0; i < 500; i++) {
    (void)vl_pool_alloc(&P);
  }
  vl_pool_release(&P);
}
static void test_buf(void) {
  VL_Buffer b;
  vl_buf_init(&b);
  vl_buf_printf(&b, "num=%d", 42);
  vl_buf_putc(&b, '\n');
  vl_buf_append(&b, "ok", 2);
  char *s = vl_buf_take_cstr(&b);
  fputs(s, stdout);
  free(s);
}
int main(void) {
  test_arena();
  test_pool();
  test_buf();
  size_t a, f, l;
  vl_mem_stats(&a, &f, &l);
  fprintf(stderr, "alloc=%zu freed=%zu live=%zu\n", a, f, l);
  return 0;
}
#endif
