// vitte-light/core/mem.h
// API mémoire utilitaire: allocations sûres, arène, pool fixe, buffer
// dynamique. Implémentation: core/mem.c

#ifndef VITTE_LIGHT_CORE_MEM_H
#define VITTE_LIGHT_CORE_MEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>  // va_list
#include <stddef.h>
#include <stdint.h>

#ifndef VL_DEFAULT_ALIGN
#define VL_DEFAULT_ALIGN ((size_t)(sizeof(void *) * 2u))
#endif

// ───────────────────── OOM & stats ─────────────────────

typedef void (*VL_OOM_Handler)(size_t bytes);
void vl_mem_set_oom_handler(VL_OOM_Handler fn);
void vl_mem_stats(size_t *allocd, size_t *freed, size_t *live);

// ───────────────────── Wrappers d'allocation ─────────────────────
// suffixe _s: retourne NULL en cas d'échec. suffixe _x: abort via handler OOM.
void *vl_malloc_s(size_t n);
void *vl_calloc_s(size_t c, size_t n);
void *vl_realloc_s(void *p, size_t n);
void vl_free(void *p, size_t n_hint);

void *vl_malloc_x(size_t n);
void *vl_calloc_x(size_t c, size_t n);
void *vl_realloc_x(void *p, size_t n);

// ───────────────────── Allocation alignée ─────────────────────
void *vl_aligned_alloc(size_t align, size_t n);
void vl_aligned_free(void *p);

// ───────────────────── Arène (bump allocator) ─────────────────────

typedef struct VL_ArenaChunk VL_ArenaChunk;  // opaque pour l'utilisateur

typedef struct VL_Arena {
  VL_ArenaChunk *head;
  size_t chunk_size;
  size_t align;
  size_t total_bytes;  // stats cumulées
  size_t peak_used;    // plus gros "used" dans la tête
  size_t n_chunks;
} VL_Arena;

typedef struct {
  VL_ArenaChunk *chunk;
  size_t used;
} VL_ArenaMark;

void vl_arena_init(VL_Arena *A, size_t chunk_size, size_t align);
void vl_arena_release(VL_Arena *A);
void *vl_arena_alloc(VL_Arena *A, size_t n);
void *vl_arena_alloc_aligned(VL_Arena *A, size_t n, size_t align);
char *vl_arena_strdup(VL_Arena *A, const char *s);
char *vl_arena_strndup(VL_Arena *A, const char *s, size_t n);
VL_ArenaMark vl_arena_mark(const VL_Arena *A);
void vl_arena_reset_to(VL_Arena *A, VL_ArenaMark m);

// ───────────────────── Pool fixe (slab allocator) ─────────────────────

typedef struct VL_PoolSlab VL_PoolSlab;  // opaque

typedef struct VL_Pool {
  VL_PoolSlab *slabs;
  void *free_list;        // single-linked via tête de bloc
  size_t item_size;       // arrondi à VL_DEFAULT_ALIGN
  size_t items_per_slab;  // éléments par slab
  size_t slab_bytes;      // taille réelle par slab
  size_t n_items;         // objets vivants
} VL_Pool;

void vl_pool_init(VL_Pool *P, size_t item_size, size_t items_per_slab);
void *vl_pool_alloc(VL_Pool *P);
void vl_pool_free(VL_Pool *P, void *ptr);
void vl_pool_release(VL_Pool *P);

// ───────────────────── Buffer dynamique (byte builder) ─────────────────────

typedef struct VL_Buffer {
  uint8_t *d;
  size_t n, cap;
} VL_Buffer;

void vl_buf_init(VL_Buffer *b);
void vl_buf_free(VL_Buffer *b);
int vl_buf_reserve(VL_Buffer *b, size_t need);
int vl_buf_append(VL_Buffer *b, const void *src, size_t n);
int vl_buf_putc(VL_Buffer *b, int c);
int vl_buf_vprintf(VL_Buffer *b, const char *fmt, va_list ap);
int vl_buf_printf(VL_Buffer *b, const char *fmt, ...);
char *vl_buf_take_cstr(VL_Buffer *b);  // retourne et remet le buffer à zéro

// ───────────────────── Fichiers ─────────────────────
int vl_write_file(const char *path, const void *data, size_t n);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_MEM_H
