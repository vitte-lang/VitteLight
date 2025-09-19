// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/opnames.h — Noms et métadonnées d’opcodes VitteLight
//
// Fournit :
//   - vl_op_name(op) → "ADD" / "UNKNOWN"
//   - Table globale vl_opnames[VL_MAX_OPCODE]
//   - (optionnel, si VL_OPMETA_ENABLE) : arité et catégorie par opcode
//
// Usage :
//   #include "opnames.h"
//   printf("%s\n", vl_op_name(bytecode[i]));
//
// Build : placer ce header avec opnames.c dans core/.

#ifndef VITTE_LIGHT_CORE_OPNAMES_H
#define VITTE_LIGHT_CORE_OPNAMES_H

#include <stdint.h>
#include <stddef.h>

#ifndef VL_MAX_OPCODE
#  define VL_MAX_OPCODE 256
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────── Noms d’opcodes ───────────── */

/* Table indexée par code opcode. Entrées non utilisées = NULL. */
extern const char *const vl_opnames[VL_MAX_OPCODE];

/* Retourne un nom lisible ou "UNKNOWN" si op inconnu. */
const char *vl_op_name(uint8_t op);

/* ───────────── Métadonnées (optionnel) ─────────────
 * Activez en définissant VL_OPMETA_ENABLE AVANT l’inclusion de ce header.
 * Donne l’arité (nb d’opérandes encodés après l’opcode) et la catégorie.
 */
#ifdef VL_OPMETA_ENABLE

/* Catégories d’opcodes pour tri/affichage. */
typedef enum {
    VL_OPCAT_MISC = 0,   /* HALT, NOP, BREAK, etc. */
    VL_OPCAT_STACK,      /* PUSHS, PUSHI, POP, DUP, SWAP… */
    VL_OPCAT_ARITH,      /* ADD, SUB, MUL, DIV, MOD, NEG… */
    VL_OPCAT_CMP,        /* CMP, EQ, NE, LT, LE, GT, GE… */
    VL_OPCAT_LOGIC,      /* AND, OR, XOR, NOT… */
    VL_OPCAT_JUMP,       /* JUMP, JZ, JNZ, JLT… */
    VL_OPCAT_CALL,       /* CALL, CALLN, RET… */
    VL_OPCAT_TABLE,      /* NEWTABLE, GET/SETFIELD/INDEX… */
    VL_OPCAT_GLOBAL,     /* GET/SETGLOBAL, GET/SETLOCAL… */
    VL_OPCAT_VM,         /* TRACE, PRINT, DUMPSTACK… */
    VL_OPCAT__COUNT
} vl_opcat_t;

/* Arité : nombre d’octets d’opérandes immédiats consécutifs.
 * Exemple si JUMP rel32, arity peut valoir 4. */
extern const uint8_t vl_op_arity[VL_MAX_OPCODE];

/* Catégorie par opcode. */
extern const uint8_t vl_op_category[VL_MAX_OPCODE];

/* Vue structurée pratique. */
typedef struct {
    const char *name;   /* NULL si non défini */
    uint8_t     arity;  /* nb d’octets d’immédiats */
    uint8_t     cat;    /* vl_opcat_t */
} vl_opmeta_t;

extern const vl_opmeta_t vl_opmeta[VL_MAX_OPCODE];

/* Helpers. */
static inline uint8_t vl_op_get_arity(uint8_t op) {
    return (op < VL_MAX_OPCODE) ? vl_op_arity[op] : 0u;
}
static inline uint8_t vl_op_get_category(uint8_t op) {
    return (op < VL_MAX_OPCODE) ? vl_op_category[op] : (uint8_t)VL_OPCAT_MISC;
}
const char *vl_op_category_name(uint8_t cat); /* "STACK", "JUMP", etc. */

#endif /* VL_OPMETA_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* VITTE_LIGHT_CORE_OPNAMES_H */