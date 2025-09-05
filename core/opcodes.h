/* ============================================================================
   opcodes.h — Définition des opcodes Vitte/Vitl, métadonnées et API bytecode.
   - Enum opcodes et genres d’opérandes
   - Infos d’instruction (mnemonic, arité, effets pile, flags)
   - Encodeur/décodeur, désassembleur, vérif, calcul stack max
   - Builder bytecode (buffer dynamique)
   C17, licence MIT.
   ============================================================================
 */
#ifndef VT_OPCODES_H
#define VT_OPCODES_H
#pragma once

#define VT_OPCODES_H_SENTINEL 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   Export
--------------------------------------------------------------------------- */
#ifndef VT_OPCODES_API
#define VT_OPCODES_API extern
#endif

/* ---------------------------------------------------------------------------
   Genres d’opérandes immédiates
--------------------------------------------------------------------------- */
typedef enum vt_operand_kind {
  VT_OK_NONE = 0, /* aucun */
  VT_OK_U8,       /* uint8  (LE) */
  VT_OK_U16,      /* uint16 (LE) */
  VT_OK_U32,      /* uint32 (LE) */
  VT_OK_I32,      /* int32  (LE) */
  VT_OK_I64,      /* int64  (LE) */
  VT_OK_F64,      /* double (LE IEEE-754) */
  VT_OK_REL32,    /* saut relatif signé 32b, origine = fin d’insn */
  VT_OK_KIDX,     /* index pool de constantes (u16) */
  VT_OK_SIDX      /* index symbole/nom (u16) */
} vt_operand_kind;

/* ---------------------------------------------------------------------------
   Opcodes VM
--------------------------------------------------------------------------- */
typedef enum vt_opcode {
  OP_NOP = 0,
  OP_HALT,

  /* Constantes / charge */
  OP_ICONST, /* i64 */
  OP_FCONST, /* f64 */
  OP_SCONST, /* kidx string */
  OP_LOADK,  /* kidx any */

  /* Locals / globals */
  OP_LD,  /* u16 slot     */
  OP_ST,  /* u16 slot     */
  OP_LDG, /* u16 gslot    */
  OP_STG, /* u16 gslot    */

  /* Pile */
  OP_POP, /* u16 n        */
  OP_DUP,
  OP_SWAP,

  /* Arith / logique */
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_NEG,
  OP_NOT,

  /* Comparaisons */
  OP_EQ,
  OP_NE,
  OP_LT,
  OP_LE,
  OP_GT,
  OP_GE,

  /* Collections */
  OP_NEWA,  /* u16 count  -> Array */
  OP_APUSH, /* arr,val -> arr' */
  OP_AGET,  /* arr,idx -> val */
  OP_ASET,  /* arr,idx,val -> arr' */
  OP_NEWM,  /* u16 pairs -> Map  */
  OP_MGET,  /* map,key -> val */
  OP_MSET,  /* map,key,val -> map' */

  /* Contrôle */
  OP_JMP, /* rel32       */
  OP_JT,  /* rel32 si vrai  */
  OP_JF,  /* rel32 si faux  */

  /* Appels */
  OP_CALL, /* u8 nargs, u8 nrets */
  OP_RET,  /* u8 nrets */

  /* Fonctions/closures */
  OP_CLOSURE, /* kidx proto, u8 nup */
  OP_CAPTURE, /* u16 local */
  OP_TYPEOF,  /* any -> string */
  OP_CONCAT,  /* a,b -> a+b */

  /* Exceptions */
  OP_THROW,
  OP_TENTER, /* rel32 handler */
  OP_TLEAVE,

  /* Debug */
  OP_PRINT,

  OP__COUNT
} vt_opcode;

/* ---------------------------------------------------------------------------
   Flags d’opcode
--------------------------------------------------------------------------- */
enum {
  VT_OF_BRANCH = 1u << 0,  /* possède une cible de branchement      */
  VT_OF_COND = 1u << 1,    /* branchement conditionnel              */
  VT_OF_CALL = 1u << 2,    /* effectue un appel                     */
  VT_OF_RET = 1u << 3,     /* effectue un retour                    */
  VT_OF_TERM = 1u << 4,    /* terminaisons possibles (halt/throw…)  */
  VT_OF_MAYTHROW = 1u << 5 /* peut lever (div/mod, call, …)         */
};

/* ---------------------------------------------------------------------------
   Métadonnées d’instruction
   - argc: nombre d’opérandes immédiates
   - argk: genres des opérandes (jusqu’à 3)
   - stack_in/stack_out: effets pile min (delta = out - in). Négatif = variable.
--------------------------------------------------------------------------- */
typedef struct vt_opcode_info {
  const char* name;
  uint8_t argc;
  vt_operand_kind argk[3];
  int8_t stack_in;
  int8_t stack_out;
  uint32_t flags;
} vt_opcode_info;

/* ---------------------------------------------------------------------------
   Représentation décodée d’une instruction
   - imm : stockage brut des immédiats (selon argk)
   - size: taille encodée en octets
--------------------------------------------------------------------------- */
typedef struct vt_insn {
  vt_opcode op;
  uint64_t imm[3];
  size_t size;
} vt_insn;

/* ---------------------------------------------------------------------------
   Buffer de génération de bytecode
--------------------------------------------------------------------------- */
typedef struct vt_bcode {
  uint8_t* data;
  size_t len;
  size_t cap;
} vt_bcode;

/* ---------------------------------------------------------------------------
   API
--------------------------------------------------------------------------- */
/* Infos et décodage */
VT_OPCODES_API const vt_opcode_info* vt_op_info(vt_opcode op);
VT_OPCODES_API size_t vt_decode(const uint8_t* buf, size_t buflen,
                                vt_insn* out);

/* Désassemblage d’une seule ligne.
   Retourne la taille de l’instruction lue, 0 si erreur. */
VT_OPCODES_API int vt_disasm_line(const uint8_t* base, size_t off, size_t len,
                                  char* out, size_t outsz);

/* Vérification structurelle (tailles, cibles, alignements). 1=ok, 0=err. */
VT_OPCODES_API int vt_verify(const uint8_t* code, size_t len, char* err,
                             size_t errsz);

/* Approximation conservatrice du pic de pile. 1=ok. */
VT_OPCODES_API int vt_calc_stack_max(const uint8_t* code, size_t len,
                                     int* out_max);

/* Cible d’un branchement à off. 1=ok, 0=pas branchement/erreur. */
VT_OPCODES_API int vt_branch_target(const uint8_t* base, size_t off, size_t len,
                                    size_t* out_target);

/* Builder */
VT_OPCODES_API void vt_bcode_init(vt_bcode* bc);
VT_OPCODES_API void vt_bcode_free(vt_bcode* bc);

VT_OPCODES_API int vt_emit_u8(vt_bcode* bc, uint8_t v);
VT_OPCODES_API int vt_emit_u16(vt_bcode* bc, uint16_t v);
VT_OPCODES_API int vt_emit_u32(vt_bcode* bc, uint32_t v);
VT_OPCODES_API int vt_emit_i32(vt_bcode* bc, int32_t v);
VT_OPCODES_API int vt_emit_i64(vt_bcode* bc, int64_t v);
VT_OPCODES_API int vt_emit_f64(vt_bcode* bc, double v);

/* Émission d’une instruction complète selon vt_op_info(op).
   imm peut être NULL si aucun opérande. Retourne nb d’octets écrits ou 0. */
VT_OPCODES_API int vt_emit_insn(vt_bcode* bc, vt_opcode op,
                                const uint64_t* imm);

/* Patch rel32 du premier opérande relatif d’une insn à from_off. 1=ok. */
VT_OPCODES_API int vt_patch_rel32(uint8_t* code, size_t from_off, int32_t rel);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_OPCODES_H */
