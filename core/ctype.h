// vitte-light/core/ctype.h
// En-tête public pour les utilitaires de type et conversions VitteLight
// Implémentation: core/ctype.c (fonctions de base)

#ifndef VITTE_LIGHT_CORE_CTYPE_H
#define VITTE_LIGHT_CORE_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "api.h"

// ───────────────────── Noms de types ─────────────────────
const char *vl_type_name(VL_Type t);

// ───────────────────── Pretty-print & sérialisation ─────────────────────
void vl_value_print(const VL_Value *v, FILE *out);
size_t vl_value_to_cstr(const VL_Value *v, char *buf, size_t n);

// ───────────────────── Conversions scalaires ─────────────────────
bool vl_value_truthy(const VL_Value *v);
bool vl_value_as_int(const VL_Value *v, int64_t *out);
bool vl_value_as_float(const VL_Value *v, double *out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_CTYPE_H
