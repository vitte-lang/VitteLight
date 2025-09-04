// vitte-light/core/opnames.h
// Noms symboliques et helpers statiques pour le set d'opcodes VLBC.
// Header-only. S'aligne sur core/opcodes.h et la VM (api.c / jumptab.c).

#ifndef VITTE_LIGHT_CORE_OPNAMES_H
#define VITTE_LIGHT_CORE_OPNAMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

#include "opcodes.h"  // définit OP_* si pas déjà défini

// Taille utile du tableau (dernier opcode inclus)
#ifndef VL_OP_MAX
#define VL_OP_MAX OP_HALT
#endif

// Nom court d'un opcode. Retourne "?" si inconnu.
static inline const char *vl_opname(uint8_t op) {
  // Tableau indexé par code opcode.
  static const char *const NAMES[VL_OP_MAX + 1] = {
      [OP_NOP] = "NOP",     [OP_PUSHI] = "PUSHI",   [OP_PUSHF] = "PUSHF",
      [OP_PUSHS] = "PUSHS", [OP_ADD] = "ADD",       [OP_SUB] = "SUB",
      [OP_MUL] = "MUL",     [OP_DIV] = "DIV",       [OP_EQ] = "EQ",
      [OP_NEQ] = "NEQ",     [OP_LT] = "LT",         [OP_GT] = "GT",
      [OP_LE] = "LE",       [OP_GE] = "GE",         [OP_PRINT] = "PRINT",
      [OP_POP] = "POP",     [OP_STOREG] = "STOREG", [OP_LOADG] = "LOADG",
      [OP_CALLN] = "CALLN", [OP_HALT] = "HALT",
  };
  return (op <= VL_OP_MAX && NAMES[op]) ? NAMES[op] : "?";
}

// Lookup par nom (insensible à la casse optionnelle). -1 si inconnu.
static inline int vl_opcode_from_name(const char *name) {
  if (!name || !*name) return -1;
  // Comparaison sensible à la casse d'abord
  if (!strcmp(name, "NOP")) return OP_NOP;
  if (!strcmp(name, "PUSHI")) return OP_PUSHI;
  if (!strcmp(name, "PUSHF")) return OP_PUSHF;
  if (!strcmp(name, "PUSHS")) return OP_PUSHS;
  if (!strcmp(name, "ADD")) return OP_ADD;
  if (!strcmp(name, "SUB")) return OP_SUB;
  if (!strcmp(name, "MUL")) return OP_MUL;
  if (!strcmp(name, "DIV")) return OP_DIV;
  if (!strcmp(name, "EQ")) return OP_EQ;
  if (!strcmp(name, "NEQ")) return OP_NEQ;
  if (!strcmp(name, "LT")) return OP_LT;
  if (!strcmp(name, "GT")) return OP_GT;
  if (!strcmp(name, "LE")) return OP_LE;
  if (!strcmp(name, "GE")) return OP_GE;
  if (!strcmp(name, "PRINT")) return OP_PRINT;
  if (!strcmp(name, "POP")) return OP_POP;
  if (!strcmp(name, "STOREG")) return OP_STOREG;
  if (!strcmp(name, "LOADG")) return OP_LOADG;
  if (!strcmp(name, "CALLN")) return OP_CALLN;
  if (!strcmp(name, "HALT")) return OP_HALT;
// Variante insensible à la casse
#define EQCI(a, b) (strcasecmp((a), (b)) == 0)
  if (EQCI(name, "nop")) return OP_NOP;
  if (EQCI(name, "pushi")) return OP_PUSHI;
  if (EQCI(name, "pushf")) return OP_PUSHF;
  if (EQCI(name, "pushs")) return OP_PUSHS;
  if (EQCI(name, "add")) return OP_ADD;
  if (EQCI(name, "sub")) return OP_SUB;
  if (EQCI(name, "mul")) return OP_MUL;
  if (EQCI(name, "div")) return OP_DIV;
  if (EQCI(name, "eq")) return OP_EQ;
  if (EQCI(name, "neq")) return OP_NEQ;
  if (EQCI(name, "lt")) return OP_LT;
  if (EQCI(name, "gt")) return OP_GT;
  if (EQCI(name, "le")) return OP_LE;
  if (EQCI(name, "ge")) return OP_GE;
  if (EQCI(name, "print")) return OP_PRINT;
  if (EQCI(name, "pop")) return OP_POP;
  if (EQCI(name, "storeg")) return OP_STOREG;
  if (EQCI(name, "loadg")) return OP_LOADG;
  if (EQCI(name, "calln")) return OP_CALLN;
  if (EQCI(name, "halt")) return OP_HALT;
#undef EQCI
  return -1;
}

// Helpers booléens utiles pour l'assembleur et le désassembleur.
static inline int vl_op_is_binary(uint8_t op) {
  switch (op) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_EQ:
    case OP_NEQ:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE:
      return 1;
    default:
      return 0;
  }
}
static inline int vl_op_has_u32_const(uint8_t op) {  // utilise un index kstr
  return (op == OP_PUSHS || op == OP_LOADG || op == OP_STOREG ||
          op == OP_CALLN);
}
static inline int vl_op_is_pure(uint8_t op) {  // grossière approximation
  switch (op) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_EQ:
    case OP_NEQ:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE:
      return 1;
    default:
      return 0;
  }
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_OPNAMES_H
