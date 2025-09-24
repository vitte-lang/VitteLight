// SPDX-License-Identifier: GPL-3.0-or-later
// core/types.h — Type system de base pour VitteLight

#ifndef VL_TYPES_H
#define VL_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

/* Visibilité neutre si non fournie par l’API publique */
#ifndef VL_API
#define VL_API
#endif

/* Tags de type (doivent rester stables pour le VM et l’ABI) */
typedef enum {
    VT_T_NIL = 0,
    VT_T_BOOL,
    VT_T_INT,
    VT_T_FLOAT,
    VT_T_STRING,
    VT_T_TABLE,
    VT_T_FUNC,
    VT_T_NATIVE,
    VT_T_USERDATA,
    VT_T_MAX
} VL_TypeTag;

/* Valeur typée (union taggée) */
typedef struct {
    VL_TypeTag t;
    union {
        int64_t i;
        double  f;
        void*   p;
    } v;
} VL_Value;

/* ───── Constructeurs ───── */
VL_API VL_Value vl_make_nil(void);
VL_API VL_Value vl_make_bool(int b);
VL_API VL_Value vl_make_int(int64_t i);
VL_API VL_Value vl_make_float(double f);
/* Crée une valeur de type "t" référant un pointeur brut "p" */
VL_API VL_Value vl_make_ptr_tag(VL_TypeTag t, void* p);
/* Duplique une C-string (malloc) et la stocke en VT_T_STRING */
VL_API VL_Value vl_make_cstring(const char* s);

/* ───── Destruction / copie ───── */
/* Libère la mémoire détenue par la valeur (ex: STRING), puis met NIL */
VL_API void     vl_value_free(VL_Value* v);
/* Copie profonde (STRING dupliquée). 0=ok, -1=OOM/erreur */
VL_API int      vl_value_copy(VL_Value* dst, const VL_Value* src);

/* ───── Accesseurs / tests ───── */
VL_API int      vl_is_nil   (const VL_Value* v);
VL_API int      vl_is_bool  (const VL_Value* v);
VL_API int      vl_is_int   (const VL_Value* v);
VL_API int      vl_is_float (const VL_Value* v);
VL_API int      vl_is_string(const VL_Value* v);

VL_API int64_t  vl_as_int   (const VL_Value* v);
VL_API double   vl_as_float (const VL_Value* v);
VL_API void*    vl_as_ptr   (const VL_Value* v);

/* ───── Utilitaires ───── */
VL_API const char* vl_type_name(VL_TypeTag t);
/* Impression lisible de la valeur sur 'out' (stdout si out==NULL) */
VL_API void        vl_print_value(const VL_Value* v, FILE* out);
/* Comparaison faible: par valeur pour bool/int/float/string, sinon par adresse.
   Retourne <0, 0, >0 comme strcmp. */
VL_API int         vl_value_cmp(const VL_Value* a, const VL_Value* b);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VL_TYPES_H */
