/* ============================================================================
   table.h — Hash table générique (C17), robin-hood open addressing.
   - Clés = octets arbitraires (ptr + taille). Valeurs = void*.
   - Callbacks configurables: hash, égalité, free(key), free(val).
   - Copy-on-insert optionnel des clés (copy_keys=1) ou adoption.
   - Effacement par backward-shift. Rehash auto. Itération simple.
   - Thread-safety: non incluse.
   Lier avec table.c. Licence: MIT.
   ============================================================================
 */
#ifndef VT_TABLE_H
#define VT_TABLE_H
#pragma once

#include <stdbool.h> /* bool */
#include <stddef.h>  /* size_t */
#include <stdint.h>  /* uint64_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export
---------------------------------------------------------------------------- */
#ifndef VT_TABLE_API
#define VT_TABLE_API extern
#endif

/* ----------------------------------------------------------------------------
   Types publics
---------------------------------------------------------------------------- */
typedef struct vt_table vt_table; /* opaque */

/* Itérateur simple (stateful). Initialiser avec vt_table_iter_init(). */
typedef struct vt_table_iter {
  size_t idx; /* interne */
} vt_table_iter;

/* Callbacks utilisateur */
typedef uint64_t (*vt_hash_fn)(const void* key, size_t klen, void* udata);
/* doit retourner !=0 seulement si a==b, 0 sinon */
typedef int (*vt_keyeq_fn)(const void* a, size_t alen, const void* b,
                           size_t blen, void* udata);
typedef void (*vt_free_fn)(void* p, void* udata);

/* Configuration de la table */
typedef struct vt_table_config {
  vt_hash_fn hash;     /* défaut: FNV-1a 64 + avalanche */
  vt_keyeq_fn eq;      /* défaut: memcmp + taille */
  vt_free_fn free_key; /* appelé sur clé lors de destroy/erase si pertinent */
  vt_free_fn
      free_val; /* appelé sur valeur lors de destroy/erase/remplacement */
  void* udata;  /* opaque pour callbacks */

  float max_load;     /* 0→0.85f par défaut. Ex: 0.90f */
  size_t initial_cap; /* puissance de 2 recommandée. 0→16 */

  int copy_keys;       /* 1: table duplique la clé à l’insertion. 0: adopte le
                          pointeur */
  int free_key_always; /* 1: toujours free_key(k) à la suppression même si
                          copy_keys=0 */
} vt_table_config;

/* ----------------------------------------------------------------------------
   Cycle de vie
---------------------------------------------------------------------------- */
VT_TABLE_API int vt_table_init(vt_table* t, const vt_table_config* cfg);
VT_TABLE_API void vt_table_free(vt_table* t);
VT_TABLE_API void vt_table_clear(vt_table* t);
VT_TABLE_API void vt_table_reserve(vt_table* t, size_t n);
VT_TABLE_API size_t vt_table_len(const vt_table* t);

/* Contrôle stratégie des clés (copy/adopt) après init (non thread-safe). */
VT_TABLE_API void vt_table_set_copy_keys(vt_table* t, bool copy);

/* ----------------------------------------------------------------------------
   Accès
   - key = pointeur vers la clé, klen = taille en octets.
   - val = pointeur valeur stockée tel quel (void*).
---------------------------------------------------------------------------- */
/* Insert/replace: si clé existante → remplace valeur et renvoie ancienne via
 * old_val_out. */
VT_TABLE_API bool vt_table_put(vt_table* t, const void* key, size_t klen,
                               void* val, void** old_val_out);

/* Récupère la valeur. Renvoie false si absent. */
VT_TABLE_API bool vt_table_get(const vt_table* t, const void* key, size_t klen,
                               void** val_out);

/* Version commodité: renvoie directement le pointeur valeur ou NULL si absent.
 */
VT_TABLE_API void* vt_table_getptr(const vt_table* t, const void* key,
                                   size_t klen);

/* Supprime l’entrée. Optionnel: récupère l’ancienne valeur. */
VT_TABLE_API bool vt_table_del(vt_table* t, const void* key, size_t klen,
                               void** old_val_out);

/* Remplace la valeur uniquement si la clé existe. */
VT_TABLE_API bool vt_table_replace(vt_table* t, const void* key, size_t klen,
                                   void* val, void** old_val_out);

/* Test existence. */
VT_TABLE_API bool vt_table_has(const vt_table* t, const void* key, size_t klen);

/* ----------------------------------------------------------------------------
   Itération
   Exemple:
       vt_table_iter it; vt_table_iter_init(&it);
       const void* k; size_t kl; void* v;
       while (vt_table_next(&tab, &it, &k, &kl, &v)) { ... }
---------------------------------------------------------------------------- */
VT_TABLE_API void vt_table_iter_init(vt_table_iter* it);
VT_TABLE_API bool vt_table_next(vt_table* t, vt_table_iter* it,
                                const void** kptr, size_t* klen, void** vptr);

/* ----------------------------------------------------------------------------
   Hash utils (par défauts compatibles config.hash / eq)
---------------------------------------------------------------------------- */
VT_TABLE_API uint64_t vt_hash_bytes(const void* p, size_t n);
VT_TABLE_API uint64_t vt_hash_cstr(const char* z); /* strlen(z) si z != NULL */
VT_TABLE_API int vt_keyeq_bytes(const void* a, size_t alen, const void* b,
                                size_t blen);

/* ----------------------------------------------------------------------------
   Debug interne (optionnel)
---------------------------------------------------------------------------- */
#ifndef NDEBUG
VT_TABLE_API void vt_table__self_check(const vt_table* t);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_TABLE_H */
