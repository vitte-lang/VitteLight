/* ============================================================================
   object.h — Système d’objets runtime (C17) pour Vitte/Vitl
   - Types: Nil, Bool, Int, Float, String, Bytes, Array, Map, Func, Ptr
   - Gestion: RC explicite (retain/release) + clone/move
   - Conteneurs: string/bytes/array/map
   - Utilitaires: hash, égalité, stringify, vérité, concat
   Dépendances d’implémentation: mem.h (vt_malloc/vt_free...), debug.h
   (optionnel) Licence: MIT.
   ============================================================================
 */
#ifndef VT_OBJECT_H
#define VT_OBJECT_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint64_t, int64_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export
---------------------------------------------------------------------------- */
#ifndef VT_OBJECT_API
#define VT_OBJECT_API extern
#endif

/* ----------------------------------------------------------------------------
   Types opaques heap (détails cachés)
---------------------------------------------------------------------------- */
typedef struct vt_str vt_str;
typedef struct vt_bytes vt_bytes;
typedef struct vt_array vt_array;
typedef struct vt_map vt_map;
typedef struct vt_func vt_func; /* si vous exposez des closures natives */

/* ----------------------------------------------------------------------------
   Tag de type et vt_value
---------------------------------------------------------------------------- */
typedef enum vt_type {
  VT_NIL = 0,
  VT_BOOL = 1,
  VT_INT = 2,
  VT_FLOAT = 3,
  VT_STR = 4,
  VT_BYTES = 5,
  VT_ARRAY = 6,
  VT_MAP = 7,
  VT_FUNC = 8,
  VT_PTR = 9
} vt_type;

typedef struct vt_value {
  uint32_t type;  /* vt_type */
  uint32_t flags; /* réservé */
  union {
    int64_t i;
    double f;
    void* p;
  } as;
} vt_value;

/* Nom lisible d’un tag */
VT_OBJECT_API const char* vt_type_name(vt_type t);

/* ----------------------------------------------------------------------------
   Constructeurs scalaires
---------------------------------------------------------------------------- */
VT_OBJECT_API vt_value vt_nil(void);
VT_OBJECT_API vt_value vt_bool(int b);
VT_OBJECT_API vt_value vt_int(int64_t x);
VT_OBJECT_API vt_value vt_float(double x);

/* Pointeur brut (interop natif) */
VT_OBJECT_API vt_value vt_ptr(void* p);

/* ----------------------------------------------------------------------------
   Strings (UTF-8 agnostique, NUL-terminé, RC)
---------------------------------------------------------------------------- */
VT_OBJECT_API vt_value vt_string_from(const char* cstr);
VT_OBJECT_API vt_value vt_string_from_n(const char* s, size_t n);
VT_OBJECT_API const char* vt_string_cstr(
    const vt_value* v); /* NULL si non-string */
VT_OBJECT_API size_t vt_string_len(const vt_value* v);
VT_OBJECT_API int vt_string_append(vt_value* v, const char* s,
                                   size_t n);                       /* 1=ok */
VT_OBJECT_API int vt_string_concat(vt_value* a, const vt_value* b); /* 1=ok */

/* Retain/Release sur l’objet string sous-jacent */
VT_OBJECT_API void vt_str_retain(vt_str* s);
VT_OBJECT_API void vt_str_release(vt_str* s);

/* ----------------------------------------------------------------------------
   Bytes (octet string, RC)
---------------------------------------------------------------------------- */
VT_OBJECT_API vt_value vt_bytes_from(const void* data, size_t n);
VT_OBJECT_API size_t vt_bytes_len(const vt_value* v);
VT_OBJECT_API const unsigned char* vt_bytes_ptr(const vt_value* v);

VT_OBJECT_API void vt_bytes_retain(vt_bytes* b);
VT_OBJECT_API void vt_bytes_release(vt_bytes* b);

/* ----------------------------------------------------------------------------
   Array (tableau dynamique de vt_value, RC)
---------------------------------------------------------------------------- */
VT_OBJECT_API vt_value vt_array_new(void);
VT_OBJECT_API size_t vt_array_len(const vt_value* arr);
VT_OBJECT_API int vt_array_push(vt_value* arr, vt_value v); /* 1=ok */
VT_OBJECT_API int vt_array_get(const vt_value* arr, size_t idx,
                               vt_value* out); /* 1=ok */
VT_OBJECT_API int vt_array_set(vt_value* arr, size_t idx,
                               vt_value v); /* 1=ok */

VT_OBJECT_API void vt_array_retain(vt_array* a);
VT_OBJECT_API void vt_array_release(vt_array* a);

/* ----------------------------------------------------------------------------
   Map (clé string → vt_value, RC)
---------------------------------------------------------------------------- */
VT_OBJECT_API vt_value vt_map_new(void);
VT_OBJECT_API size_t vt_map_len(const vt_value* mapv);
VT_OBJECT_API int vt_map_set(vt_value* mapv, const char* key,
                             const vt_value* val); /* 1=ok */
VT_OBJECT_API int vt_map_get(const vt_value* mapv, const char* key,
                             vt_value* out);                         /* 1=ok */
VT_OBJECT_API int vt_map_has(const vt_value* mapv, const char* key); /* 1/0 */
VT_OBJECT_API int vt_map_del(vt_value* mapv, const char* key);       /* 1=ok */

VT_OBJECT_API void vt_map_retain(vt_map* m);
VT_OBJECT_API void vt_map_release(vt_map* m);

/* ----------------------------------------------------------------------------
   Cycle de vie des valeurs
---------------------------------------------------------------------------- */
VT_OBJECT_API vt_value vt_value_clone(const vt_value* v); /* RC++ si heap */
VT_OBJECT_API void vt_value_move(vt_value* dst,
                                 vt_value* src); /* move puis src ← nil */
VT_OBJECT_API void vt_value_release(
    vt_value* v); /* RC-- si heap, sinon no-op */

/* ----------------------------------------------------------------------------
   Hash / Égalité / Truthiness
---------------------------------------------------------------------------- */
VT_OBJECT_API uint64_t vt_value_hash(const vt_value* v);
VT_OBJECT_API int vt_value_equal(const vt_value* a,
                                 const vt_value* b); /* 1/0 */
VT_OBJECT_API int vt_truthy(
    const vt_value* v); /* règles de vérité dynamiques */

/* ----------------------------------------------------------------------------
   Stringify
   - Renvoie une chaîne C allouée via vt_mem (vt_free pour libérer).
---------------------------------------------------------------------------- */
VT_OBJECT_API char* vt_value_to_cstr(const vt_value* v);

/* ----------------------------------------------------------------------------
   Init/Shutdown globaux optionnels
---------------------------------------------------------------------------- */
VT_OBJECT_API void vt_object_runtime_init(void);
VT_OBJECT_API void vt_object_runtime_shutdown(void);

/* ----------------------------------------------------------------------------
   Notes d’utilisation
   ----------------------------------------------------------------------------
   - Toute valeur avec stockage heap (string/bytes/array/map/func) suit RC:
       vt_value_clone / vt_*_retain  → +1
       vt_value_release / vt_*_release → -1 et libération à 0.
   - vt_value_to_cstr retourne un buffer à libérer par vt_free().
   - Les fonctions *_get copient par valeur logique (clone interne).
---------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_OBJECT_H */
