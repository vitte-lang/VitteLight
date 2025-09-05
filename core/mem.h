/* ============================================================================
   mem.h — Allocation & utilitaires mémoire (C17) — API ultra complète
   - Wrappers sûrs (vt_malloc/calloc/realloc/free) avec statistiques atomiques
   - Aligned alloc cross-platform
   - Pages OS (mmap/VirtualAlloc)
   - Arena allocator (grow, mark/reset, align)
   - Pool d’objets fixes (free-list)
   - Buffer dynamique (vt_buf) + printf-like
   - Duplication (mem/str), fill/zero/swap
   - Leak tracking optionnel (VT_MEM_LEAK_TRACK)
   Licence: MIT.
   ============================================================================
 */
#ifndef VT_MEM_H
#define VT_MEM_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* uintptr_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export / annotations
---------------------------------------------------------------------------- */
#ifndef VT_MEM_API
#define VT_MEM_API extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VT_PRINTF(fmt_idx, va_idx) \
  __attribute__((format(printf, fmt_idx, va_idx)))
#else
#define VT_PRINTF(fmt_idx, va_idx)
#endif

/* ----------------------------------------------------------------------------
   Configuration compile-time (optionnelle)
   - VT_MEM_LEAK_TRACK       : active le suivi de fuites (coût modéré)
   - VT_MEM_DEFAULT_ABORT_ON_OOM=1 : abort sur OOM dans vt_*alloc (par défaut 1)
---------------------------------------------------------------------------- */
#ifndef VT_MEM_DEFAULT_ABORT_ON_OOM
#define VT_MEM_DEFAULT_ABORT_ON_OOM 1
#endif

/* ----------------------------------------------------------------------------
   Statistiques globales
---------------------------------------------------------------------------- */
typedef struct vt_mem_stats {
  size_t cur_bytes;    /* octets actuellement alloués */
  size_t peak_bytes;   /* pic observé */
  size_t total_allocs; /* nombre total d’allocations */
  size_t total_frees;  /* nombre total de free */
} vt_mem_stats;

/* ----------------------------------------------------------------------------
   API de base (wrappers sûrs)
---------------------------------------------------------------------------- */
VT_MEM_API void vt_mem_init(void);
VT_MEM_API void vt_mem_shutdown(void);
VT_MEM_API void vt_mem_get_stats(vt_mem_stats* out);
VT_MEM_API void vt_mem_set_abort_on_oom(int on); /* 0=retourne NULL, 1=abort */

VT_MEM_API void* vt_malloc(size_t n);
VT_MEM_API void* vt_calloc(size_t nmemb, size_t size);
VT_MEM_API void* vt_realloc(void* p, size_t n);
VT_MEM_API void vt_free(void* p);

VT_MEM_API void* vt_aligned_alloc(size_t alignment, size_t size);
VT_MEM_API void vt_aligned_free(void* p);

VT_MEM_API void* vt_memdup(const void* src, size_t n);
VT_MEM_API char* vt_strndup(const char* s, size_t n);

VT_MEM_API void vt_mem_zero(void* p, size_t n);
VT_MEM_API void vt_mem_fill(void* p, int byte, size_t n);
VT_MEM_API void vt_mem_swap(void* a, void* b, size_t n);

/* ----------------------------------------------------------------------------
   Pages OS (grands buffers page-alignés)
---------------------------------------------------------------------------- */
VT_MEM_API void* vt_page_alloc(size_t size);        /* size arrondi à la page */
VT_MEM_API void vt_page_free(void* p, size_t size); /* size identique à alloc */

/* ----------------------------------------------------------------------------
   Arena allocator
   - vt_arena : opaque côté utilisateur
   - Mark/reset pour rollback O(1)
---------------------------------------------------------------------------- */
typedef struct vt_arena vt_arena;

typedef struct vt_arena_mark {
  void* _chunk;  /* opaque */
  size_t _len;   /* offset sauvegardé */
  size_t _total; /* total bytes dans l’arena au moment du mark */
} vt_arena_mark;

/* Crée la première chunk (taille suggérée). Si 0 → ~64 KiB par défaut. */
VT_MEM_API void vt_arena_init(vt_arena* a, size_t first_chunk_bytes);
VT_MEM_API void vt_arena_dispose(vt_arena* a);

/* Alloue n octets, alignés sur `align` (puissance de 2 ; 0 → align par défaut).
 */
VT_MEM_API void* vt_arena_alloc(vt_arena* a, size_t n, size_t align);

/* Remet l’arena à zéro mais conserve la première chunk. */
VT_MEM_API void vt_arena_reset(vt_arena* a);

/* Sauvegarde et restaure une position logique de l’arena. */
VT_MEM_API vt_arena_mark vt_arena_mark_get(vt_arena* a);
VT_MEM_API void vt_arena_mark_reset(vt_arena* a, vt_arena_mark m);

/* ----------------------------------------------------------------------------
   Pool d’objets fixes (free-list)
   - vt_pool : opaque
---------------------------------------------------------------------------- */
typedef struct vt_pool vt_pool;

/* obj_align puissance de 2 (0 → align défaut). objs_per_block=0 → 64. */
VT_MEM_API int vt_pool_init(vt_pool* p, size_t obj_size, size_t obj_align,
                            size_t objs_per_block);
VT_MEM_API void vt_pool_dispose(vt_pool* p);
VT_MEM_API void* vt_pool_alloc(vt_pool* p);
VT_MEM_API void vt_pool_free(vt_pool* p, void* obj);

/* ----------------------------------------------------------------------------
   Buffer dynamique (octets) avec helpers printf-like
---------------------------------------------------------------------------- */
typedef struct vt_buf {
  unsigned char* data;
  size_t len;
  size_t cap;
} vt_buf;

VT_MEM_API void vt_buf_init(vt_buf* b);
VT_MEM_API void vt_buf_dispose(vt_buf* b);
VT_MEM_API int vt_buf_reserve(vt_buf* b, size_t need_cap);
VT_MEM_API int vt_buf_append(vt_buf* b, const void* data, size_t n);
VT_MEM_API int vt_buf_append_cstr(vt_buf* b, const char* s);
VT_MEM_API int vt_buf_printf(vt_buf* b, const char* fmt, ...) VT_PRINTF(2, 3);
/* Détache le buffer (ownership transféré). Remet b à vide. */
VT_MEM_API unsigned char* vt_buf_detach(vt_buf* b, size_t* out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_MEM_H */
