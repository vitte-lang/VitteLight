// vitte-light/core/object.h
// API objets runtime: chaînes, tableaux, maps (hash) pour VitteLight.
// Implémentation: core/object.c

#ifndef VITTE_LIGHT_CORE_OBJECT_H
#define VITTE_LIGHT_CORE_OBJECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "api.h"    // VL_Context, VL_Value, VL_Status
#include "ctype.h"  // helpers VL_Value si besoin

// ───────────────────── Types (synchronisés avec api.c) ─────────────────────
// Exposés ici pour usage des hôtes. Conserver strictement ces définitions.
#ifndef VL_INTERNAL_TYPES_DECLARED
#define VL_INTERNAL_TYPES_DECLARED 1

typedef struct VL_String {  // chaîne courte compacte
  uint32_t hash;            // FNV-1a 32-bit
  uint32_t len;             // octets utiles (hors NUL)
  char data[];              // UTF-8 arbitraire + NUL
} VL_String;

typedef struct VL_Array {  // tableau dynamique de VL_Value
  VL_Value *data;
  size_t len;
  size_t cap;
} VL_Array;

typedef struct VL_Map {  // table ouverte (clés = VL_String*)
  VL_String **keys;
  VL_Value *vals;
  size_t cap;
  size_t len;
  size_t tomb;  // tombstones
} VL_Map;

#endif  // VL_INTERNAL_TYPES_DECLARED

// ───────────────────── Chaînes ─────────────────────
// Fabriques VL_Value (VT_STR). Renvoient nil en cas d'échec.
VL_Value vl_make_strn(struct VL_Context *ctx, const char *s, size_t n);
VL_Value vl_make_str(struct VL_Context *ctx, const char *cstr);
int vl_string_eq(const VL_String *a, const VL_String *b);

// ───────────────────── Tableaux ─────────────────────
void vl_array_init(VL_Array *ar);
void vl_array_clear(VL_Array *ar);
int vl_array_push(VL_Array *ar, VL_Value v);
int vl_array_pop(VL_Array *ar, VL_Value *out);
int vl_array_get(VL_Array *ar, size_t i, VL_Value *out);
int vl_array_set(VL_Array *ar, size_t i, VL_Value v);
size_t vl_array_len(const VL_Array *ar);

// ───────────────────── Maps (hash) ─────────────────────
void vl_map_init(VL_Map *m, size_t initial_cap);
void vl_map_clear(VL_Map *m);
int vl_map_put(VL_Map *m, VL_String *key, VL_Value val);
int vl_map_get(const VL_Map *m, const VL_String *key, VL_Value *out);
int vl_map_del(VL_Map *m, const VL_String *key);
// Itération: retourne l'index occupé suivant ou -1 si terminé.
ssize_t vl_map_next(const VL_Map *m, ssize_t i);

// Helpers C-string
int vl_map_put_cstr(VL_Map *m, struct VL_Context *ctx, const char *key,
                    VL_Value v);
int vl_map_get_cstr(const VL_Map *m, struct VL_Context *ctx, const char *key,
                    VL_Value *out);
int vl_map_del_cstr(VL_Map *m, struct VL_Context *ctx, const char *key);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_OBJECT_H
