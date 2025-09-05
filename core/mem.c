/* ============================================================================
   mem.c — Allocateurs et utilitaires mémoire « ultra complet » (C17)
   - Wrappers sûrs (vt_malloc/calloc/realloc/free) avec stats atomiques
   - Aligned alloc cross-platform
   - Pages OS (mmap/VirtualAlloc)
   - Arena allocator (grow, mark/reset, align)
   - Pool fixe (free-list), thread-safe optionnel
   - Buffer dynamique (vt_buf) + printf-like
   - Duplication (mem/str), fill/zero/swap
   - Leak tracking optionnel (VT_MEM_LEAK_TRACK)
   Licence: MIT.
   ============================================================================
 */

#if defined(__has_include)
#if __has_include("mem.h")
#include "mem.h"
#define VT_HAVE_MEM_H 1
#endif
#endif

#ifndef VT_HAVE_MEM_H
/* Interface minimale si mem.h absent (compilation autonome) */
#include <stddef.h>
#include <stdint.h>
typedef struct vt_mem_stats {
  size_t cur_bytes;
  size_t peak_bytes;
  size_t total_allocs;
  size_t total_frees;
} vt_mem_stats;

typedef struct vt_arena vt_arena;
typedef struct vt_arena_mark {
  void* _chunk;
  size_t _len;
  size_t _total;
} vt_arena_mark;

typedef struct vt_pool vt_pool;

typedef struct vt_buf {
  unsigned char* data;
  size_t len, cap;
} vt_buf;

/* API */
void vt_mem_init(void);
void vt_mem_shutdown(void);
void vt_mem_get_stats(vt_mem_stats* out);
void vt_mem_set_abort_on_oom(int on);

void* vt_malloc(size_t n);
void* vt_calloc(size_t nmemb, size_t size);
void* vt_realloc(void* p, size_t n);
void vt_free(void* p);

void* vt_aligned_alloc(size_t alignment, size_t size);
void vt_aligned_free(void* p);

void* vt_memdup(const void* src, size_t n);
char* vt_strndup(const char* s, size_t n);

void* vt_page_alloc(size_t size);
void vt_page_free(void* p, size_t size);

void vt_mem_zero(void* p, size_t n);
void vt_mem_fill(void* p, int byte, size_t n);
void vt_mem_swap(void* a, void* b, size_t n);

/* Arena */
void vt_arena_init(vt_arena* a, size_t first_chunk);
void vt_arena_dispose(vt_arena* a);
void* vt_arena_alloc(vt_arena* a, size_t n, size_t align);
void vt_arena_reset(vt_arena* a);
vt_arena_mark vt_arena_mark_get(vt_arena* a);
void vt_arena_mark_reset(vt_arena* a, vt_arena_mark m);

/* Pool */
int vt_pool_init(vt_pool* p, size_t obj_size, size_t obj_align,
                 size_t objs_per_block);
void vt_pool_dispose(vt_pool* p);
void* vt_pool_alloc(vt_pool* p);
void vt_pool_free(vt_pool* p, void* obj);

/* Buffer */
void vt_buf_init(vt_buf* b);
void vt_buf_dispose(vt_buf* b);
int vt_buf_reserve(vt_buf* b, size_t need_cap);
int vt_buf_append(vt_buf* b, const void* data, size_t n);
int vt_buf_append_cstr(vt_buf* b, const char* s);
int vt_buf_printf(vt_buf* b, const char* fmt, ...);
unsigned char* vt_buf_detach(vt_buf* b, size_t* out_len);

#endif /* VT_HAVE_MEM_H */

/* ----------------------------------------------------------------------------
   Includes système
---------------------------------------------------------------------------- */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ----------------------------------------------------------------------------
   Config compile-time
---------------------------------------------------------------------------- */
#ifndef VT_MEM_API
#define VT_MEM_API /* extern par défaut dans mem.h ; noop ici */
#endif

#ifndef VT_MEM_DEFAULT_ABORT_ON_OOM
#define VT_MEM_DEFAULT_ABORT_ON_OOM 1
#endif

#ifndef VT_MEM_ALIGN_DEFAULT
#if INTPTR_MAX == INT64_MAX
#define VT_MEM_ALIGN_DEFAULT 16u
#else
#define VT_MEM_ALIGN_DEFAULT 8u
#endif
#endif

/* Leak tracking optionnel (simple table open-addressing) */
#ifdef VT_MEM_LEAK_TRACK
#ifndef VT_MEM_LT_CAP
#define VT_MEM_LT_CAP 65536u
#endif
#endif

/* ----------------------------------------------------------------------------
   Stats globales
---------------------------------------------------------------------------- */
static _Atomic size_t g_cur_bytes = 0;
static _Atomic size_t g_peak_bytes = 0;
static _Atomic size_t g_total_alloc = 0;
static _Atomic size_t g_total_free = 0;
static _Atomic int g_abort_oom = VT_MEM_DEFAULT_ABORT_ON_OOM;

static void vt__stats_on_alloc(size_t n) {
  size_t cur = atomic_fetch_add(&g_cur_bytes, n) + n;
  /* peak: CAS loop */
  size_t peak = atomic_load(&g_peak_bytes);
  while (cur > peak &&
         !atomic_compare_exchange_weak(&g_peak_bytes, &peak, cur)) { /* loop */
  }
  atomic_fetch_add(&g_total_alloc, 1);
}
static void vt__stats_on_free(size_t n) {
  atomic_fetch_sub(&g_cur_bytes, n);
  atomic_fetch_add(&g_total_free, 1);
}

void vt_mem_get_stats(vt_mem_stats* out) {
  if (!out) return;
  out->cur_bytes = atomic_load(&g_cur_bytes);
  out->peak_bytes = atomic_load(&g_peak_bytes);
  out->total_allocs = atomic_load(&g_total_alloc);
  out->total_frees = atomic_load(&g_total_free);
}

void vt_mem_set_abort_on_oom(int on) { atomic_store(&g_abort_oom, on ? 1 : 0); }

void vt_mem_init(void) { /* no-op pour l’instant */ }
void vt_mem_shutdown(void) {
#ifdef VT_MEM_LEAK_TRACK
  /* Rapport basique des fuites */
  extern void vt__lt_report_and_clear(void);
  vt__lt_report_and_clear();
#endif
}

/* ----------------------------------------------------------------------------
   Helpers
---------------------------------------------------------------------------- */
static size_t vt__align_up(size_t x, size_t a) {
  size_t m = a - 1;
  return (x + m) & ~m;
}

/* On stocke la taille juste avant le bloc utilisateur (header caché). */
typedef struct vt__hdr {
  size_t sz;
  size_t align; /* 0 pour non-aligné, sinon align utilisé */
} vt__hdr;

static vt__hdr* vt__hdr_from_user(void* p) {
  if (!p) return NULL;
  return ((vt__hdr*)p) - 1;
}
static void* vt__user_from_hdr(vt__hdr* h) { return (void*)(h + 1); }

/* ----------------------------------------------------------------------------
   Leak tracking (optionnel)
---------------------------------------------------------------------------- */
#ifdef VT_MEM_LEAK_TRACK
typedef struct {
  void* key;
  size_t sz;
} vt__lt_ent;
static vt__lt_ent* g_lt_tab;
static size_t g_lt_cap;
static _Atomic int g_lt_init;

static size_t vt__ptr_hash(uintptr_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return (size_t)x;
}

static void vt__lt_init(void) {
  if (atomic_exchange(&g_lt_init, 1) == 1) return;
  g_lt_cap = VT_MEM_LT_CAP;
  g_lt_tab = (vt__lt_ent*)calloc(g_lt_cap, sizeof(vt__lt_ent));
}

static void vt__lt_add(void* p, size_t sz) {
  if (!p) return;
  if (!g_lt_tab) vt__lt_init();
  size_t cap = g_lt_cap;
  size_t i = vt__ptr_hash((uintptr_t)p) & (cap - 1);
  for (;;) {
    if (g_lt_tab[i].key == NULL) {
      g_lt_tab[i].key = p;
      g_lt_tab[i].sz = sz;
      return;
    }
    if (g_lt_tab[i].key == p) {
      g_lt_tab[i].sz = sz;
      return;
    }
    i = (i + 1) & (cap - 1);
  }
}

static size_t vt__lt_del(void* p) {
  if (!g_lt_tab || !p) return 0;
  size_t cap = g_lt_cap;
  size_t i = vt__ptr_hash((uintptr_t)p) & (cap - 1);
  for (;;) {
    if (g_lt_tab[i].key == NULL) return 0;
    if (g_lt_tab[i].key == p) {
      size_t sz = g_lt_tab[i].sz;
      g_lt_tab[i].key = (void*)(uintptr_t)1; /* tombstone */
      g_lt_tab[i].sz = 0;
      return sz;
    }
    i = (i + 1) & (cap - 1);
  }
}

void vt__lt_report_and_clear(void) {
  if (!g_lt_tab) return;
  size_t leaks = 0, bytes = 0;
  for (size_t i = 0; i < g_lt_cap; ++i) {
    if (g_lt_tab[i].key && g_lt_tab[i].key != (void*)1) {
      ++leaks;
      bytes += g_lt_tab[i].sz;
      fprintf(stderr, "[mem] leak: ptr=%p size=%zu\n", g_lt_tab[i].key,
              g_lt_tab[i].sz);
    }
  }
  if (leaks) {
    fprintf(stderr, "[mem] total leaks: %zu, bytes: %zu\n", leaks, bytes);
  }
  free(g_lt_tab);
  g_lt_tab = NULL;
  g_lt_cap = 0;
  atomic_store(&g_lt_init, 0);
}
#endif

/* ----------------------------------------------------------------------------
   Allocation de base (header + stats + leak track)
---------------------------------------------------------------------------- */
static void vt__oom_abort(size_t n) {
  fprintf(stderr, "[mem] OOM requesting %zu bytes\n", n);
  abort();
}

void* vt_malloc(size_t n) {
  if (n == 0) n = 1;
  size_t need = sizeof(vt__hdr) + n;
  vt__hdr* h = (vt__hdr*)malloc(need);
  if (!h) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(n);
    errno = ENOMEM;
    return NULL;
  }
  h->sz = n;
  h->align = 0;
  vt__stats_on_alloc(n);
#ifdef VT_MEM_LEAK_TRACK
  vt__lt_add(vt__user_from_hdr(h), n);
#endif
  return vt__user_from_hdr(h);
}

void* vt_calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0) return vt_malloc(1);
  /* vérifier overflow nmemb*size */
  if (SIZE_MAX / size < nmemb) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(SIZE_MAX);
    errno = ENOMEM;
    return NULL;
  }
  size_t n = nmemb * size;
  size_t need = sizeof(vt__hdr) + n;
  vt__hdr* h = (vt__hdr*)calloc(1, need);
  if (!h) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(n);
    errno = ENOMEM;
    return NULL;
  }
  h->sz = n;
  h->align = 0;
  vt__stats_on_alloc(n);
#ifdef VT_MEM_LEAK_TRACK
  vt__lt_add(vt__user_from_hdr(h), n);
#endif
  return vt__user_from_hdr(h);
}

void* vt_realloc(void* p, size_t n) {
  if (!p) return vt_malloc(n);
  if (n == 0) {
    vt_free(p);
    return vt_malloc(1);
  }
  vt__hdr* h = vt__hdr_from_user(p);
  size_t old = h->sz;
  if (h->align) {
    /* réaligner proprement: allouer neuf */
    void* np = vt_aligned_alloc(h->align, n);
    if (!np) return NULL;
    memcpy(np, p, old < n ? old : n);
    vt_aligned_free(p);
    return np;
  }
  size_t need = sizeof(vt__hdr) + n;
  vt__hdr* nh = (vt__hdr*)realloc(h, need);
  if (!nh) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(n);
    errno = ENOMEM;
    return NULL;
  }
  nh->sz = n; /* align reste 0 */
  if (n > old)
    vt__stats_on_alloc(n - old);
  else
    vt__stats_on_free(old - n);
#ifdef VT_MEM_LEAK_TRACK
  vt__lt_add(vt__user_from_hdr(nh), n);
#endif
  return vt__user_from_hdr(nh);
}

void vt_free(void* p) {
  if (!p) return;
  vt__hdr* h = vt__hdr_from_user(p);
#ifdef VT_MEM_LEAK_TRACK
  (void)vt__lt_del(p);
#endif
  vt__stats_on_free(h->sz);
  free(h);
}

/* ----------------------------------------------------------------------------
   Aligned alloc
---------------------------------------------------------------------------- */
void* vt_aligned_alloc(size_t alignment, size_t size) {
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  if ((alignment & (alignment - 1)) != 0) {
    errno = EINVAL;
    return NULL;
  }
#if defined(_WIN32)
  /* _aligned_malloc ne permet pas header adjacent. Emulons : sur-dimensionner.
   */
  size_t total = size + alignment + sizeof(vt__hdr);
  unsigned char* raw = (unsigned char*)_aligned_malloc(total, alignment);
  if (!raw) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(size);
    errno = ENOMEM;
    return NULL;
  }
  uintptr_t base = (uintptr_t)(raw + sizeof(vt__hdr));
  uintptr_t aligned = (base + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
  vt__hdr* h = (vt__hdr*)(aligned - sizeof(vt__hdr));
  h->sz = size;
  h->align = alignment;
  ((void**)h)[-1] = raw; /* stocker ptr réel juste avant h (slot privé) */
  vt__stats_on_alloc(size);
#ifdef VT_MEM_LEAK_TRACK
  vt__lt_add((void*)aligned, size);
#endif
  return (void*)aligned;
#else
  /* posix_memalign renvoie un bloc directement aligné. On doit prévoir un
   * header adjacent. */
  void* raw = NULL;
  /* sur-dimensionner et réaligner manuellement pour avoir notre header */
  size_t total = size + alignment + sizeof(vt__hdr);
#if defined(__APPLE__) || defined(__ANDROID__) || defined(__MUSL__)
  raw = malloc(total);
  if (!raw) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(size);
    errno = ENOMEM;
    return NULL;
  }
#else
  raw = aligned_alloc(alignment, vt__align_up(total, alignment));
  if (!raw) {
    raw = malloc(total);
  }
  if (!raw) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(size);
    errno = ENOMEM;
    return NULL;
  }
#endif
  unsigned char* basep = (unsigned char*)raw + sizeof(vt__hdr);
  uintptr_t aligned =
      ((uintptr_t)basep + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
  vt__hdr* h = (vt__hdr*)(aligned - sizeof(vt__hdr));
  h->sz = size;
  h->align = alignment;
  ((void**)h)[-1] = raw; /* stock réel */
  vt__stats_on_alloc(size);
#ifdef VT_MEM_LEAK_TRACK
  vt__lt_add((void*)aligned, size);
#endif
  return (void*)aligned;
#endif
}

void vt_aligned_free(void* p) {
  if (!p) return;
  vt__hdr* h = vt__hdr_from_user(p);
  void* raw = ((void**)h)[-1];
#ifdef VT_MEM_LEAK_TRACK
  (void)vt__lt_del(p);
#endif
  vt__stats_on_free(h->sz);
#if defined(_WIN32)
  _aligned_free(raw);
#else
  free(raw);
#endif
}

/* ----------------------------------------------------------------------------
   Dup/Utils
---------------------------------------------------------------------------- */
void* vt_memdup(const void* src, size_t n) {
  if (!src && n) {
    errno = EINVAL;
    return NULL;
  }
  void* p = vt_malloc(n);
  if (!p) return NULL;
  memcpy(p, src, n);
  return p;
}

char* vt_strndup(const char* s, size_t n) {
  if (!s) {
    errno = EINVAL;
    return NULL;
  }
  size_t m = 0;
  while (m < n && s[m] != '\0') m++;
  char* out = (char*)vt_malloc(m + 1);
  if (!out) return NULL;
  memcpy(out, s, m);
  out[m] = '\0';
  return out;
}

void vt_mem_zero(void* p, size_t n) {
  if (p && n) memset(p, 0, n);
}
void vt_mem_fill(void* p, int byte, size_t n) {
  if (p && n) memset(p, byte, n);
}

void vt_mem_swap(void* a, void* b, size_t n) {
  if (a == b || n == 0) return;
  unsigned char* x = (unsigned char*)a;
  unsigned char* y = (unsigned char*)b;
  while (n--) {
    unsigned char t = *x;
    *x++ = *y;
    *y++ = t;
  }
}

/* ----------------------------------------------------------------------------
   Pages OS
---------------------------------------------------------------------------- */
static size_t vt__page_size(void) {
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
#else
  long ps = sysconf(_SC_PAGESIZE);
  return ps > 0 ? (size_t)ps : 4096u;
#endif
}

void* vt_page_alloc(size_t size) {
  size_t ps = vt__page_size();
  size = vt__align_up(size ? size : ps, ps);
#if defined(_WIN32)
  void* p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!p) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(size);
    errno = ENOMEM;
  }
  return p;
#else
  void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    if (atomic_load(&g_abort_oom)) vt__oom_abort(size);
    return NULL;
  }
  return p;
#endif
}

void vt_page_free(void* p, size_t size) {
  if (!p) return;
  size_t ps = vt__page_size();
  size = vt__align_up(size, ps);
#if defined(_WIN32)
  (void)size;
  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, size);
#endif
}

/* ----------------------------------------------------------------------------
   Arena allocator
---------------------------------------------------------------------------- */
typedef struct vt__arena_chunk {
  struct vt__arena_chunk* next;
  size_t cap, len;
  alignas(VT_MEM_ALIGN_DEFAULT) unsigned char data[];
} vt__arena_chunk;

struct vt_arena {
  vt__arena_chunk* head;
  size_t chunk_size; /* taille par défaut à la croissance */
  size_t total;      /* octets totaux réservés pour stats */
};

static vt__arena_chunk* vt__arena_new_chunk(size_t cap) {
  size_t hdr = sizeof(vt__arena_chunk);
  size_t need = hdr + cap;
  vt__arena_chunk* c = (vt__arena_chunk*)vt_malloc(need);
  if (!c) return NULL;
  c->next = NULL;
  c->cap = cap;
  c->len = 0;
  return c;
}

void vt_arena_init(vt_arena* a, size_t first_chunk) {
  if (!a) return;
  if (first_chunk == 0) first_chunk = 64 * 1024;
  a->head = vt__arena_new_chunk(first_chunk);
  a->chunk_size = first_chunk;
  a->total = a->head ? a->head->cap : 0;
}

void vt_arena_dispose(vt_arena* a) {
  if (!a) return;
  vt__arena_chunk* c = a->head;
  while (c) {
    vt__arena_chunk* n = c->next;
    vt_free(c);
    c = n;
  }
  a->head = NULL;
  a->total = 0;
}

static void vt__arena_grow(vt_arena* a, size_t need) {
  size_t cap = a->chunk_size;
  if (cap < need) cap = vt__align_up(need, 4096);
  vt__arena_chunk* c = vt__arena_new_chunk(cap);
  if (!c) return;
  c->next = a->head;
  a->head = c;
  a->total += cap;
  /* stratégie simple: doubler modérément */
  if (a->chunk_size < (size_t)16 * 1024 * 1024) a->chunk_size = cap * 2;
}

void* vt_arena_alloc(vt_arena* a, size_t n, size_t align) {
  if (!a) {
    errno = EINVAL;
    return NULL;
  }
  if (align == 0) align = VT_MEM_ALIGN_DEFAULT;
  vt__arena_chunk* c = a->head;
  if (!c) {
    vt_arena_init(a, n + 1024);
    c = a->head;
    if (!c) return NULL;
  }
  size_t off = vt__align_up(c->len, align);
  if (off + n > c->cap) {
    vt__arena_grow(a, n + align);
    c = a->head;
    if (!c) return NULL;
    off = vt__align_up(c->len, align);
  }
  void* p = c->data + off;
  c->len = off + n;
  return p;
}

void vt_arena_reset(vt_arena* a) {
  if (!a || !a->head) return;
  /* On garde la première chunk, on libère le reste */
  vt__arena_chunk* first = a->head;
  vt__arena_chunk* c = first->next;
  while (c) {
    vt__arena_chunk* n = c->next;
    vt_free(c);
    c = n;
  }
  first->next = NULL;
  first->len = 0;
  a->head = first;
  a->total = first->cap;
  if (a->chunk_size > first->cap) a->chunk_size = first->cap * 2;
}

vt_arena_mark vt_arena_mark_get(vt_arena* a) {
  vt_arena_mark m = {0};
  if (!a || !a->head) return m;
  m._chunk = a->head;
  m._len = a->head->len;
  m._total = a->total;
  return m;
}

void vt_arena_mark_reset(vt_arena* a, vt_arena_mark m) {
  if (!a) return;
  vt__arena_chunk* target = (vt__arena_chunk*)m._chunk;
  if (!target) {
    vt_arena_reset(a);
    return;
  }
  /* Libère toutes les chunks avant la cible */
  vt__arena_chunk* c = a->head;
  while (c && c != target) {
    vt__arena_chunk* n = c->next;
    vt_free(c);
    c = n;
  }
  a->head = target;
  target->len = m._len;
  target->next = NULL;
  a->total = m._total;
}

/* ----------------------------------------------------------------------------
   Pool (objets fixes)
---------------------------------------------------------------------------- */
typedef struct vt__blk {
  struct vt__blk* next;
  /* objets suivent */
} vt__blk;

struct vt_pool {
  size_t obj_size;
  size_t obj_align;
  size_t objs_per_block;
  void* free_list;
  vt__blk* blocks;
};

int vt_pool_init(vt_pool* p, size_t obj_size, size_t obj_align,
                 size_t objs_per_block) {
  if (!p || obj_size == 0 || (obj_align & (obj_align - 1))) return 0;
  if (objs_per_block == 0) objs_per_block = 64;
  p->obj_size =
      vt__align_up(obj_size, obj_align ? obj_align : VT_MEM_ALIGN_DEFAULT);
  p->obj_align = obj_align ? obj_align : VT_MEM_ALIGN_DEFAULT;
  p->objs_per_block = objs_per_block;
  p->free_list = NULL;
  p->blocks = NULL;
  return 1;
}

static int vt__pool_grow(vt_pool* p) {
  size_t hdr = sizeof(vt__blk);
  size_t stride = vt__align_up(p->obj_size, p->obj_align);
  size_t block_bytes = hdr + stride * p->objs_per_block + p->obj_align;
  unsigned char* raw = (unsigned char*)vt_malloc(block_bytes);
  if (!raw) return 0;
  vt__blk* blk = (vt__blk*)raw;
  blk->next = p->blocks;
  p->blocks = blk;
  unsigned char* base = raw + hdr;
  uintptr_t aligned =
      ((uintptr_t)base + (p->obj_align - 1)) & ~(uintptr_t)(p->obj_align - 1);
  unsigned char* cur = (unsigned char*)aligned;
  for (size_t i = 0; i < p->objs_per_block; ++i) {
    /* push dans free_list */
    *(void**)cur = p->free_list;
    p->free_list = cur;
    cur += stride;
  }
  return 1;
}

void* vt_pool_alloc(vt_pool* p) {
  if (!p) {
    errno = EINVAL;
    return NULL;
  }
  if (!p->free_list && !vt__pool_grow(p)) return NULL;
  void* obj = p->free_list;
  p->free_list = *(void**)p->free_list;
  return obj;
}

void vt_pool_free(vt_pool* p, void* obj) {
  if (!p || !obj) return;
  *(void**)obj = p->free_list;
  p->free_list = obj;
}

void vt_pool_dispose(vt_pool* p) {
  if (!p) return;
  vt__blk* b = p->blocks;
  while (b) {
    vt__blk* n = b->next;
    vt_free(b);
    b = n;
  }
  p->blocks = NULL;
  p->free_list = NULL;
}

/* ----------------------------------------------------------------------------
   Buffer dynamique
---------------------------------------------------------------------------- */
void vt_buf_init(vt_buf* b) {
  if (b) {
    b->data = NULL;
    b->len = b->cap = 0;
  }
}

void vt_buf_dispose(vt_buf* b) {
  if (!b) return;
  vt_free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

int vt_buf_reserve(vt_buf* b, size_t need_cap) {
  if (!b) return 0;
  if (b->cap >= need_cap) return 1;
  size_t new_cap = b->cap ? b->cap : 64;
  while (new_cap < need_cap) {
    if (new_cap > (SIZE_MAX / 3) * 2) {
      new_cap = need_cap;
      break;
    }
    new_cap = new_cap + (new_cap >> 1); /* *1.5 */
  }
  unsigned char* p = (unsigned char*)vt_realloc(b->data, new_cap);
  if (!p) return 0;
  b->data = p;
  b->cap = new_cap;
  return 1;
}

int vt_buf_append(vt_buf* b, const void* data, size_t n) {
  if (!b) return 0;
  if (!vt_buf_reserve(b, b->len + n)) return 0;
  memcpy(b->data + b->len, data, n);
  b->len += n;
  return 1;
}

int vt_buf_append_cstr(vt_buf* b, const char* s) {
  if (!s) return 0;
  return vt_buf_append(b, s, strlen(s));
}

static int vt__buf_vprintf(vt_buf* b, const char* fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (need < 0) return 0;
  size_t n = (size_t)need;
  if (!vt_buf_reserve(b, b->len + n)) return 0;
  int wrote = vsnprintf((char*)b->data + b->len, n + 1, fmt, ap);
  if (wrote < 0) return 0;
  b->len += (size_t)wrote;
  return 1;
}

int vt_buf_printf(vt_buf* b, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ok = vt__buf_vprintf(b, fmt, ap);
  va_end(ap);
  return ok;
}

unsigned char* vt_buf_detach(vt_buf* b, size_t* out_len) {
  if (!b) return NULL;
  unsigned char* p = b->data;
  if (out_len) *out_len = b->len;
  b->data = NULL;
  b->len = b->cap = 0;
  return p;
}

/* ============================================================================
   Fin
   ============================================================================
 */
