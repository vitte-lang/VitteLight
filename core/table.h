// vitte-light/core/table.h
// Table de hachage générique (Robin Hood + backshift). Clés/valeurs opaques.
// Implémentation: core/table.c

#ifndef VITTE_LIGHT_CORE_TABLE_H
#define VITTE_LIGHT_CORE_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#if !defined(ssize_t)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#else
#include <sys/types.h>
#endif

// ───────────────────── Signatures de hooks ─────────────────────
typedef uint32_t (*VL_HashFn)(const void *key, void *udata);
typedef int (*VL_EqualFn)(const void *a, const void *b, void *udata);
typedef void *(*VL_RetainFn)(
    void *ptr, void *udata);  // peut copier/retain, peut retourner ptr
typedef void (*VL_ReleaseFn)(void *ptr, void *udata);  // free/release

// ───────────────────── Type opaque ─────────────────────
typedef struct VL_Table VL_Table;  // contenu privé à table.c

// ───────────────────── API principale ─────────────────────
void vl_tab_init(VL_Table *t, size_t initial_cap, VL_HashFn hf, VL_EqualFn eq,
                 VL_RetainFn kret, VL_ReleaseFn kfree, VL_RetainFn vret,
                 VL_ReleaseFn vfree, void *udata);
void vl_tab_release(VL_Table *t);

size_t vl_tab_len(const VL_Table *t);
size_t vl_tab_cap(const VL_Table *t);
int vl_tab_reserve(VL_Table *t, size_t min_cap);

// put remplace si la clé existe déjà. Retourne 1 si succès, 0 sinon.
int vl_tab_put(VL_Table *t, const void *key, const void *val);
int vl_tab_get(const VL_Table *t, const void *key, void **out_val);
int vl_tab_del(VL_Table *t, const void *key);

// Itération: passer i=-1 au premier appel. Retourne l’index ou -1 si fini.
ssize_t vl_tab_next(const VL_Table *t, ssize_t i, void **out_key,
                    void **out_val);

// ───────────────────── Helpers C-string ─────────────────────
uint32_t vl_hash_cstr(const void *k, void *udata);
int vl_eq_cstr(const void *a, const void *b, void *udata);

// Init pratique: hash/eq configurés pour char*, clé dupliquée automatiquement.
void vl_tab_init_cstr(VL_Table *t, size_t initial_cap);
int vl_tab_put_cstr(VL_Table *t, const char *key, const void *val);
int vl_tab_get_cstr(const VL_Table *t, const char *key, void **out_val);
int vl_tab_del_cstr(VL_Table *t, const char *key);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_TABLE_H
