// vitte-light/core/parser.c
// Assembleur ultra-complet pour VitteLight (source ASM -> conteneur VLBC)
//
// Entrées supportées (tokens via core/lex.c):
//   - Instructions: NOP, PUSHI <i64>, PUSHF <f64>, PUSHS <str|id>,
//                   ADD,SUB,MUL,DIV, EQ,NEQ,LT,GT,LE,GE,
//                   PRINT, POP, STOREG <id>, LOADG <id>, CALLN <id|"str">
//                   <argc>, HALT
//   - Identifiants et littéraux numériques standards
//   - Chaînes "..." avec échappements (prises directement en PUSHS ou comme
//   nom)
//   - Labels optionnels: <id> ':' (conservés mais pas utilisés sans opcodes de
//   saut)
//   - Séparateurs d'arguments: espace ou virgule facultative
//   - Commentaires: '#', ';', '//', '/* ... */'
//
// Sortie: buffer binaire contenant un module VLBC
//   [ 'VLBC' 1 ] [kstr_count:u32] [(len:u32, bytes)*] [code_size:u32]
//   [code_bytes]
//
// API primaire (si vous créez parser.h, exportez ces symboles):
//   int vl_asm(const char *src, size_t n, uint8_t **out_bytes, size_t
//   *out_size,
//              char *err, size_t errn);
//   int vl_asm_file(const char *path, uint8_t **out_bytes, size_t *out_size,
//                   char *err, size_t errn);
//
// Dépendances:
//   core/lex.h, core/opcodes.h, core/mem.h, core/limits.h
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/parser.c

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lex.h"
#include "limits.h"
#include "mem.h"
#include "opcodes.h"

// ───────────────────────── Utilitaires erreurs ─────────────────────────

typedef struct {
  char msg[256];
  int line, col;
} AsmError;
static void asm_err(AsmError *e, int line, int col, const char *fmt, ...) {
  e->line = line;
  e->col = col;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
  va_end(ap);
}

// ───────────────────────── KString pool (interning) ─────────────────────────

typedef struct {
  const char *s;
  uint32_t n;
  uint32_t idx;
} KSEntry;

typedef struct {
  VL_Arena arena;  // stockage des copies C-string NUL-terminées
  KSEntry *tab;    // table open addressing
  size_t cap;      // taille de tab (puissance de 2 conseillée)
  size_t len;      // #entrées occupées
  char **list;     // table des cstr par index (pour écriture VLBC)
  uint32_t list_n;
  uint32_t list_cap;
} KStrPool;

static uint32_t ks_hash(const char *s, uint32_t n) {  // FNV-1a 32-bit
  const unsigned char *p = (const unsigned char *)s;
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}

static void ks_init(KStrPool *K) {
  memset(K, 0, sizeof(*K));
  vl_arena_init(&K->arena, 8 * 1024, 8);
  K->cap = 64;
  K->tab = (KSEntry *)calloc(K->cap, sizeof(KSEntry));
  K->list_cap = 64;
  K->list = (char **)calloc(K->list_cap, sizeof(char *));
}
static void ks_release(KStrPool *K) {
  free(K->tab);
  free(K->list);
  vl_arena_release(&K->arena);
  memset(K, 0, sizeof(*K));
}

static int ks_grow(KStrPool *K) {
  size_t ncap = K->cap ? K->cap * 2 : 64;
  KSEntry *nt = (KSEntry *)calloc(ncap, sizeof(KSEntry));
  if (!nt) return 0;
  for (size_t i = 0; i < K->cap; i++) {
    if (K->tab[i].s) {
      const char *s = K->tab[i].s;
      uint32_t n = K->tab[i].n;
      uint32_t idx = K->tab[i].idx;
      uint32_t h = ks_hash(s, n);
      size_t j = (size_t)(h & (ncap - 1));
      while (nt[j].s) j = (j + 1) & (ncap - 1);
      nt[j].s = s;
      nt[j].n = n;
      nt[j].idx = idx;
    }
  }
  free(K->tab);
  K->tab = nt;
  K->cap = ncap;
  return 1;
}

static uint32_t ks_intern(KStrPool *K, const char *s, uint32_t n) {
  if (!K->cap) ks_init(K);
  if (!s) s = "";
  if (n == 0) n = (uint32_t)strlen(s);
  if (K->len * 100 >= K->cap * 70) {
    if (!ks_grow(K)) return UINT32_MAX;
  }
  uint32_t h = ks_hash(s, n);
  size_t i = (size_t)(h & (K->cap - 1));
  for (;;) {
    if (!K->tab[i].s) {  // insert
      char *copy = vl_arena_strndup(&K->arena, s, n);
      if (!copy) return UINT32_MAX;
      if (K->list_n == K->list_cap) {
        uint32_t nc = K->list_cap ? K->list_cap * 2 : 64;
        char **np = (char **)realloc(K->list, nc * sizeof(char *));
        if (!np) return UINT32_MAX;
        K->list = np;
        K->list_cap = nc;
      }
      uint32_t idx = K->list_n++;
      K->list[idx] = copy;
      K->tab[i].s = copy;
      K->tab[i].n = n;
      K->tab[i].idx = idx;
      K->len++;
      return idx;
    }
    if (K->tab[i].n == n && memcmp(K->tab[i].s, s, n) == 0)
      return K->tab[i].idx;
    i = (i + 1) & (K->cap - 1);
  }
}

// ───────────────────────── Assembleur ─────────────────────────

typedef struct {
  VL_Lexer lx;
  VL_Buffer code;  // code binaire
  KStrPool kstr;   // pool des constantes
  AsmError err;
  int had_err;
} Asm;

static void asm_init(Asm *A, const char *src, size_t n) {
  memset(A, 0, sizeof(*A));
  vl_lex_init(&A->lx, src, n);
  vl_lex_cfg(&A->lx, 1);
  vl_buf_init(&A->code);
  ks_init(&A->kstr);
}
static void asm_release(Asm *A) {
  ks_release(&A->kstr);
  vl_buf_free(&A->code);
}

static VL_Token peek(Asm *A) {
  VL_Token t;
  vl_lex_peek(&A->lx, &t);
  return t;
}
static VL_Token next(Asm *A) { return vl_lex_next(&A->lx); }
static int is_nl_or_eof(const VL_Token *t) {
  return t->kind == VL_TK_NL || t->kind == VL_TK_EOF;
}
static void skip_nl(Asm *A) {
  for (;;) {
    VL_Token t = peek(A);
    if (t.kind == VL_TK_NL) {
      (void)next(A);
      continue;
    }
    break;
  }
}

static int expect_char(Asm *A, int ch) {
  VL_Token t = next(A);
  if (!(t.kind == VL_TK_PUNCT && t.v.ch == ch)) {
    asm_err(&A->err, t.line, t.col, "'%c' attendu", ch);
    A->had_err = 1;
    return 0;
  }
  return 1;
}

static int optional_comma(Asm *A) {
  VL_Token t = peek(A);
  if (t.kind == VL_TK_PUNCT && t.v.ch == ',') {
    (void)next(A);
    return 1;
  }
  return 0;
}

// Réserve 'need' octets dans A->code et renvoie le pointeur d'écriture
static uint8_t *emit_reserve(Asm *A, size_t need) {
  if (!vl_buf_reserve(&A->code, A->code.n + need + 1)) return NULL;
  return A->code.d + A->code.n;
}
static void emit_commit(Asm *A, size_t wrote) {
  A->code.n += wrote;
  A->code.d[A->code.n] = '\0';
}

static int take_opcode(Asm *A, const char *name, size_t n) {
  char buf[32];
  size_t k = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
  memcpy(buf, name, k);
  buf[k] = '\0';
  return vl_op_from_name(buf);
}

// Parse un argument entier signé
static int64_t parse_i64(Asm *A, int *ok) {
  VL_Token t = next(A);
  if (t.kind == VL_TK_INT) {
    *ok = 1;
    return (int64_t)t.v.i64;
  }
  if (t.kind == VL_TK_FLOAT) {
    *ok = 1;
    return (int64_t)t.v.f64;
  }
  asm_err(&A->err, t.line, t.col, "entier attendu");
  A->had_err = 1;
  *ok = 0;
  return 0;
}
// Parse un argument float
static double parse_f64(Asm *A, int *ok) {
  VL_Token t = next(A);
  if (t.kind == VL_TK_FLOAT) {
    *ok = 1;
    return t.v.f64;
  }
  if (t.kind == VL_TK_INT) {
    *ok = 1;
    return (double)t.v.i64;
  }
  asm_err(&A->err, t.line, t.col, "float attendu");
  A->had_err = 1;
  *ok = 0;
  return 0.0;
}

// Parse un identifiant ou chaîne comme nom dans le kstr; retourne son index
static uint32_t parse_name_index(Asm *A, int *ok) {
  VL_Token t = next(A);
  if (t.kind == VL_TK_ID) {
    uint32_t idx = ks_intern(&A->kstr, t.start, (uint32_t)t.len);
    *ok = 1;
    return idx;
  }
  if (t.kind == VL_TK_STRING) {
    const char *s = t.v.str ? t.v.str : "";
    uint32_t idx = ks_intern(&A->kstr, s, (uint32_t)strlen(s));
    vl_tok_free(&t);
    *ok = 1;
    return idx;
  }
  asm_err(&A->err, t.line, t.col, "nom attendu");
  A->had_err = 1;
  *ok = 0;
  return 0;
}

static int parse_end_of_line(Asm *A) {
  VL_Token t = peek(A);
  if (t.kind == VL_TK_EOF || t.kind == VL_TK_NL) return 1;
  asm_err(&A->err, t.line, t.col, "fin de ligne attendue");
  A->had_err = 1;
  return 0;
}

// Émission d'une instruction connue avec vérif de taille
static int emit_op0(Asm *A, uint8_t op) {
  size_t need = vl_op_insn_size(op);
  uint8_t *p = emit_reserve(A, need);
  if (!p) return 0;
  switch (op) {
    case OP_NOP:
      vl_emit_NOP(&p);
      break;
    case OP_ADD:
      vl_emit_ADD(&p);
      break;
    case OP_SUB:
      vl_emit_SUB(&p);
      break;
    case OP_MUL:
      vl_emit_MUL(&p);
      break;
    case OP_DIV:
      vl_emit_DIV(&p);
      break;
    case OP_EQ:
      vl_emit_EQ(&p);
      break;
    case OP_NEQ:
      vl_emit_NEQ(&p);
      break;
    case OP_LT:
      vl_emit_LT(&p);
      break;
    case OP_GT:
      vl_emit_GT(&p);
      break;
    case OP_LE:
      vl_emit_LE(&p);
      break;
    case OP_GE:
      vl_emit_GE(&p);
      break;
    case OP_PRINT:
      vl_emit_PRINT(&p);
      break;
    case OP_POP:
      vl_emit_POP(&p);
      break;
    case OP_HALT:
      vl_emit_HALT(&p);
      break;
    default:
      return 0;
  }
  emit_commit(A, need);
  return 1;
}

static int emit_PUSHI(Asm *A, int64_t v) {
  size_t need = vl_op_insn_size(OP_PUSHI);
  uint8_t *p = emit_reserve(A, need);
  if (!p) return 0;
  vl_emit_PUSHI(&p, v);
  emit_commit(A, need);
  return 1;
}
static int emit_PUSHF(Asm *A, double d) {
  size_t need = vl_op_insn_size(OP_PUSHF);
  uint8_t *p = emit_reserve(A, need);
  if (!p) return 0;
  vl_emit_PUSHF(&p, d);
  emit_commit(A, need);
  return 1;
}
static int emit_PUSHS(Asm *A, uint32_t si) {
  size_t need = vl_op_insn_size(OP_PUSHS);
  uint8_t *p = emit_reserve(A, need);
  if (!p) return 0;
  vl_emit_PUSHS(&p, si);
  emit_commit(A, need);
  return 1;
}
static int emit_STOREG(Asm *A, uint32_t si) {
  size_t need = vl_op_insn_size(OP_STOREG);
  uint8_t *p = emit_reserve(A, need);
  if (!p) return 0;
  vl_emit_STOREG(&p, si);
  emit_commit(A, need);
  return 1;
}
static int emit_LOADG(Asm *A, uint32_t si) {
  size_t need = vl_op_insn_size(OP_LOADG);
  uint8_t *p = emit_reserve(A, need);
  if (!p) return 0;
  vl_emit_LOADG(&p, si);
  emit_commit(A, need);
  return 1;
}
static int emit_CALLN(Asm *A, uint32_t name_si, uint8_t argc) {
  size_t need = vl_op_insn_size(OP_CALLN);
  uint8_t *p = emit_reserve(A, need);
  if (!p) return 0;
  vl_emit_CALLN(&p, name_si, argc);
  emit_commit(A, need);
  return 1;
}

// Ligne: [label ':']? instr [args]
static int parse_line(Asm *A) {
  skip_nl(A);
  VL_Token t = peek(A);
  if (t.kind == VL_TK_EOF) return 0;
  if (t.kind == VL_TK_NL) {
    (void)next(A);
    return 1;
  }
  // label optionnel
  if (t.kind == VL_TK_ID) {
    VL_Token id = next(A);
    VL_Token p = peek(A);
    if (p.kind == VL_TK_PUNCT && p.v.ch == ':') {
      (void)next(A);  // label défini à A->code.n (non utilisé ici)
      // Consommer espaces/NL éventuels avant une éventuelle instruction
      skip_nl(A);
      t = peek(A);
      if (is_nl_or_eof(&t)) return 1;  // ligne avec label seul
    } else {
      // c'était un opcode; on remet dans le flux
      vl_lex_unget(&A->lx, &id);
    }
  }

  t = next(A);
  if (t.kind != VL_TK_ID) {
    asm_err(&A->err, t.line, t.col, "mnémonique attendu");
    A->had_err = 1;
    return 0;
  }

  // opcode par nom
  int opc = take_opcode(A, t.start, t.len);
  if (opc < 0) {
    asm_err(&A->err, t.line, t.col, "opcode inconnu");
    A->had_err = 1;
    return 0;
  }

  int ok = 1;  // parsing state
  switch (opc) {
    case OP_NOP:
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
    case OP_PRINT:
    case OP_POP:
    case OP_HALT:
      if (!emit_op0(A, (uint8_t)opc)) return 0;
      break;
    case OP_PUSHI: {
      int64_t v = parse_i64(A, &ok);
      if (!ok) return 0;
      if (!emit_PUSHI(A, v)) return 0;
    } break;
    case OP_PUSHF: {
      double d = parse_f64(A, &ok);
      if (!ok) return 0;
      if (!emit_PUSHF(A, d)) return 0;
    } break;
    case OP_PUSHS: {
      uint32_t si = parse_name_index(A, &ok);
      if (!ok) return 0;
      if (!emit_PUSHS(A, si)) return 0;
    } break;
    case OP_STOREG: {
      uint32_t si = parse_name_index(A, &ok);
      if (!ok) return 0;
      if (!emit_STOREG(A, si)) return 0;
    } break;
    case OP_LOADG: {
      uint32_t si = parse_name_index(A, &ok);
      if (!ok) return 0;
      if (!emit_LOADG(A, si)) return 0;
    } break;
    case OP_CALLN: {
      uint32_t si = parse_name_index(A, &ok);
      if (!ok) return 0;
      optional_comma(A);
      int64_t argc = parse_i64(A, &ok);
      if (!ok) return 0;
      if (argc < 0 || argc > 255) {
        asm_err(&A->err, t.line, t.col, "argc invalide");
        A->had_err = 1;
        return 0;
      }
      if (!emit_CALLN(A, si, (uint8_t)argc)) return 0;
    } break;
    default:
      asm_err(&A->err, t.line, t.col, "opcode non géré");
      A->had_err = 1;
      return 0;
  }

  // fin de ligne ou EOF
  VL_Token e = peek(A);
  if (e.kind == VL_TK_EOF || e.kind == VL_TK_NL) return 1;
  // tolère un trailing commentaire déjà sauté par le lexer; sinon erreur si
  // reste
  if (e.kind == VL_TK_PUNCT && e.v.ch == ',') {
    (void)next(A); /* virgule superflue -> ignore */
  }
  return parse_end_of_line(A);
}

// ───────────────────────── Driver d'assemblage ─────────────────────────
static int assemble(Asm *A) {
  while (1) {
    VL_Token t = peek(A);
    if (t.kind == VL_TK_EOF) break;
    if (!parse_line(A)) break;
  }
  return A->had_err ? 0 : 1;
}

// ───────────────────────── Construction VLBC ─────────────────────────
static int build_vlbc(Asm *A, uint8_t **out_bytes, size_t *out_size) {
  // bornes
  if (A->kstr.list_n > VLBC_MAX_STRINGS) return 0;
  if (A->code.n > VLBC_MAX_CODE_BYTES) return 0;

  // buffer sortie
  VL_Buffer out;
  vl_buf_init(&out);
  // estimation grossière: header(5) + pool + code_size + code
  size_t est = 5 + 4 + (size_t)A->kstr.list_n * (4 + 8) + 4 +
               A->code.n;  // strings moyennes ~8
  if (!vl_buf_reserve(&out, est)) {
    vl_buf_free(&out);
    return 0;
  }
  uint8_t *p = out.d + out.n;

  vl_bc_emit_header(&p, (uint8_t)VLBC_VERSION);
  vl_bc_emit_kstr(&p, (const char *const *)A->kstr.list, A->kstr.list_n);
  uint8_t *code_size_slot = vl_bc_begin_code(&p);
  uint8_t *code_begin = p;
  memcpy(p, A->code.d, A->code.n);
  p += A->code.n;
  vl_bc_end_code(code_size_slot, code_begin, p);

  out.n = (size_t)(p - out.d);

  // validation
  // Localiser l'offset du code: 4(magic)+1(ver)+4(count)+Σ(4+len)+4(code_size)
  size_t off = 5 + 4;
  for (uint32_t i = 0; i < A->kstr.list_n; i++) {
    off += 4 + strlen(A->kstr.list[i]);
  }
  off += 4;
  size_t code_len = A->code.n;
  if (vl_validate_code(out.d + off, code_len, A->kstr.list_n) != VL_OK) {
    vl_buf_free(&out);
    return 0;
  }

  // produire le buffer final
  *out_size = out.n;
  *out_bytes = (uint8_t *)malloc(out.n);
  if (!*out_bytes) {
    vl_buf_free(&out);
    return 0;
  }
  memcpy(*out_bytes, out.d, out.n);
  vl_buf_free(&out);
  return 1;
}

// ───────────────────────── API externe ─────────────────────────
int vl_asm(const char *src, size_t n, uint8_t **out_bytes, size_t *out_size,
           char *err, size_t errn) {
  if (!src || !out_bytes || !out_size) return 0;
  Asm A;
  asm_init(&A, src, n);
  int ok = assemble(&A);
  if (ok) ok = build_vlbc(&A, out_bytes, out_size);
  if (!ok) {
    if (err && errn > 0) {
      snprintf(err, errn, "L%d C%d: %s", A.err.line, A.err.col,
               A.err.msg[0] ? A.err.msg : "erreur d'assemblage");
    }
  }
  asm_release(&A);
  return ok;
}

static int read_file_all(const char *path, char **buf, size_t *n) {
  *buf = NULL;
  *n = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return 0;
  }
  rewind(f);
  char *p = (char *)malloc((size_t)sz + 1);
  if (!p) {
    fclose(f);
    return 0;
  }
  size_t rd = fread(p, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(p);
    return 0;
  }
  p[sz] = '\0';
  *buf = p;
  *n = (size_t)sz;
  return 1;
}

int vl_asm_file(const char *path, uint8_t **out_bytes, size_t *out_size,
                char *err, size_t errn) {
  char *src = NULL;
  size_t n = 0;
  if (!read_file_all(path, &src, &n)) return 0;
  int ok = vl_asm(src, n, out_bytes, out_size, err, errn);
  free(src);
  return ok;
}

// ───────────────────────── Test autonome ─────────────────────────
#ifdef VL_PARSER_TEST_MAIN
int main(void) {
  const char *src =
      "; mini programme\n"
      "PUSHI 2\n"
      "PUSHI 40\n"
      "ADD\n"
      "PUSHS \"result\"\n"
      "STOREG x\n"
      "LOADG print\n"
      "CALLN print, 1\n"
      "HALT\n";
  uint8_t *bc = NULL;
  size_t nb = 0;
  char err[128];
  if (!vl_asm(src, strlen(src), &bc, &nb, err, sizeof(err))) {
    fprintf(stderr, "asm failed: %s\n", err);
    return 1;
  }
  // désassembly de la section code seulement
  // localiser le code
  const uint8_t *p = bc;
  size_t off = 5;
  uint32_t kcount = (uint32_t)(p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) |
                               (p[off + 3] << 24));
  off += 4;
  for (uint32_t i = 0; i < kcount; i++) {
    uint32_t L = (uint32_t)(p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) |
                            (p[off + 3] << 24));
    off += 4 + L;
  }
  uint32_t code_len = (uint32_t)(p[off] | (p[off + 1] << 8) |
                                 (p[off + 2] << 16) | (p[off + 3] << 24));
  off += 4;
  vl_disasm_program(p + off, code_len, stdout);
  free(bc);
  return 0;
}
#endif
