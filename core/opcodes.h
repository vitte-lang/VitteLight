// vitte-light/core/opcodes.h
// En-tête métadonnées/émission/désassemblage d'opcodes VLBC.
// Implémentation: core/opcodes.c

#ifndef VITTE_LIGHT_CORE_OPCODES_H
#define VITTE_LIGHT_CORE_OPCODES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t, uint64_t
#include <stdio.h>   // FILE*

#include "api.h"  // VL_Status

// ───────────────────── Jeu d'opcodes (doit matcher api.c/jumptab.c)
// ─────────────────────
#ifndef OP_NOP
enum {
  OP_NOP = 0,
  OP_PUSHI = 1,
  OP_PUSHF = 2,
  OP_PUSHS = 3,
  OP_ADD = 4,
  OP_SUB = 5,
  OP_MUL = 6,
  OP_DIV = 7,
  OP_EQ = 8,
  OP_NEQ = 9,
  OP_LT = 10,
  OP_GT = 11,
  OP_LE = 12,
  OP_GE = 13,
  OP_PRINT = 14,
  OP_POP = 15,
  OP_STOREG = 16,
  OP_LOADG = 17,
  OP_CALLN = 18,
  OP_HALT = 19
};
#endif

// ───────────────────── Introspection opcodes ─────────────────────
// Nom à partir du code (ou "?")
const char *vl_op_name(uint8_t op);
// Code à partir du nom (ou -1 si inconnu)
int vl_op_from_name(const char *name);
// Taille binaire d'une instruction (opcode inclus)
size_t vl_op_insn_size(uint8_t op);
// Taille à un offset dans un buffer de code, 0 si out-of-bounds
size_t vl_insn_size_at(const uint8_t *code, size_t code_len, size_t ip);
// Validation structurelle du flux de code et bornes d'indices de constantes
VL_Status vl_validate_code(const uint8_t *code, size_t code_len,
                           size_t kstr_len);

// ───────────────────── Désassemblage ─────────────────────
// Désassemble une instruction dans buf (NUL-terminé). Retourne octets écrits.
size_t vl_disasm_one(const uint8_t *code, size_t code_len, size_t ip, char *buf,
                     size_t n);
// Désassemble tout le programme vers out
void vl_disasm_program(const uint8_t *code, size_t code_len, FILE *out);

// ───────────────────── Émission d'instructions ─────────────────────
void vl_emit_NOP(uint8_t **pp);
void vl_emit_PUSHI(uint8_t **pp, int64_t v);
void vl_emit_PUSHF(uint8_t **pp, double d);
void vl_emit_PUSHS(uint8_t **pp, uint32_t si);
void vl_emit_ADD(uint8_t **pp);
void vl_emit_SUB(uint8_t **pp);
void vl_emit_MUL(uint8_t **pp);
void vl_emit_DIV(uint8_t **pp);
void vl_emit_EQ(uint8_t **pp);
void vl_emit_NEQ(uint8_t **pp);
void vl_emit_LT(uint8_t **pp);
void vl_emit_GT(uint8_t **pp);
void vl_emit_LE(uint8_t **pp);
void vl_emit_GE(uint8_t **pp);
void vl_emit_PRINT(uint8_t **pp);
void vl_emit_POP(uint8_t **pp);
void vl_emit_STOREG(uint8_t **pp, uint32_t si);
void vl_emit_LOADG(uint8_t **pp, uint32_t si);
void vl_emit_CALLN(uint8_t **pp, uint32_t name_si, uint8_t argc);
void vl_emit_HALT(uint8_t **pp);

// ───────────────────── Conteneur VLBC (header, pool, code)
// ─────────────────────
#ifndef VLBC_MAGIC
#define VLBC_MAGIC "VLBC"
#endif
#ifndef VLBC_VERSION
#define VLBC_VERSION 1u
#endif

// Entête: magic(4) + version(1)
void vl_bc_emit_header(uint8_t **pp, uint8_t version);
// Pool de chaînes: count(u32) puis (len(u32), bytes)*count
void vl_bc_emit_kstr(uint8_t **pp, const char *const *kstr, uint32_t count);
// Bloc code: begin -> pointeur size_slot, end -> backfill du size
uint8_t *vl_bc_begin_code(uint8_t **pp);
void vl_bc_end_code(uint8_t *size_slot, const uint8_t *code_begin,
                    const uint8_t *code_end);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_OPCODES_H
