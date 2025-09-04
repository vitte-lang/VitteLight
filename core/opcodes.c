// vitte-light/core/opcodes.c
// Métadonnées d'opcodes, helpers d'encodage/décodage, désassembleur VLBC.
// Couvre le set d'instructions minimal de VitteLight tel qu'utilisé par
// api.c/jumptab.c.
//
// Fourni:
//   - Table d'opcodes (nom, code, opérandes, effets de pile)
//   - Helpers little-endian rd/wr
//   - Taille d'instruction, validation borne/consts
//   - Désassembleur 1-instr et programme complet
//   - Émetteurs de bytecode et helpers VLBC (header, pool de chaînes, code)
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/opcodes.c

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"     // VL_Status, VL_Context (pas requis pour désasm)
#include "limits.h"  // VLBC_* si dispo, sinon valeurs locales

// ───────────────────────── Définition des opcodes ─────────────────────────
// Si non déjà définis par api.h, on reconfirme ici les codes attendus par la
// VM.
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

// Kinds d'opérandes
typedef enum {
  OPD_NONE = 0,
  OPD_U8 = 1,
  OPD_U32 = 2,
  OPD_U64 = 3,
  OPD_F64 = 4
} VLOperandKind;

// Infos d'une instruction
typedef struct {
  const char *name;      // mnémonique
  uint8_t code;          // opcode
  uint8_t nops;          // nombre d'opérandes
  VLOperandKind ops[3];  // kinds d'opérandes
  int8_t pop;            // éléments pop (si connu, -1 pour dépendant)
  int8_t push;           // éléments push (si connu, -1 pour dépendant)
} VLOpInfo;

static const VLOpInfo kOps[] = {
    {"NOP", OP_NOP, 0, {OPD_NONE, 0, 0}, 0, 0},
    {"PUSHI", OP_PUSHI, 1, {OPD_U64, 0, 0}, 0, 1},
    {"PUSHF", OP_PUSHF, 1, {OPD_F64, 0, 0}, 0, 1},
    {"PUSHS", OP_PUSHS, 1, {OPD_U32, 0, 0}, 0, 1},
    {"ADD", OP_ADD, 0, {0, 0, 0}, 2, 1},
    {"SUB", OP_SUB, 0, {0, 0, 0}, 2, 1},
    {"MUL", OP_MUL, 0, {0, 0, 0}, 2, 1},
    {"DIV", OP_DIV, 0, {0, 0, 0}, 2, 1},
    {"EQ", OP_EQ, 0, {0, 0, 0}, 2, 1},
    {"NEQ", OP_NEQ, 0, {0, 0, 0}, 2, 1},
    {"LT", OP_LT, 0, {0, 0, 0}, 2, 1},
    {"GT", OP_GT, 0, {0, 0, 0}, 2, 1},
    {"LE", OP_LE, 0, {0, 0, 0}, 2, 1},
    {"GE", OP_GE, 0, {0, 0, 0}, 2, 1},
    {"PRINT", OP_PRINT, 0, {0, 0, 0}, 0, 0},  // peek puis print
    {"POP", OP_POP, 0, {0, 0, 0}, 1, 0},
    {"STOREG", OP_STOREG, 1, {OPD_U32, 0, 0}, 1, 0},
    {"LOADG", OP_LOADG, 1, {OPD_U32, 0, 0}, 0, 1},
    {"CALLN", OP_CALLN, 2, {OPD_U32, OPD_U8, 0}, -1, -1},  // dépend de argc et
                                                           // du natif
    {"HALT", OP_HALT, 0, {0, 0, 0}, 0, 0},
};

static const size_t kOpsCount = sizeof(kOps) / sizeof(kOps[0]);

// ───────────────────────── Helpers table ─────────────────────────
static const VLOpInfo *opinfo_by_code(uint8_t op) {
  for (size_t i = 0; i < kOpsCount; i++)
    if (kOps[i].code == op) return &kOps[i];
  return NULL;
}
static const VLOpInfo *opinfo_by_name(const char *name) {
  for (size_t i = 0; i < kOpsCount; i++)
    if (strcmp(kOps[i].name, name) == 0) return &kOps[i];
  return NULL;
}
const char *vl_op_name(uint8_t op) {
  const VLOpInfo *i = opinfo_by_code(op);
  return i ? i->name : "?";
}
int vl_op_from_name(const char *name) {
  const VLOpInfo *i = opinfo_by_name(name);
  return i ? (int)i->code : -1;
}

// ───────────────────────── I/O little-endian ─────────────────────────
static inline void wr_u8(uint8_t **p, uint8_t v) { *(*p)++ = v; }
static inline void wr_u32(uint8_t **p, uint32_t v) {
  (*p)[0] = (uint8_t)(v);
  (*p)[1] = (uint8_t)(v >> 8);
  (*p)[2] = (uint8_t)(v >> 16);
  (*p)[3] = (uint8_t)(v >> 24);
  *p += 4;
}
static inline void wr_u64(uint8_t **p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    *(*p)++ = (uint8_t)(v >> (8 * i));
  }
}
static inline void wr_f64(uint8_t **p, double d) {
  union {
    uint64_t u;
    double d;
  } u;
  u.d = d;
  wr_u64(p, u.u);
}

static inline int rd_u8(const uint8_t *p, size_t n, size_t *io, uint8_t *out) {
  if (*io + 1 > n) return 0;
  *out = p[(*io)++];
  return 1;
}
static inline int rd_u32(const uint8_t *p, size_t n, size_t *io,
                         uint32_t *out) {
  if (*io + 4 > n) return 0;
  uint32_t v = (uint32_t)p[*io] | ((uint32_t)p[*io + 1] << 8) |
               ((uint32_t)p[*io + 2] << 16) | ((uint32_t)p[*io + 3] << 24);
  *io += 4;
  *out = v;
  return 1;
}
static inline int rd_u64(const uint8_t *p, size_t n, size_t *io,
                         uint64_t *out) {
  if (*io + 8 > n) return 0;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)p[*io + i]) << (8 * i);
  *io += 8;
  *out = v;
  return 1;
}
static inline int rd_f64(const uint8_t *p, size_t n, size_t *io, double *out) {
  union {
    uint64_t u;
    double d;
  } u;
  if (!rd_u64(p, n, io, &u.u)) return 0;
  *out = u.d;
  return 1;
}

// ───────────────────────── Taille instruction ─────────────────────────
size_t vl_op_insn_size(uint8_t op) {
  const VLOpInfo *i = opinfo_by_code(op);
  if (!i) return 1;
  size_t sz = 1;
  for (uint8_t k = 0; k < i->nops; k++) {
    switch (i->ops[k]) {
      case OPD_U8:
        sz += 1;
        break;
      case OPD_U32:
        sz += 4;
        break;
      case OPD_U64:
      case OPD_F64:
        sz += 8;
        break;
      default:
        break;
    }
  }
  return sz;
}

// retourne 0 si out-of-bounds
size_t vl_insn_size_at(const uint8_t *code, size_t code_len, size_t ip) {
  if (ip >= code_len) return 0;
  uint8_t op = code[ip];
  size_t need = vl_op_insn_size(op);
  return (ip + need <= code_len) ? need : 0;
}

// ───────────────────────── Validation bytecode ─────────────────────────
// Vérifie que tout le flux est lisible, que les indices de chaînes sont <
// kstr_len.
VL_Status vl_validate_code(const uint8_t *code, size_t code_len,
                           size_t kstr_len) {
  size_t ip = 0;
  while (ip < code_len) {
    uint8_t op = code[ip++];
    const VLOpInfo *i = opinfo_by_code(op);
    if (!i) return VL_ERR_BAD_BYTECODE;
    for (uint8_t a = 0; a < i->nops; a++) {
      switch (i->ops[a]) {
        case OPD_U8:
          if (ip + 1 > code_len) return VL_ERR_BAD_BYTECODE;
          ip += 1;
          break;
        case OPD_U32:
          if (ip + 4 > code_len) return VL_ERR_BAD_BYTECODE;
          if (op == OP_PUSHS || op == OP_LOADG || op == OP_STOREG ||
              (op == OP_CALLN && a == 0)) {
            uint32_t si = (uint32_t)code[ip] | ((uint32_t)code[ip + 1] << 8) |
                          ((uint32_t)code[ip + 2] << 16) |
                          ((uint32_t)code[ip + 3] << 24);
            if (si >= kstr_len) return VL_ERR_BAD_BYTECODE;
          }
          ip += 4;
          break;
        case OPD_U64:
          if (ip + 8 > code_len) return VL_ERR_BAD_BYTECODE;
          ip += 8;
          break;
        case OPD_F64:
          if (ip + 8 > code_len) return VL_ERR_BAD_BYTECODE;
          ip += 8;
          break;
        default:
          break;
      }
    }
  }
  return VL_OK;
}

// ───────────────────────── Désassembleur ─────────────────────────
// Imprime une instruction en clair dans buf. Retourne le nombre de chars
// écrits.
size_t vl_disasm_one(const uint8_t *code, size_t code_len, size_t ip, char *buf,
                     size_t n) {
  if (ip >= code_len) {
    if (n > 0) buf[0] = '\0';
    return 0;
  }
  uint8_t op = code[ip++];
  const VLOpInfo *i = opinfo_by_code(op);
  if (!i) {
    return (size_t)snprintf(buf, n, ".db 0x%02X", op);
  }
  size_t k = 0;
  int m = snprintf(buf + k, n > k ? n - k : 0, "%s", i->name);
  if (m < 0) return 0;
  k += (size_t)m;
  for (uint8_t a = 0; a < i->nops; a++) {
    if (k < n) buf[k++] = ' ';
    switch (i->ops[a]) {
      case OPD_U8: {
        uint8_t v = 0;
        size_t t = ip;
        if (!rd_u8(code, code_len, &t, &v)) return k;
        ip = t;
        m = snprintf(buf + k, n > k ? n - k : 0, "%u", (unsigned)v);
      } break;
      case OPD_U32: {
        uint32_t v = 0;
        size_t t = ip;
        if (!rd_u32(code, code_len, &t, &v)) return k;
        ip = t;
        m = snprintf(buf + k, n > k ? n - k : 0, "#%u", (unsigned)v);
      } break;
      case OPD_U64: {
        uint64_t v = 0;
        size_t t = ip;
        if (!rd_u64(code, code_len, &t, &v)) return k;
        ip = t;
        m = snprintf(buf + k, n > k ? n - k : 0, "%" PRIu64, v);
      } break;
      case OPD_F64: {
        double d = 0;
        size_t t = ip;
        if (!rd_f64(code, code_len, &t, &d)) return k;
        ip = t;
        m = snprintf(buf + k, n > k ? n - k : 0, "%g", d);
      } break;
      default:
        m = 0;
        break;
    }
    if (m < 0) return k;
    k += (size_t)m;
  }
  if (k < n) buf[k] = '\0';
  return k;
}

// Désassemble tout le programme vers un FILE*.
void vl_disasm_program(const uint8_t *code, size_t code_len, FILE *out) {
  if (!out) out = stdout;
  size_t ip = 0;
  char line[128];
  while (ip < code_len) {
    size_t ip0 = ip;
    size_t insz = vl_insn_size_at(code, code_len, ip);
    if (!insz) {
      fprintf(out, "%04zu: <bad>\n", ip);
      break;
    }
    vl_disasm_one(code, code_len, ip, line, sizeof(line));
    fprintf(out, "%04zu: %-16s  ", ip0, line);
    // hex dump
    for (size_t j = 0; j < insz; j++) fprintf(out, "%02X ", code[ip0 + j]);
    fputc('\n', out);
    ip += insz;
  }
}

// ───────────────────────── Émetteurs bytecode ─────────────────────────
// Emet une instruction dans *pp et avance le pointeur.
void vl_emit_NOP(uint8_t **pp) { wr_u8(pp, OP_NOP); }
void vl_emit_PUSHI(uint8_t **pp, int64_t v) {
  wr_u8(pp, OP_PUSHI);
  wr_u64(pp, (uint64_t)v);
}
void vl_emit_PUSHF(uint8_t **pp, double d) {
  wr_u8(pp, OP_PUSHF);
  wr_f64(pp, d);
}
void vl_emit_PUSHS(uint8_t **pp, uint32_t si) {
  wr_u8(pp, OP_PUSHS);
  wr_u32(pp, si);
}
void vl_emit_ADD(uint8_t **pp) { wr_u8(pp, OP_ADD); }
void vl_emit_SUB(uint8_t **pp) { wr_u8(pp, OP_SUB); }
void vl_emit_MUL(uint8_t **pp) { wr_u8(pp, OP_MUL); }
void vl_emit_DIV(uint8_t **pp) { wr_u8(pp, OP_DIV); }
void vl_emit_EQ(uint8_t **pp) { wr_u8(pp, OP_EQ); }
void vl_emit_NEQ(uint8_t **pp) { wr_u8(pp, OP_NEQ); }
void vl_emit_LT(uint8_t **pp) { wr_u8(pp, OP_LT); }
void vl_emit_GT(uint8_t **pp) { wr_u8(pp, OP_GT); }
void vl_emit_LE(uint8_t **pp) { wr_u8(pp, OP_LE); }
void vl_emit_GE(uint8_t **pp) { wr_u8(pp, OP_GE); }
void vl_emit_PRINT(uint8_t **pp) { wr_u8(pp, OP_PRINT); }
void vl_emit_POP(uint8_t **pp) { wr_u8(pp, OP_POP); }
void vl_emit_STOREG(uint8_t **pp, uint32_t si) {
  wr_u8(pp, OP_STOREG);
  wr_u32(pp, si);
}
void vl_emit_LOADG(uint8_t **pp, uint32_t si) {
  wr_u8(pp, OP_LOADG);
  wr_u32(pp, si);
}
void vl_emit_CALLN(uint8_t **pp, uint32_t name_si, uint8_t argc) {
  wr_u8(pp, OP_CALLN);
  wr_u32(pp, name_si);
  wr_u8(pp, argc);
}
void vl_emit_HALT(uint8_t **pp) { wr_u8(pp, OP_HALT); }

// ───────────────────────── Conteneur VLBC (header, pool, code)
// ─────────────────────────
#ifndef VLBC_MAGIC
#define VLBC_MAGIC "VLBC"
#endif
#ifndef VLBC_VERSION
#define VLBC_VERSION 1u
#endif

// Ecrit l'entête: magic(4) + version(1)
void vl_bc_emit_header(uint8_t **pp, uint8_t version) {
  wr_u8(pp, (uint8_t)VLBC_MAGIC[0]);
  wr_u8(pp, (uint8_t)VLBC_MAGIC[1]);
  wr_u8(pp, (uint8_t)VLBC_MAGIC[2]);
  wr_u8(pp, (uint8_t)VLBC_MAGIC[3]);
  wr_u8(pp, version);
}

// Ecrit le pool de chaînes: count(u32) puis (len(u32), bytes) * count
void vl_bc_emit_kstr(uint8_t **pp, const char *const *kstr, uint32_t count) {
  wr_u32(pp, count);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t L = (uint32_t)strlen(kstr[i]);
    wr_u32(pp, L);
    memcpy(*pp, kstr[i], L);
    *pp += L;
  }
}

// Réserve le champ taille du code et renvoie un pointeur pour le backfill
uint8_t *vl_bc_begin_code(uint8_t **pp) {
  uint8_t *slot = *pp;
  wr_u32(pp, 0u);
  return slot;
}
void vl_bc_end_code(uint8_t *size_slot, const uint8_t *code_begin,
                    const uint8_t *code_end) {
  uint32_t sz = (uint32_t)(code_end - code_begin);
  size_slot[0] = (uint8_t)(sz);
  size_slot[1] = (uint8_t)(sz >> 8);
  size_slot[2] = (uint8_t)(sz >> 16);
  size_slot[3] = (uint8_t)(sz >> 24);
}

// ───────────────────────── Test autonome ─────────────────────────
#ifdef VL_OPCODES_TEST_MAIN
int main(void) {
  // Construire un mini programme: print("hello")
  const char *kstr[] = {"hello", "print"};
  uint8_t buf[512];
  uint8_t *p = buf;
  vl_bc_emit_header(&p, (uint8_t)VLBC_VERSION);
  vl_bc_emit_kstr(&p, kstr, 2);
  uint8_t *code_size = vl_bc_begin_code(&p);
  uint8_t *code_begin = p;
  vl_emit_PUSHS(&p, 0);     // "hello"
  vl_emit_CALLN(&p, 1, 1);  // print 1 arg
  vl_emit_HALT(&p);
  uint8_t *code_end = p;
  vl_bc_end_code(code_size, code_begin, code_end);

  size_t total = (size_t)(p - buf);
  fprintf(stderr, "VLBC bytes=%zu\n", total);

  // Désassembler
  size_t ip = (size_t)((5) + 4 + (4 + 5) + (4 + 5) +
                       4);  // header + count + strings + code_size
  vl_disasm_program(buf + ip, (size_t)(code_end - code_begin), stdout);
  return 0;
}
#endif
