// vitte-light/core/string.h
// API chaînes pour VitteLight: concat, slice, case, trim, search,
// conversions, repeat, escape. Basé sur VL_String / VL_Value.
// Implémentation: core/string.c

#ifndef VITTE_LIGHT_CORE_STRING_H
#define VITTE_LIGHT_CORE_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32)
#if !defined(ssize_t)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#else
#include <sys/types.h>
#endif

#include "api.h"     // struct VL_Context, VL_Value
#include "object.h"  // VL_String

// ───────────────────── Hash (FNV‑1a 32‑bit) ─────────────────────
uint32_t vl_str_hash_bytes(const void *p, size_t n);

// ───────────────────── Concat / slice ─────────────────────
VL_Value vl_str_concat(struct VL_Context *ctx, VL_Value a, VL_Value b);
VL_Value vl_str_slice_v(struct VL_Context *ctx, VL_Value s, size_t pos,
                        size_t len);

// ───────────────────── Transformations de casse (ASCII only)
// ─────────────────────
VL_Value vl_str_lower_v(struct VL_Context *ctx, VL_Value s);
VL_Value vl_str_upper_v(struct VL_Context *ctx, VL_Value s);

// ───────────────────── Trim (ASCII whitespace) ─────────────────────
VL_Value vl_str_trim_v(struct VL_Context *ctx, VL_Value s);

// ───────────────────── Conversions numériques ─────────────────────
int vl_str_to_int64(const VL_String *s, int64_t *out);
int vl_str_to_double(const VL_String *s, double *out);

// ───────────────────── Recherche / préfixe / suffixe ─────────────────────
ssize_t vl_str_find_cstr(const VL_String *s, const char *needle);
int vl_str_starts_with_cstr(const VL_String *s, const char *prefix);
int vl_str_ends_with_cstr(const VL_String *s, const char *suffix);

// ───────────────────── Répétition ─────────────────────
VL_Value vl_str_repeat_v(struct VL_Context *ctx, const VL_String *s,
                         size_t times);

// ───────────────────── Échappement imprimable ─────────────────────
// Retourne le nombre d'octets écrits. Enveloppe la sortie par des quotes (").
size_t vl_str_write_escaped(const VL_String *s, FILE *out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_STRING_H
