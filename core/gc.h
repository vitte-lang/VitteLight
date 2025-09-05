/* ============================================================================
   gc.h — Collecteur mark-sweep précis avec racines explicites (C17).
   S’associe à gc.c. Licence: MIT.
   ============================================================================
 */
#ifndef VT_GC_H
#define VT_GC_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint*_t */
#include <stdio.h>  /* FILE */

#ifdef __cplusplus
extern "C" {
#endif

/* Export */
#ifndef VT_GC_API
#define VT_GC_API extern
#endif

/* Types opaques */
typedef struct vt_gc vt_gc;

/* Callbacks de tracing et finalisation
   - trace(obj, visit, ctx) doit appeler visit(child, ctx) pour chaque pointeur
   enfant géré. */
typedef void (*vt_gc_visit_fn)(void* child, void* ctx);
typedef void (*vt_gc_trace_fn)(void* obj, vt_gc_visit_fn visit, void* ctx);
typedef void (*vt_gc_finalizer)(void* obj);

/* Configuration runtime */
typedef struct {
  size_t heap_limit_bytes; /* 0 = valeur par défaut (8 MiB) */
  int enable_logging;      /* 1 = logs sur stderr */
} vt_gc_config;

/* Cycle de vie du GC */
VT_GC_API vt_gc* vt_gc_create(const vt_gc_config* cfg);
VT_GC_API void vt_gc_destroy(vt_gc* gc);

/* Allocation gérée
   - size: taille du payload
   - trace: callback de marquage (peut être NULL pour objet feuille)
   - fin: finalizer optionnel (peut être NULL)
   - tag: étiquette utilisateur (debug/typage) */
VT_GC_API void* vt_gc_alloc(vt_gc* gc, size_t size, vt_gc_trace_fn trace,
                            vt_gc_finalizer fin, uint32_t tag);

/* Déclenche un GC complet (raison libre, peut être NULL) */
VT_GC_API void vt_gc_collect(vt_gc* gc, const char* reason);

/* Racines explicites
   - slot est l’adresse d’un pointeur (void**) que vous mettez/retirez du set de
   racines. */
VT_GC_API void vt_gc_add_root(vt_gc* gc, void** slot);
VT_GC_API void vt_gc_remove_root(vt_gc* gc, void** slot);

/* Épinglage d’objets (empêche la collecte tant que pin>0) */
VT_GC_API void vt_gc_pin(void* obj);
VT_GC_API void vt_gc_unpin(void* obj);

/* Limite d’heap dynamique */
VT_GC_API void vt_gc_set_limit(vt_gc* gc, size_t bytes);

/* Stats et introspection */
VT_GC_API size_t vt_gc_bytes_live(vt_gc* gc);
VT_GC_API size_t vt_gc_object_count(vt_gc* gc);
VT_GC_API uint32_t vt_gc_tag_of(const void* obj);
VT_GC_API void vt_gc_set_tag(void* obj, uint32_t tag);

/* Dump debug (passer NULL → stderr) */
VT_GC_API void vt_gc_dump(vt_gc* gc, FILE* out);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_GC_H */
