/* ============================================================================
   jumptab.h — Tables de dispatch rapides pour opcodes (C17).
   Signature unique: handler(void* ctx, uint32_t op). Fallback par défaut.
   Zéro dépendance. Licence: MIT.
   ============================================================================
 */
#ifndef VT_JUMPTAB_H
#define VT_JUMPTAB_H
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Export */
#ifndef VT_JMP_API
#define VT_JMP_API extern
#endif

/* Attributs optionnels */
#if defined(__GNUC__) || defined(__clang__)
#define VT_JMP_HOT __attribute__((hot))
#define VT_JMP_COLD __attribute__((cold))
#else
#define VT_JMP_HOT
#define VT_JMP_COLD
#endif

/* ---------------------------------------------------------------------------
   Types
--------------------------------------------------------------------------- */
typedef void (*vt_jmp_fn)(void* ctx, uint32_t op);

typedef struct {
  vt_jmp_fn* fns; /* tableau d’handlers (NULL = absent) */
  size_t n;       /* taille du tableau */
  vt_jmp_fn dflt; /* handler par défaut (peut être NULL) */
} vt_jumptab;

/* ---------------------------------------------------------------------------
   Construction
   - Option 1 (statique): utilisez VT_JMP_TABLE(...) ci-dessous.
   - Option 2 (dynamique): allouez un tableau puis init().
--------------------------------------------------------------------------- */
VT_JMP_API void vt_jmp_init(vt_jumptab* t, vt_jmp_fn* fns, size_t n,
                            vt_jmp_fn dflt);
VT_JMP_API void vt_jmp_set(vt_jumptab* t, uint32_t op,
                           vt_jmp_fn fn); /* hors bornes = no-op */
VT_JMP_API vt_jmp_fn vt_jmp_get(const vt_jumptab* t,
                                uint32_t op); /* hors bornes => NULL */

/* ---------------------------------------------------------------------------
   Dispatch
   - Retour: 0=handler direct, 1=default, -1=aucun (invalide sans default).
--------------------------------------------------------------------------- */
VT_JMP_API int VT_JMP_HOT vt_jmp_dispatch(const vt_jumptab* t, uint32_t op,
                                          void* ctx);
VT_JMP_API int VT_JMP_HOT vt_jmp_dispatch_masked(const vt_jumptab* t,
                                                 uint32_t op_masked, void* ctx);
/* Variante sûre: vérifie la borne puis appelle, sinon -1 (n’appelle pas
 * default) */
VT_JMP_API int VT_JMP_HOT vt_jmp_try(const vt_jumptab* t, uint32_t op,
                                     void* ctx);

/* Outils */
VT_JMP_API void vt_jmp_fill(vt_jumptab* t, vt_jmp_fn fn); /* remplit tout */
VT_JMP_API void vt_jmp_fill_range(vt_jumptab* t, uint32_t lo, uint32_t hi,
                                  vt_jmp_fn fn); /* [lo,hi] clip */

/* ---------------------------------------------------------------------------
   Macro de table statique
   - Déclare un tableau et un vt_jumptab constant.
   - Initialisez avec positions explicites si besoin: [0]=h0, [5]=h5, ...
   - Les entrées non précisées valent NULL et tomberont sur dflt si présent.
--------------------------------------------------------------------------- */
/* Exemple:
     void op_add(void*,uint32_t); void op_sub(void*,uint32_t); void
   op_default(void*,uint32_t); VT_JMP_TABLE(ops, op_default, [0]=op_add,
         [1]=op_sub
     );
     // vt_jmp_dispatch(&ops, opcode, ctx);
*/
#define VT_JMP_TABLE(NAME, DEFAULT_FN, ...)      \
  static vt_jmp_fn NAME##_fns[] = {__VA_ARGS__}; \
  static const vt_jumptab NAME = {               \
      NAME##_fns, sizeof(NAME##_fns) / sizeof(NAME##_fns[0]), (DEFAULT_FN)}

/* ---------------------------------------------------------------------------
   Implémentation header-only (inline)
--------------------------------------------------------------------------- */
#ifndef VT_JUMPTAB_NO_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define VTJ_INLINE static __inline__ __attribute__((always_inline))
#else
#define VTJ_INLINE static inline
#endif

VTJ_INLINE void vt_jmp_init(vt_jumptab* t, vt_jmp_fn* fns, size_t n,
                            vt_jmp_fn dflt) {
  if (!t) return;
  t->fns = fns;
  t->n = n;
  t->dflt = dflt;
}
VTJ_INLINE void vt_jmp_set(vt_jumptab* t, uint32_t op, vt_jmp_fn fn) {
  if (!t || !t->fns) return;
  if (op < t->n) t->fns[op] = fn;
}
VTJ_INLINE vt_jmp_fn vt_jmp_get(const vt_jumptab* t, uint32_t op) {
  if (!t || !t->fns || op >= t->n) return (vt_jmp_fn)0;
  return t->fns[op];
}
VTJ_INLINE int vt_jmp_dispatch(const vt_jumptab* t, uint32_t op, void* ctx) {
  if (!t || !t->fns) return -1;
  if (op < t->n) {
    vt_jmp_fn fn = t->fns[op];
    if (fn) {
      fn(ctx, op);
      return 0;
    }
  }
  if (t->dflt) {
    t->dflt(ctx, op);
    return 1;
  }
  return -1;
}
VTJ_INLINE int vt_jmp_try(const vt_jumptab* t, uint32_t op, void* ctx) {
  if (!t || !t->fns || op >= t->n) return -1;
  vt_jmp_fn fn = t->fns[op];
  if (!fn) return -1;
  fn(ctx, op);
  return 0;
}
/* Pour tailles power-of-two: op_masked = op & (n-1). Ne vérifie pas les NULL.
 */
VTJ_INLINE int vt_jmp_dispatch_masked(const vt_jumptab* t, uint32_t op_masked,
                                      void* ctx) {
  if (!t || !t->fns || t->n == 0) return -1;
  uint32_t i = op_masked & (uint32_t)(t->n - 1u);
  vt_jmp_fn fn = t->fns[i];
  if (fn) {
    fn(ctx, i);
    return 0;
  }
  if (t->dflt) {
    t->dflt(ctx, i);
    return 1;
  }
  return -1;
}
VTJ_INLINE void vt_jmp_fill(vt_jumptab* t, vt_jmp_fn fn) {
  if (!t || !t->fns) return;
  for (size_t i = 0; i < t->n; ++i) t->fns[i] = fn;
}
VTJ_INLINE void vt_jmp_fill_range(vt_jumptab* t, uint32_t lo, uint32_t hi,
                                  vt_jmp_fn fn) {
  if (!t || !t->fns || lo > hi) return;
  if (lo >= t->n) return;
  size_t end = hi + 1u;
  if (end > t->n) end = t->n;
  for (size_t i = lo; i < end; ++i) t->fns[i] = fn;
}
#endif /* VT_JUMPTAB_NO_INLINE */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_JUMPTAB_H */
