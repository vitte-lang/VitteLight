// vitte-light/core/limits.h
// Paramètres de construction et bornes par défaut pour VitteLight.
// Unifie les tailles de piles, tables et bornes VLBC.
// Usage: inclure avant api.c / do.c pour centraliser les constantes.

#ifndef VITTE_LIGHT_CORE_LIMITS_H
#define VITTE_LIGHT_CORE_LIMITS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// ───────────────────── Versionnage ─────────────────────
#define VL_LIMITS_VERSION 1

// ───────────────────── Bornes VLBC (bytecode) ─────────────────────
#ifndef VLBC_MAGIC
#define VLBC_MAGIC "VLBC"  // 4 octets
#endif
#ifndef VLBC_VERSION
#define VLBC_VERSION 1u  // version du format
#endif

// Tailles maximales logiques (bornes « douces »; exécution refusera au-delà)
#ifndef VLBC_MAX_STRINGS  // nombre d'entrées dans le pool de constantes
                          // (strings)
#define VLBC_MAX_STRINGS (1u << 20)  // 1,048,576
#endif
#ifndef VLBC_MAX_CODE_BYTES
#define VLBC_MAX_CODE_BYTES (64u * 1024u * 1024u)  // 64 MiB
#endif

// ───────────────────── Capacités runtime par défaut ─────────────────────
#ifndef VL_STACK_CAP_DEFAULT
#define VL_STACK_CAP_DEFAULT 4096u
#endif
#ifndef VL_GLOBALS_CAP_DEFAULT
#define VL_GLOBALS_CAP_DEFAULT 512u
#endif
#ifndef VL_NATIVES_CAP_DEFAULT
#define VL_NATIVES_CAP_DEFAULT 512u
#endif
#ifndef VL_MAP_LOAD_PCT      // facteur de charge (×100) pour les maps ouvertes
#define VL_MAP_LOAD_PCT 70u  // 0<value<100
#endif
#ifndef VL_STR_MAX_BYTES  // longueur maximale d’une VL_String (hors NUL)
#define VL_STR_MAX_BYTES (16u * 1024u * 1024u)  // 16 MiB
#endif

// ───────────────────── Garde-fous compile-time ─────────────────────
#if VL_STACK_CAP_DEFAULT < 16
#error "VL_STACK_CAP_DEFAULT trop petit"
#endif
#if VL_MAP_LOAD_PCT <= 10 || VL_MAP_LOAD_PCT >= 95
#error "VL_MAP_LOAD_PCT hors intervalle raisonnable"
#endif
#if VL_STR_MAX_BYTES < 1024
#error "VL_STR_MAX_BYTES trop petit"
#endif

// ───────────────────── Variables d’environnement (overrides)
// ─────────────────────
#define VL_ENV_STACK_CAP "VL_STACK_CAP"
#define VL_ENV_GLOBALS_CAP "VL_GLOBALS_CAP"
#define VL_ENV_NATIVES_CAP "VL_NATIVES_CAP"
#define VL_ENV_STR_MAX "VL_STR_MAX"
#define VL_ENV_BC_MAX "VL_BC_MAX"

// ───────────────────── Résolution des limites effectives ─────────────────────
// Fournit une structure compacte et des helpers inline pour lire les overrides.

typedef struct VL_Limits {
  size_t stack_cap;
  size_t globals_cap;
  size_t natives_cap;
  size_t str_max_bytes;
  size_t bc_max_bytes;
  uint32_t bc_max_strings;
} VL_Limits;

// Helpers inline. Sans I/O, juste logique. Version avec getenv ci-dessous
// (optionnelle).
static inline struct VL_Limits vl_limits_default(void) {
  struct VL_Limits L;
  L.stack_cap = (size_t)VL_STACK_CAP_DEFAULT;
  L.globals_cap = (size_t)VL_GLOBALS_CAP_DEFAULT;
  L.natives_cap = (size_t)VL_NATIVES_CAP_DEFAULT;
  L.str_max_bytes = (size_t)VL_STR_MAX_BYTES;
  L.bc_max_bytes = (size_t)VLBC_MAX_CODE_BYTES;
  L.bc_max_strings = (uint32_t)VLBC_MAX_STRINGS;
  return L;
}

// Optionnel: lecture d’overrides via getenv. Nécessite <stdlib.h> ;
// sûre dans un header si utilisée en inline.
#include <stdlib.h>
static inline size_t vl__clamp_zu(size_t v, size_t lo, size_t hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
static inline size_t vl__env_zu(const char *name, size_t def, size_t lo,
                                size_t hi) {
  const char *s = getenv(name);
  if (!s || !*s) return def;
  char *end = NULL;
  unsigned long long x = strtoull(s, &end, 0);
  if (end == s) return def;
  return vl__clamp_zu((size_t)x, lo, hi);
}
static inline uint32_t vl__env_u32(const char *name, uint32_t def, uint32_t lo,
                                   uint32_t hi) {
  const char *s = getenv(name);
  if (!s || !*s) return def;
  char *end = NULL;
  unsigned long long x = strtoull(s, &end, 0);
  if (end == s) return def;
  if (x < lo) x = lo;
  if (x > hi) x = hi;
  return (uint32_t)x;
}

static inline struct VL_Limits vl_limits_from_env(void) {
  struct VL_Limits L = vl_limits_default();
  L.stack_cap = vl__env_zu(VL_ENV_STACK_CAP, L.stack_cap, 16,
                           (size_t)1 << 26);  // jusqu’à ~67M entries
  L.globals_cap =
      vl__env_zu(VL_ENV_GLOBALS_CAP, L.globals_cap, 16, (size_t)1 << 24);
  L.natives_cap =
      vl__env_zu(VL_ENV_NATIVES_CAP, L.natives_cap, 16, (size_t)1 << 24);
  L.str_max_bytes = vl__env_zu(VL_ENV_STR_MAX, L.str_max_bytes, 64,
                               (size_t)1 << 30);  // 1 GiB max
  L.bc_max_bytes =
      vl__env_zu(VL_ENV_BC_MAX, L.bc_max_bytes, 1024, (size_t)1 << 31);
  // bc_max_strings ne passe pas par env par défaut; changez ci-dessous si utile
  return L;
}

// ───────────────────── Utilitaires divers ─────────────────────
static inline int vl_is_pow2_zu(size_t x) { return x && ((x & (x - 1)) == 0); }
static inline size_t vl_round_up_pow2_zu(size_t x) {
  if (x <= 1) return 1;
  x--;
  for (unsigned k = 1; k < sizeof(size_t) * 8; k <<= 1) x |= x >> k;
  return x + 1;
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_LIMITS_H
