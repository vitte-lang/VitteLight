// vitte-light/core/debug.h
// API publique des outils de debug VitteLight
// Implémentation: core/debug.c

#ifndef VITTE_LIGHT_CORE_DEBUG_H
#define VITTE_LIGHT_CORE_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "api.h"
#include "ctype.h"

// ───────────────────── Hexdump ─────────────────────
void vl_debug_hexdump(const void *data, size_t len, FILE *out);

// ───────────────────── Inspecteur VLBC ─────────────────────
// Affiche l’entête, pool de chaînes et taille code.
bool vl_debug_vlbc_inspect(const uint8_t *buf, size_t n, FILE *out);

// ───────────────────── Désassembleur minimal ─────────────────────
bool vl_debug_disassemble(const uint8_t *buf, size_t n, FILE *out);

// ───────────────────── Dump VM ─────────────────────
void vl_debug_dump_stack(struct VL_Context *ctx, FILE *out);
void vl_debug_dump_globals(struct VL_Context *ctx, FILE *out);

// ───────────────────── Trace exécution ─────────────────────
// Exécute en traçant instruction par instruction. Arrêt sur HALT ou erreur.
VL_Status vl_debug_run_trace(struct VL_Context *ctx, uint64_t max_steps,
                             FILE *out);

// ───────────────────── Assertions ─────────────────────
int vl_debug_expect_true(int cond, const char *expr, const char *file,
                         int line);
#define VL_EXPECT(x)                                        \
  do {                                                      \
    if (!vl_debug_expect_true((x), #x, __FILE__, __LINE__)) \
      return VL_ERR_RUNTIME;                                \
  } while (0)

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_DEBUG_H
