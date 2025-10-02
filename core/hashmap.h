/* ============================================================================
   hashmap.h — Table de hachage générique (C17, MIT)
   - Open addressing avec Robin Hood + tombstones
   - API générique: callbacks hash/equals + free
   - Variante string map (clés UTF-8 dupliquées)
   ============================================================================
 */
#ifndef VT_HASHMAP_H
#define VT_HASHMAP_H
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VT_HASHMAP_API
#define VT_HASHMAP_API extern
#endif

/* ----------------------------------------------------------------------------
   Types de callback
---------------------------------------------------------------------------- */
typedef uint64_t (*vt_hash_fn)(const void* key, void* udata);
typedef int      (*vt_eq_fn)(const void* a, const void* b, void* udata);
typedef void     (*vt_key_free_fn)(void* key, void* udata);

/* opaque */
typedef struct vt_hashmap vt_hashmap;

/* ----------------------------------------------------------------------------
   API générique
---------------------------------------------------------------------------- */

/* construction / destruction */
VT_HASHMAP_API vt_hashmap* vt_hashmap_new(vt_hash_fn hf, vt_eq_fn eq,
                                          vt_key_free_fn kfree,
                                          void* udata);
VT_HASHMAP_API void        vt_hashmap_free(vt_hashmap* m);

/* stats */
VT_HASHMAP_API size_t vt_hashmap_len(const vt_hashmap* m);
VT_HASHMAP_API size_t vt_hashmap_capacity(const vt_hashmap* m);

/* opérations principales */
VT_HASHMAP_API int vt_hashmap_put(vt_hashmap* m, void* key, void* value); /* 0/-1 */
VT_HASHMAP_API int vt_hashmap_get(const vt_hashmap* m, const void* key, void** out_val); /* 1=found,0=miss */
VT_HASHMAP_API int vt_hashmap_del(vt_hashmap* m, const void* key); /* 1=deleted,0=miss */

/* itération */
typedef int (*vt_hashmap_iter_fn)(void* key, void* value, void* udata);
VT_HASHMAP_API void vt_hashmap_foreach(vt_hashmap* m, vt_hashmap_iter_fn it, void* udata);

/* ----------------------------------------------------------------------------
   Variante « string map » — clés char* dupliquées (free auto)
---------------------------------------------------------------------------- */
VT_HASHMAP_API vt_hashmap* vt_hashmap_new_string(void);
VT_HASHMAP_API int         vt_hashmap_put_str(vt_hashmap* m, const char* key, void* value);
VT_HASHMAP_API int         vt_hashmap_get_str(const vt_hashmap* m, const char* key, void** out_val);
VT_HASHMAP_API int         vt_hashmap_del_str(vt_hashmap* m, const char* key);

#ifdef __cplusplus
}
#endif
#endif /* VT_HASHMAP_H */
