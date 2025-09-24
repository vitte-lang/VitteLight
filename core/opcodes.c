/* ============================================================================
   opcodes.c — Table des opcodes, encodeur/décodeur, désassembleur, vérif.
   Cible: machine virtuelle Vitte/Vitl (format bytecode LE).
   - Opcodes, métadonnées (mnemonic, opérandes, effets de pile, flags)
   - Encodeurs émissifs (builder bytecode)
   - Décodeur/iterate, patch de sauts relatifs
   - Désassembleur mono-ligne
   - Vérif structurelle (bornes, cibles de branchement)
   - Calcul stack max par effet local (approx conservatrice)
   Licence: MIT.
   ============================================================================
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef VT_OPCODES_API
#define VT_OPCODES_API /* extern par défaut dans le .h ; local ici */
#endif

/* ----------------------------------------------------------------------------
   Journalisation optionnelle
---------------------------------------------------------------------------- */
#ifdef HAVE_VT_DEBUG_H
#include "debug.h"
#else
#define VT_TRACE(...) ((void)0)
#define VT_DEBUG(...) ((void)0)
#define VT_INFO(...) ((void)0)
#define VT_WARN(...) ((void)0)
#define VT_ERROR(...) ((void)0)
#define VT_FATAL(...) ((void)0)
#endif

/* ----------------------------------------------------------------------------
   Déclaration publique minimale (si opcodes.h absent)
   Remplacez par #include "opcodes.h" si vous avez le header dédié.
---------------------------------------------------------------------------- */
#ifndef VT_OPCODES_H_SENTINEL
typedef enum vt_operand_kind {
  VT_OK_NONE = 0, /* aucun opérande */
  VT_OK_U8,       /* unsigned 8 */
  VT_OK_U16,      /* unsigned 16 LE */
  VT_OK_U32,      /* unsigned 32 LE */
  VT_OK_I32,      /* signed   32 LE */
  VT_OK_I64,      /* signed   64 LE */
  VT_OK_F64,      /* IEEE 754 64 LE */
  VT_OK_REL32,    /* saut relatif signé 32 vers début de l’instruction */
  VT_OK_KIDX,     /* index pool constantes (u16) */
  VT_OK_SIDX,     /* index symbole/nom (u16) */
} vt_operand_kind;

typedef enum vt_opcode {
  OP_NOP = 0,
  OP_HALT,

  /* Constantes / charge */
  OP_ICONST, /* i64 imm        */
  OP_FCONST, /* f64 imm        */
  OP_SCONST, /* kidx (string)  */
  OP_LOADK,  /* kidx any       */

  /* Locals / globals */
  OP_LD,  /* u16 slot       */
  OP_ST,  /* u16 slot       */
  OP_LDG, /* u16 gslot      */
  OP_STG, /* u16 gslot      */

  /* Pile */
  OP_POP,  /* u16 n          */
  OP_DUP,  /*                */
  OP_SWAP, /*                */

  /* Arith / logique (dyn) */
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_NEG,
  OP_NOT,

  /* Comparaisons (dyn) */
  OP_EQ,
  OP_NE,
  OP_LT,
  OP_LE,
  OP_GT,
  OP_GE,

  /* Tables */
  OP_NEWA,  /* u16 count -> Array */
  OP_APUSH, /* append 1 (value already on stack) */
  OP_AGET,  /* index */
  OP_ASET,  /* index, value */
  OP_NEWM,  /* u16 pairs  -> Map    */
  OP_MGET,  /* key */
  OP_MSET,  /* key, value */

  /* Contrôle */
  OP_JMP, /* rel32 */
  OP_JT,  /* rel32 if truthy */
  OP_JF,  /* rel32 if falsy  */

  /* Appels */
  OP_CALL, /* u8 nargs, u8 nrets */
  OP_RET,  /* u8 nrets */

  /* Fonctions/closures */
  OP_CLOSURE, /* kidx func proto, u8 nup */
  OP_CAPTURE, /* u16 local-slot */
  OP_TYPEOF,  /* -> string type name */
  OP_CONCAT,  /* a b -> a+b (string/bytes/array) */

  /* Exceptions */
  OP_THROW,  /* value -> (unwind) */
  OP_TENTER, /* rel32 handler */
  OP_TLEAVE, /* leave try */

  /* Debug util */
  OP_PRINT, /* consomme au sommet et écrit debug */

  OP__COUNT
} vt_opcode;

typedef struct vt_opcode_info {
  const char* name;
  uint8_t argc;            /* nb d’opérandes immédiates */
  vt_operand_kind argk[3]; /* kinds des opérandes        */
  int8_t stack_in;         /* conso pile minimale        */
  int8_t stack_out;        /* prod pile minimale         */
  uint32_t flags;          /* bits (branch, call, etc.)  */
} vt_opcode_info;

/* Flags */
enum {
  VT_OF_BRANCH = 1u << 0,   /* a une cible de branchement */
  VT_OF_COND = 1u << 1,     /* branchement conditionnel   */
  VT_OF_CALL = 1u << 2,     /* effectue un appel          */
  VT_OF_RET = 1u << 3,      /* effectue un retour         */
  VT_OF_TERM = 1u << 4,     /* terminaisons possibles     */
  VT_OF_MAYTHROW = 1u << 5, /* peut lever                 */
};

typedef struct vt_insn {
  vt_opcode op;
  uint64_t imm[3]; /* stockage brut, interprétation selon argk */
  size_t size;     /* taille en octets de l’instruction encodée */
} vt_insn;

/* Builder bytecode minimal */
typedef struct vt_bcode {
  uint8_t* data;
  size_t len;
  size_t cap;
} vt_bcode;

VT_OPCODES_API const vt_opcode_info* vt_op_info(vt_opcode op);
VT_OPCODES_API size_t vt_decode(const uint8_t* buf, size_t buflen,
                                vt_insn* out);
VT_OPCODES_API int vt_disasm_line(const uint8_t* base, size_t off, size_t len,
                                  char* out, size_t outsz);
VT_OPCODES_API int vt_verify(const uint8_t* code, size_t len, char* err,
                             size_t errsz);
VT_OPCODES_API int vt_calc_stack_max(const uint8_t* code, size_t len,
                                     int* out_max);

VT_OPCODES_API void vt_bcode_init(vt_bcode* bc);
VT_OPCODES_API void vt_bcode_free(vt_bcode* bc);
VT_OPCODES_API int vt_emit_u8(vt_bcode* bc, uint8_t v);
VT_OPCODES_API int vt_emit_u16(vt_bcode* bc, uint16_t v);
VT_OPCODES_API int vt_emit_u32(vt_bcode* bc, uint32_t v);
VT_OPCODES_API int vt_emit_i32(vt_bcode* bc, int32_t v);
VT_OPCODES_API int vt_emit_i64(vt_bcode* bc, int64_t v);
VT_OPCODES_API int vt_emit_f64(vt_bcode* bc, double v);
VT_OPCODES_API int vt_emit_insn(vt_bcode* bc, vt_opcode op,
                                const uint64_t* imm);
VT_OPCODES_API int vt_patch_rel32(uint8_t* code, size_t from_off, int32_t rel);

VT_OPCODES_API int vt_branch_target(const uint8_t* base, size_t off, size_t len,
                                    size_t* out_target);
#endif /* sentinel */

/* ----------------------------------------------------------------------------
   Helpers LE
---------------------------------------------------------------------------- */
static inline uint8_t rd_u8(const uint8_t* p) { return p[0]; }
static inline uint16_t rd_u16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd_u32(const uint8_t* p) {
  return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static inline int32_t rd_i32(const uint8_t* p) { return (int32_t)rd_u32(p); }
static inline uint64_t rd_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | p[i];
  }
  return v;
}
static inline double rd_f64(const uint8_t* p) {
  union {
    uint64_t u;
    double d;
  } u;
  u.u = rd_u64(p);
  return u.d;
}

static inline void wr_u8(uint8_t* p, uint8_t v) { p[0] = v; }
static inline void wr_u16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}
static inline void wr_u32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static inline void wr_i32(uint8_t* p, int32_t v) { wr_u32(p, (uint32_t)v); }
static inline void wr_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = (uint8_t)(v & 0xFF);
    v >>= 8;
  }
}
static inline void wr_f64(uint8_t* p, double d) {
  union {
    uint64_t u;
    double d;
  } u;
  u.d = d;
  wr_u64(p, u.u);
}

/* ----------------------------------------------------------------------------
   Table des opcodes
   Effets de pile: stack_out - stack_in = delta minimal.
   Les op dynamiques ne tiennent pas compte de promotions/erreurs.
---------------------------------------------------------------------------- */
static const vt_opcode_info G_OPS[OP__COUNT] = {
    /* name     argc argk[]                              in  out flags */
    [OP_NOP] = {"nop", 0, {VT_OK_NONE}, 0, 0, 0},
    [OP_HALT] = {"halt", 0, {VT_OK_NONE}, 0, 0, VT_OF_TERM},

    [OP_ICONST] = {"iconst", 1, {VT_OK_I64}, 0, 1, 0},
    [OP_FCONST] = {"fconst", 1, {VT_OK_F64}, 0, 1, 0},
    [OP_SCONST] = {"sconst", 1, {VT_OK_KIDX}, 0, 1, 0},
    [OP_LOADK] = {"loadk", 1, {VT_OK_KIDX}, 0, 1, 0},

    [OP_LD] = {"ld", 1, {VT_OK_U16}, 0, 1, 0},
    [OP_ST] = {"st", 1, {VT_OK_U16}, 1, 0, 0},
    [OP_LDG] = {"ldg", 1, {VT_OK_U16}, 0, 1, 0},
    [OP_STG] = {"stg", 1, {VT_OK_U16}, 1, 0, 0},

    [OP_POP] =
        {"pop", 1, {VT_OK_U16}, -1, 0, 0}, /* in = n (approx: -1 => variable) */
    [OP_DUP] = {"dup", 0, {VT_OK_NONE}, 1, 2, 0},
    [OP_SWAP] = {"swap", 0, {VT_OK_NONE}, 2, 2, 0},

    [OP_ADD] = {"add", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_SUB] = {"sub", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_MUL] = {"mul", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_DIV] = {"div", 0, {VT_OK_NONE}, 2, 1, VT_OF_MAYTHROW},
    [OP_MOD] = {"mod", 0, {VT_OK_NONE}, 2, 1, VT_OF_MAYTHROW},
    [OP_NEG] = {"neg", 0, {VT_OK_NONE}, 1, 1, 0},
    [OP_NOT] = {"not", 0, {VT_OK_NONE}, 1, 1, 0},

    [OP_EQ] = {"eq", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_NE] = {"ne", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_LT] = {"lt", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_LE] = {"le", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_GT] = {"gt", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_GE] = {"ge", 0, {VT_OK_NONE}, 2, 1, 0},

    [OP_NEWA] = {"newa", 1, {VT_OK_U16}, 0, 1, 0},
    [OP_APUSH] = {"apush", 0, {VT_OK_NONE}, 2, 1, 0}, /* arr val -> arr' */
    [OP_AGET] = {"aget", 0, {VT_OK_NONE}, 2, 1, VT_OF_MAYTHROW},
    [OP_ASET] = {"aset", 0, {VT_OK_NONE}, 3, 1, VT_OF_MAYTHROW},
    [OP_NEWM] = {"newm", 1, {VT_OK_U16}, 0, 1, 0},
    [OP_MGET] = {"mget", 0, {VT_OK_NONE}, 2, 1, 0},
    [OP_MSET] = {"mset", 0, {VT_OK_NONE}, 3, 1, 0},

    [OP_JMP] = {"jmp", 1, {VT_OK_REL32}, 0, 0, VT_OF_BRANCH},
    [OP_JT] = {"jt", 1, {VT_OK_REL32}, 1, 0, VT_OF_BRANCH | VT_OF_COND},
    [OP_JF] = {"jf", 1, {VT_OK_REL32}, 1, 0, VT_OF_BRANCH | VT_OF_COND},

    [OP_CALL] = {"call",
                 2,
                 {VT_OK_U8, VT_OK_U8},
                 -1,
                 -1,
                 VT_OF_CALL | VT_OF_MAYTHROW}, /* in/out variables */
    [OP_RET] = {"ret", 1, {VT_OK_U8}, 0, 0, VT_OF_RET | VT_OF_TERM},

    [OP_CLOSURE] = {"closure", 2, {VT_OK_KIDX, VT_OK_U8}, 0, 1, 0},
    [OP_CAPTURE] = {"capture", 1, {VT_OK_U16}, 1, 1, 0},
    [OP_TYPEOF] = {"typeof", 0, {VT_OK_NONE}, 1, 1, 0},
    [OP_CONCAT] = {"concat", 0, {VT_OK_NONE}, 2, 1, 0},

    [OP_THROW] = {"throw", 0, {VT_OK_NONE}, 1, 0, VT_OF_TERM | VT_OF_MAYTHROW},
    [OP_TENTER] = {"tenter", 1, {VT_OK_REL32}, 0, 0, 0},
    [OP_TLEAVE] = {"tleave", 0, {VT_OK_NONE}, 0, 0, 0},

    [OP_PRINT] = {"print", 0, {VT_OK_NONE}, 1, 0, 0},
};

/* Public getter */
const vt_opcode_info* vt_op_info(vt_opcode op) {
  if ((int)op < 0 || op >= OP__COUNT) return NULL;
  return &G_OPS[op];
}

/* ----------------------------------------------------------------------------
   Taille encodée d’une insn
---------------------------------------------------------------------------- */
static size_t op_size(vt_opcode op) {
  const vt_opcode_info* i = vt_op_info(op);
  if (!i) return 0;
  size_t sz = 1; /* opcode */
  for (uint8_t k = 0; k < i->argc; k++) {
    switch (i->argk[k]) {
      case VT_OK_U8:
        sz += 1;
        break;
      case VT_OK_U16:
        sz += 2;
        break;
      case VT_OK_U32:
        sz += 4;
        break;
      case VT_OK_I32:
        sz += 4;
        break;
      case VT_OK_I64:
        sz += 8;
        break;
      case VT_OK_F64:
        sz += 8;
        break;
      case VT_OK_REL32:
        sz += 4;
        break;
      case VT_OK_KIDX:
        sz += 2;
        break;
      case VT_OK_SIDX:
        sz += 2;
        break;
      default:
        break;
    }
  }
  return sz;
}

/* ----------------------------------------------------------------------------
   Décodage d’une instruction
---------------------------------------------------------------------------- */
size_t vt_decode(const uint8_t* buf, size_t buflen, vt_insn* out) {
  if (buflen == 0) return 0;
  vt_opcode op = (vt_opcode)buf[0];
  const vt_opcode_info* ii = vt_op_info(op);
  if (!ii) return 0;
  size_t need = op_size(op);
  if (need == 0 || need > buflen) return 0;
  if (out) {
    out->op = op;
    out->size = need;
    size_t off = 1;
    for (uint8_t k = 0; k < ii->argc; k++) {
      switch (ii->argk[k]) {
        case VT_OK_U8:
          out->imm[k] = rd_u8(buf + off);
          off += 1;
          break;
        case VT_OK_U16:
          out->imm[k] = rd_u16(buf + off);
          off += 2;
          break;
        case VT_OK_U32:
          out->imm[k] = rd_u32(buf + off);
          off += 4;
          break;
        case VT_OK_I32:
          out->imm[k] = (uint64_t)(int64_t)rd_i32(buf + off);
          off += 4;
          break;
        case VT_OK_I64:
          out->imm[k] = rd_u64(buf + off);
          off += 8;
          break;
        case VT_OK_F64: {
          double d = rd_f64(buf + off);
          memcpy(&out->imm[k], &d, sizeof(double));
          off += 8;
        } break;
        case VT_OK_REL32:
          out->imm[k] = (uint64_t)(int64_t)rd_i32(buf + off);
          off += 4;
          break;
        case VT_OK_KIDX:
          out->imm[k] = rd_u16(buf + off);
          off += 2;
          break;
        case VT_OK_SIDX:
          out->imm[k] = rd_u16(buf + off);
          off += 2;
          break;
        default:
          break;
      }
    }
  }
  return need;
}

/* ----------------------------------------------------------------------------
   Disassembly
---------------------------------------------------------------------------- */
static int catf(char* o, size_t osz, size_t* cur, const char* fmt, ...) {
  if (!o || osz == 0) return 0;
  va_list ap;
  va_start(ap, fmt);
#if defined(_MSC_VER)
  int n = _vsnprintf(o + *cur, (osz > *cur) ? (osz - *cur) : 0, fmt, ap);
#else
  int n = vsnprintf(o + *cur, (osz > *cur) ? (osz - *cur) : 0, fmt, ap);
#endif
  va_end(ap);
  if (n < 0) return 0;
  *cur += (size_t)n;
  if (*cur >= osz) {
    o[osz ? osz - 1 : 0] = 0;
    return 0;
  }
  return 1;
}

int vt_disasm_line(const uint8_t* base, size_t off, size_t len, char* out,
                   size_t outsz) {
  if (!out || outsz == 0) return 0;
  out[0] = 0;
  if (off >= len) return 0;
  vt_insn ins;
  size_t got = vt_decode(base + off, len - off, &ins);
  if (!got) return 0;
  const vt_opcode_info* ii = vt_op_info(ins.op);
  size_t cur = 0;
  catf(out, outsz, &cur, "%-6s", ii->name ? ii->name : "op?");
  for (uint8_t k = 0; k < ii->argc; k++) {
    if (k == 0)
      catf(out, outsz, &cur, " ");
    else
      catf(out, outsz, &cur, ", ");
    switch (ii->argk[k]) {
      case VT_OK_U8:
        catf(out, outsz, &cur, "%" PRIu64, ins.imm[k]);
        break;
      case VT_OK_U16:
        catf(out, outsz, &cur, "%" PRIu64, ins.imm[k]);
        break;
      case VT_OK_U32:
        catf(out, outsz, &cur, "%" PRIu64, ins.imm[k]);
        break;
      case VT_OK_I32:
        catf(out, outsz, &cur, "%" PRId64, (int64_t)ins.imm[k]);
        break;
      case VT_OK_I64:
        catf(out, outsz, &cur, "%" PRId64, (int64_t)ins.imm[k]);
        break;
      case VT_OK_F64: {
        double d;
        memcpy(&d, &ins.imm[k], sizeof(double));
        catf(out, outsz, &cur, "%g", d);
      } break;
      case VT_OK_REL32: {
        int32_t rel = (int32_t)ins.imm[k];
        size_t tgt = off + got + rel;
        catf(out, outsz, &cur, "%+d ; -> 0x%zx", rel, tgt);
      } break;
      case VT_OK_KIDX:
        catf(out, outsz, &cur, "k#%" PRIu64, ins.imm[k]);
        break;
      case VT_OK_SIDX:
        catf(out, outsz, &cur, "s#%" PRIu64, ins.imm[k]);
        break;
      default:
        break;
    }
  }
  return (int)got;
}

/* ----------------------------------------------------------------------------
   Vérification structurelle: opcodes connus, tailles, cibles de branchement
---------------------------------------------------------------------------- */
int vt_branch_target(const uint8_t* base, size_t off, size_t len,
                     size_t* out_target) {
  vt_insn ins;
  size_t got = vt_decode(base + off, len - off, &ins);
  if (!got) return 0;
  const vt_opcode_info* ii = vt_op_info(ins.op);
  if (!ii) return 0;
  if (!(ii->flags & VT_OF_BRANCH)) return 0;
  /* premier opérande rel32 par convention */
  int32_t rel = (int32_t)ins.imm[0];
  size_t tgt = off + got + (ptrdiff_t)rel;
  if (tgt > len) return 0;
  if (out_target) *out_target = tgt;
  return 1;
}

int vt_verify(const uint8_t* code, size_t len, char* err, size_t errsz) {
  size_t off = 0;
  while (off < len) {
    vt_insn ins;
    size_t got = vt_decode(code + off, len - off, &ins);
    if (!got) {
      if (err && errsz) snprintf(err, errsz, "decode error at 0x%zx", off);
      return 0;
    }
    const vt_opcode_info* ii = vt_op_info(ins.op);
    if (!ii) {
      if (err && errsz)
        snprintf(err, errsz, "bad opcode 0x%02x at 0x%zx", code[off], off);
      return 0;
    }
    if (ii->flags & VT_OF_BRANCH) {
      size_t tgt = 0;
      if (!vt_branch_target(code, off, len, &tgt)) {
        if (err && errsz) snprintf(err, errsz, "bad branch at 0x%zx", off);
        return 0;
      }
      /* cible doit tomber sur début d’instruction: vérif simple en rebalayant
       */
      size_t p = 0;
      int ok = 0;
      while (p < len) {
        vt_insn t;
        size_t g = vt_decode(code + p, len - p, &t);
        if (!g) break;
        if (p == tgt) {
          ok = 1;
          break;
        }
        p += g;
      }
      if (!ok) {
        if (err && errsz)
          snprintf(err, errsz, "branch target 0x%zx not aligned", tgt);
        return 0;
      }
    }
    off += got;
  }
  if (off != len) {
    if (err && errsz) snprintf(err, errsz, "trailing bytes");
    return 0;
  }
  return 1;
}

/* ----------------------------------------------------------------------------
   Calcul approximatif du max de pile via somme des deltas (borne inférieure).
   - Ignore les chemins conditionnels: prend max des chemins décodés
linéairement.
   - Suffisant pour une vérif grossière. Pour exact: dataflow (worklist).
---------------------------------------------------------------------------- */
int vt_calc_stack_max(const uint8_t* code, size_t len, int* out_max) {
  int cur = 0, mx = 0;
  size_t off = 0;
  while (off < len) {
    vt_insn ins;
    size_t got = vt_decode(code + off, len - off, &ins);
    if (!got) return 0;
    const vt_opcode_info* ii = vt_op_info(ins.op);
    if (!ii) return 0;

    int in = (ii->stack_in < 0) ? 0 : ii->stack_in;
    int out = (ii->stack_out < 0) ? 0 : ii->stack_out;
    cur -= in;
    if (cur < 0) cur = 0; /* pas d’underflow négatif pour l’estimation */
    cur += out;
    if (cur > mx) mx = cur;

    if (ii->flags & VT_OF_TERM) {
      /* borne sur chemin se termine; on n’explore pas les branches ici */
      cur = 0;
    }
    off += got;
  }
  if (out_max) *out_max = mx;
  return 1;
}

/* ----------------------------------------------------------------------------
   Builder
---------------------------------------------------------------------------- */
static int ensure(vt_bcode* bc, size_t add) {
  size_t need = bc->len + add;
  if (need <= bc->cap) return 1;
  size_t ncap = bc->cap ? bc->cap : 64;
  while (ncap < need) ncap = ncap + ncap / 2 + 32;
  void* p = (bc->data) ? realloc(bc->data, ncap) : malloc(ncap);
  if (!p) return 0;
  bc->data = (uint8_t*)p;
  bc->cap = ncap;
  return 1;
}
void vt_bcode_init(vt_bcode* bc) {
  bc->data = NULL;
  bc->len = 0;
  bc->cap = 0;
}
void vt_bcode_free(vt_bcode* bc) {
  if (bc->data) free(bc->data);
  bc->data = NULL;
  bc->len = bc->cap = 0;
}

int vt_emit_u8(vt_bcode* bc, uint8_t v) {
  if (!ensure(bc, 1)) return 0;
  bc->data[bc->len++] = v;
  return 1;
}
int vt_emit_u16(vt_bcode* bc, uint16_t v) {
  if (!ensure(bc, 2)) return 0;
  wr_u16(bc->data + bc->len, v);
  bc->len += 2;
  return 1;
}
int vt_emit_u32(vt_bcode* bc, uint32_t v) {
  if (!ensure(bc, 4)) return 0;
  wr_u32(bc->data + bc->len, v);
  bc->len += 4;
  return 1;
}
int vt_emit_i32(vt_bcode* bc, int32_t v) {
  return vt_emit_u32(bc, (uint32_t)v);
}
int vt_emit_i64(vt_bcode* bc, int64_t v) {
  if (!ensure(bc, 8)) return 0;
  wr_u64(bc->data + bc->len, (uint64_t)v);
  bc->len += 8;
  return 1;
}
int vt_emit_f64(vt_bcode* bc, double v) {
  if (!ensure(bc, 8)) return 0;
  wr_f64(bc->data + bc->len, v);
  bc->len += 8;
  return 1;
}

int vt_emit_insn(vt_bcode* bc, vt_opcode op, const uint64_t* imm) {
  const vt_opcode_info* ii = vt_op_info(op);
  if (!ii) return 0;
  size_t need = op_size(op);
  if (!ensure(bc, need)) return 0;
  size_t off = bc->len;
  vt_emit_u8(bc, (uint8_t)op);
  for (uint8_t k = 0; k < ii->argc; k++) {
    uint64_t v = imm ? imm[k] : 0;
    switch (ii->argk[k]) {
      case VT_OK_U8:
        wr_u8(bc->data + bc->len, (uint8_t)v);
        bc->len += 1;
        break;
      case VT_OK_U16:
        wr_u16(bc->data + bc->len, (uint16_t)v);
        bc->len += 2;
        break;
      case VT_OK_U32:
        wr_u32(bc->data + bc->len, (uint32_t)v);
        bc->len += 4;
        break;
      case VT_OK_I32:
        wr_i32(bc->data + bc->len, (int32_t)v);
        bc->len += 4;
        break;
      case VT_OK_I64:
        wr_u64(bc->data + bc->len, (uint64_t)v);
        bc->len += 8;
        break;
      case VT_OK_F64:
        wr_f64(bc->data + bc->len, *(double*)&v);
        bc->len += 8;
        break;
      case VT_OK_REL32:
        wr_i32(bc->data + bc->len, (int32_t)v);
        bc->len += 4;
        break;
      case VT_OK_KIDX:
      case VT_OK_SIDX:
        wr_u16(bc->data + bc->len, (uint16_t)v);
        bc->len += 2;
        break;
      default:
        break;
    }
  }
  return (int)(bc->len - off);
}

int vt_patch_rel32(uint8_t* code, size_t from_off, int32_t rel) {
  /* suppose un op avec rel32 en premier imm */
  if (!code) return 0;
  code[from_off + 1 + 0] = (uint8_t)(rel & 0xFF);
  code[from_off + 1 + 1] = (uint8_t)((rel >> 8) & 0xFF);
  code[from_off + 1 + 2] = (uint8_t)((rel >> 16) & 0xFF);
  code[from_off + 1 + 3] = (uint8_t)((rel >> 24) & 0xFF);
  return 1;
}

/* ----------------------------------------------------------------------------
   Petite batterie de tests internes (compilation: -DVT_OPCODES_TEST)
---------------------------------------------------------------------------- */
#ifdef VT_OPCODES_TEST
static void hexdump(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    printf("%02x ", p[i]);
  }
  printf("\n");
}
int main(void) {
  vt_bcode bc;
  vt_bcode_init(&bc);

  /* iconst 42 ; fconst 3.14 ; sconst k#7 ; ld 0 ; add ; jt L1 ; halt ; L1: ret
   * 0 */
  uint64_t imm[3] = {0};

  imm[0] = 42;
  vt_emit_insn(&bc, OP_ICONST, imm);
  double pi = 3.14;
  memcpy(&imm[0], &pi, 8);
  vt_emit_insn(&bc, OP_FCONST, imm);
  imm[0] = 7;
  vt_emit_insn(&bc, OP_SCONST, imm);
  imm[0] = 0;
  vt_emit_insn(&bc, OP_LD, imm);
  vt_emit_insn(&bc, OP_ADD, NULL);

  size_t jt_off = bc.len;
  imm[0] = 0;
  vt_emit_insn(&bc, OP_JT, imm); /* placeholder */

  vt_emit_insn(&bc, OP_HALT, NULL);

  size_t L1 = bc.len;
  imm[0] = 0;
  vt_emit_insn(&bc, OP_RET, imm);

  /* patch rel */
  int32_t rel = (int32_t)((int64_t)L1 - (int64_t)(jt_off + op_size(OP_JT)));
  vt_patch_rel32(bc.data, jt_off, rel);

  hexdump(bc.data, bc.len);

  /* verify + disasm */
  char err[128];
  if (!vt_verify(bc.data, bc.len, err, sizeof err)) {
    printf("verify fail: %s\n", err);
    return 1;
  }
  size_t off = 0;
  char line[128];
  while (off < bc.len) {
    int got = vt_disasm_line(bc.data, off, bc.len, line, sizeof line);
    if (!got) break;
    printf("%04zx: %s\n", off, line);
    off += (size_t)got;
  }

  int mx = 0;
  if (vt_calc_stack_max(bc.data, bc.len, &mx)) {
    printf("stack_max≈%d\n", mx);
  }

  vt_bcode_free(&bc);
  return 0;
}
#endif
