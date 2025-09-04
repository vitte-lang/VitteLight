// vitte-light/core/code.c
// CLI de référence pour VitteLight : assembleur VLBC, désassembleur et
// exécuteur. Compile: cc -std=c99 -O2 -Wall -Wextra -pedantic code.c api.c -o
// vitl Usage:
//   ./vitl run program.vlbc
//   ./vitl asm source.vlasm -o program.vlbc
//   ./vitl dis program.vlbc
//   ./vitl demo   # assemble et exécute un programme de démonstration
//
// Format VLBC v1: voir api.c. Ce fichier reste indépendant de l’impl
// de la VM et n’exporte que l’API publique via api.h.

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"

// ───────────────────────── Utils I/O ─────────────────────────

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} Buf;

static void buf_free(Buf *b) {
  if (!b) return;
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}
static bool buf_res(Buf *b, size_t need) {
  if (b->len + need <= b->cap) return true;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < b->len + need) cap *= 2;
  uint8_t *p = realloc(b->data, cap);
  if (!p) return false;
  b->data = p;
  b->cap = cap;
  return true;
}
static bool buf_u8(Buf *b, uint8_t v) {
  if (!buf_res(b, 1)) return false;
  b->data[b->len++] = v;
  return true;
}
static bool buf_u32(Buf *b, uint32_t v) {
  if (!buf_res(b, 4)) return false;
  b->data[b->len++] = (uint8_t)(v);
  b->data[b->len++] = (uint8_t)(v >> 8);
  b->data[b->len++] = (uint8_t)(v >> 16);
  b->data[b->len++] = (uint8_t)(v >> 24);
  return true;
}
static bool buf_u64(Buf *b, uint64_t v) {
  if (!buf_res(b, 8)) return false;
  for (int i = 0; i < 8; i++) b->data[b->len++] = (uint8_t)(v >> (8 * i));
  return true;
}
static bool buf_bytes(Buf *b, const void *p, size_t n) {
  if (!buf_res(b, n)) return false;
  memcpy(b->data + b->len, p, n);
  b->len += n;
  return true;
}

static uint8_t *read_entire_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  uint8_t *buf = (uint8_t *)malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(buf);
    return NULL;
  }
  if (out_len) *out_len = (size_t)sz;
  return buf;
}

static bool write_entire_file(const char *path, const void *data, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t wr = fwrite(data, 1, n, f);
  fclose(f);
  return wr == n;
}

// ───────────────────────── String pool pour VLBC ─────────────────────────

static uint32_t fnv1a(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}

typedef struct {
  char *s;
  uint32_t len;
  uint32_t hash;
} SItem;

typedef struct {
  SItem *v;
  size_t len;
  size_t cap;
} SPool;

static void spool_free(SPool *sp) {
  if (!sp) return;
  for (size_t i = 0; i < sp->len; i++) free(sp->v[i].s);
  free(sp->v);
  sp->v = NULL;
  sp->len = sp->cap = 0;
}
static int spool_find(SPool *sp, const char *s, size_t n, uint32_t h) {
  for (size_t i = 0; i < sp->len; i++) {
    if (sp->v[i].hash == h && sp->v[i].len == n &&
        memcmp(sp->v[i].s, s, n) == 0)
      return (int)i;
  }
  return -1;
}
static int spool_put(SPool *sp, const char *s, size_t n) {
  uint32_t h = fnv1a(s, n);
  int idx = spool_find(sp, s, n, h);
  if (idx >= 0) return idx;
  if (sp->len == sp->cap) {
    size_t nc = sp->cap ? sp->cap * 2 : 16;
    SItem *nv = (SItem *)realloc(sp->v, nc * sizeof(SItem));
    if (!nv) return -1;
    sp->v = nv;
    sp->cap = nc;
  }
  char *dup = (char *)malloc(n + 1);
  if (!dup) return -1;
  memcpy(dup, s, n);
  dup[n] = '\0';
  sp->v[sp->len] = (SItem){dup, (uint32_t)n, h};
  return (int)sp->len++;
}

// ───────────────────────── OpCodes miroir (doit matcher api.c)
// ─────────────────────────

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

// ───────────────────────── Parser ASM minimal ─────────────────────────

typedef struct {
  const char *src;
  size_t n;
  size_t i;
  int line;
} Lex;

static void lex_init(Lex *lx, const char *buf, size_t n) {
  lx->src = buf;
  lx->n = n;
  lx->i = 0;
  lx->line = 1;
}
static void lex_skip_ws(Lex *lx) {
  for (;;) {
    while (lx->i < lx->n && (lx->src[lx->i] == ' ' || lx->src[lx->i] == '\t' ||
                             lx->src[lx->i] == '\r'))
      lx->i++;
    if (lx->i < lx->n && lx->src[lx->i] == '/' && lx->i + 1 < lx->n &&
        lx->src[lx->i + 1] == '/') {
      while (lx->i < lx->n && lx->src[lx->i] != '\n') lx->i++;
    }
    if (lx->i < lx->n && (lx->src[lx->i] == '#' || lx->src[lx->i] == ';')) {
      while (lx->i < lx->n && lx->src[lx->i] != '\n') lx->i++;
    }
    if (lx->i < lx->n && lx->src[lx->i] == '\n') {
      lx->i++;
      lx->line++;
      continue;
    }
    break;
  }
}

static bool lex_id(Lex *lx, const char **out, size_t *on) {
  size_t s = lx->i;
  if (s >= lx->n) return false;
  if (!(isalpha((unsigned char)lx->src[s]) || lx->src[s] == '_' ||
        lx->src[s] == '.'))
    return false;
  lx->i++;
  while (lx->i < lx->n) {
    char c = lx->src[lx->i];
    if (isalnum((unsigned char)c) || c == '_' || c == '.')
      lx->i++;
    else
      break;
  }
  *out = &lx->src[s];
  *on = lx->i - s;
  return true;
}

static bool lex_int(Lex *lx, int64_t *out) {
  size_t s = lx->i;
  bool neg = false;
  if (s < lx->n && (lx->src[s] == '-' || lx->src[s] == '+')) {
    neg = lx->src[s] == '-';
    s++;
  }
  size_t i = s;
  int base = 10;
  if (i + 2 <= lx->n && lx->src[i] == '0' &&
      (lx->src[i + 1] == 'x' || lx->src[i + 1] == 'X')) {
    base = 16;
    i += 2;
  }
  if (i >= lx->n || !isxdigit((unsigned char)lx->src[i])) return false;
  long long v = 0;
  for (; i < lx->n; i++) {
    int d;
    char c = lx->src[i];
    if (base == 10) {
      if (!isdigit((unsigned char)c)) break;
      d = c - '0';
    } else {
      if (!isxdigit((unsigned char)c)) break;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = 10 + (c - 'a');
      else if (c >= 'A' && c <= 'F')
        d = 10 + (c - 'A');
      else
        break;
    }
    v = (base == 10) ? v * 10 + d : (v << 4) + d;
  }
  lx->i = i;
  if (neg) v = -v;
  *out = (int64_t)v;
  return true;
}

static bool lex_float(Lex *lx, double *out) {
  size_t s = lx->i;
  bool seen = false;
  bool exp = false;
  if (s < lx->n && (lx->src[s] == '+' || lx->src[s] == '-')) s++;
  size_t i = s;
  while (i < lx->n) {
    char c = lx->src[i];
    if (isdigit((unsigned char)c)) {
      seen = true;
      i++;
      continue;
    }
    if (c == '.') {
      seen = true;
      i++;
      continue;
    }
    if (c == 'e' || c == 'E') {
      exp = true;
      i++;
      if (i < lx->n && (lx->src[i] == '+' || lx->src[i] == '-')) i++;
      continue;
    }
    break;
  }
  if (!seen) return false;
  char tmp[128];
  size_t l = i - lx->i;
  if (l >= sizeof(tmp)) l = sizeof(tmp) - 1;
  memcpy(tmp, lx->src + lx->i, l);
  tmp[l] = '\0';
  char *end = NULL;
  double v = strtod(tmp, &end);
  if (end == tmp) return false;
  lx->i = i;
  *out = v;
  return true;
}

static bool lex_string(Lex *lx, const char **out, size_t *on, char **heap_out) {
  if (lx->i >= lx->n || lx->src[lx->i] != '"') return false;
  lx->i++;
  Buf b = {0};
  while (lx->i < lx->n) {
    char c = lx->src[lx->i++];
    if (c == '"') break;
    if (c == '\\' && lx->i < lx->n) {
      char e = lx->src[lx->i++];
      switch (e) {
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case '"':
          c = '"';
          break;
        case '\\':
          c = '\\';
          break;
        default:
          c = e;
          break;
      }
    }
    buf_u8(&b, (uint8_t)c);
  }
  char *s = (char *)malloc(b.len + 1);
  if (!s) {
    buf_free(&b);
    return false;
  }
  memcpy(s, b.data, b.len);
  s[b.len] = '\0';
  if (heap_out) *heap_out = s;
  if (out) *out = s;
  if (on) *on = b.len;
  buf_free(&b);
  return true;
}

// ───────────────────────── Assembleur ─────────────────────────

typedef struct {
  Buf code;
  SPool pool;
} Asm;

static void asm_free(Asm *a) {
  buf_free(&a->code);
  spool_free(&a->pool);
}

static int op_from_ident(const char *id, size_t n) {
  struct {
    const char *k;
    int v;
  } tbl[] = {{"NOP", OP_NOP},     {"PUSHI", OP_PUSHI},   {"PUSHF", OP_PUSHF},
             {"PUSHS", OP_PUSHS}, {"ADD", OP_ADD},       {"SUB", OP_SUB},
             {"MUL", OP_MUL},     {"DIV", OP_DIV},       {"EQ", OP_EQ},
             {"NEQ", OP_NEQ},     {"LT", OP_LT},         {"GT", OP_GT},
             {"LE", OP_LE},       {"GE", OP_GE},         {"PRINT", OP_PRINT},
             {"POP", OP_POP},     {"STOREG", OP_STOREG}, {"LOADG", OP_LOADG},
             {"CALLN", OP_CALLN}, {"HALT", OP_HALT}};
  for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
    if (strlen(tbl[i].k) == n && memcmp(tbl[i].k, id, n) == 0) return tbl[i].v;
  return -1;
}

static bool assemble(const char *src, size_t n, Buf *out_vlbc) {
  Asm a = {0};
  Lex lx;
  lex_init(&lx, src, n);
  while (1) {
    lex_skip_ws(&lx);
    if (lx.i >= lx.n) break;
    const char *id;
    size_t idn;
    if (!lex_id(&lx, &id, &idn)) {
      fprintf(stderr, "ASM:%d: opcode attendu\n", lx.line);
      asm_free(&a);
      return false;
    }
    int op = op_from_ident(id, idn);
    if (op < 0) {
      fprintf(stderr, "ASM:%d: opcode inconnu '%.*s'\n", lx.line, (int)idn, id);
      asm_free(&a);
      return false;
    }
    buf_u8(&a.code, (uint8_t)op);
    // arguments
    if (op == OP_PUSHI) {
      int64_t v;
      lex_skip_ws(&lx);
      if (!lex_int(&lx, &v)) {
        fprintf(stderr, "ASM:%d: int attendu\n", lx.line);
        asm_free(&a);
        return false;
      }
      buf_u64(&a.code, (uint64_t)v);
    } else if (op == OP_PUSHF) {
      double d;
      lex_skip_ws(&lx);
      if (!lex_float(&lx, &d)) {
        fprintf(stderr, "ASM:%d: float attendu\n", lx.line);
        asm_free(&a);
        return false;
      }
      union {
        double d;
        uint64_t u;
      } u;
      u.d = d;
      buf_u64(&a.code, u.u);
    } else if (op == OP_PUSHS || op == OP_STOREG || op == OP_LOADG ||
               op == OP_CALLN) {
      lex_skip_ws(&lx);
      // String litteral ou ident
      const char *s = NULL;
      size_t sl = 0;
      char *heap = NULL;
      if (lex_string(&lx, &s, &sl, &heap)) {
        // ok
      } else if (lex_id(&lx, &s, &sl)) {
        heap = (char *)malloc(sl + 1);
        if (!heap) {
          asm_free(&a);
          return false;
        }
        memcpy(heap, s, sl);
        heap[sl] = '\0';
        s = heap;
      } else {
        fprintf(stderr, "ASM:%d: ident ou string attendu\n", lx.line);
        asm_free(&a);
        return false;
      }
      int idx = spool_put(&a.pool, s, sl);
      free(heap);
      if (idx < 0) {
        asm_free(&a);
        return false;
      }
      buf_u32(&a.code, (uint32_t)idx);
      if (op == OP_CALLN) {
        lex_skip_ws(&lx);
        int64_t argc = 0;
        if (!lex_int(&lx, &argc)) {
          fprintf(stderr, "ASM:%d: argc entier attendu\n", lx.line);
          asm_free(&a);
          return false;
        }
        if (argc < 0 || argc > 255) {
          fprintf(stderr, "ASM:%d: argc hors plage 0..255\n", lx.line);
          asm_free(&a);
          return false;
        }
        buf_u8(&a.code, (uint8_t)argc);
      }
    } else { /* no extra args */
    }
    // fin de ligne
    while (lx.i < lx.n && lx.src[lx.i] != '\n') {
      if (!isspace((unsigned char)lx.src[lx.i])) {
        fprintf(stderr, "ASM:%d: trailing garbage\n", lx.line);
        asm_free(&a);
        return false;
      }
      lx.i++;
    }
  }

  // Construire VLBC
  Buf vlbc = {0};
  buf_u8(&vlbc, 'V');
  buf_u8(&vlbc, 'L');
  buf_u8(&vlbc, 'B');
  buf_u8(&vlbc, 'C');
  buf_u8(&vlbc, 1);  // version
  buf_u32(&vlbc, (uint32_t)a.pool.len);
  for (size_t i = 0; i < a.pool.len; i++) {
    buf_u32(&vlbc, a.pool.v[i].len);
    buf_bytes(&vlbc, a.pool.v[i].s, a.pool.v[i].len);
  }
  buf_u32(&vlbc, (uint32_t)a.code.len);
  buf_bytes(&vlbc, a.code.data, a.code.len);

  *out_vlbc = vlbc;  // transfert ownership
  buf_free(&a.code);
  spool_free(&a.pool);
  return true;
}

// ───────────────────────── Désassembleur ─────────────────────────

static bool rd_u8(const uint8_t *p, size_t n, size_t *io, uint8_t *out) {
  if (*io + 1 > n) return false;
  *out = p[(*io)++];
  return true;
}
static bool rd_u32(const uint8_t *p, size_t n, size_t *io, uint32_t *out) {
  if (*io + 4 > n) return false;
  uint32_t v = (uint32_t)p[*io] | ((uint32_t)p[*io + 1] << 8) |
               ((uint32_t)p[*io + 2] << 16) | ((uint32_t)p[*io + 3] << 24);
  *io += 4;
  *out = v;
  return true;
}
static bool rd_u64(const uint8_t *p, size_t n, size_t *io, uint64_t *out) {
  if (*io + 8 > n) return false;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)p[*io + i]) << (8 * i);
  *io += 8;
  *out = v;
  return true;
}
static bool rd_f64(const uint8_t *p, size_t n, size_t *io, double *out) {
  if (*io + 8 > n) return false;
  union {
    uint64_t u;
    double d;
  } u;
  rd_u64(p, n, io, &u.u);
  *out = u.d;
  return true;
}

static const char *name_of(int op) {
  switch (op) {
    case OP_NOP:
      return "NOP";
    case OP_PUSHI:
      return "PUSHI";
    case OP_PUSHF:
      return "PUSHF";
    case OP_PUSHS:
      return "PUSHS";
    case OP_ADD:
      return "ADD";
    case OP_SUB:
      return "SUB";
    case OP_MUL:
      return "MUL";
    case OP_DIV:
      return "DIV";
    case OP_EQ:
      "EQ";
    case OP_NEQ:
      "NEQ";
    case OP_LT:
      "LT";
    case OP_GT:
      "GT";
    case OP_LE:
      "LE";
    case OP_GE:
      "GE";
    case OP_PRINT:
      return "PRINT";
    case OP_POP:
      return "POP";
    case OP_STOREG:
      return "STOREG";
    case OP_LOADG:
      return "LOADG";
    case OP_CALLN:
      return "CALLN";
    case OP_HALT:
      return "HALT";
    default:
      return "?";
  }
}

static bool disassemble(const uint8_t *buf, size_t n) {
  if (n < 5 || memcmp(buf, "VLBC", 4) != 0) {
    fprintf(stderr, "VLBC: bad magic\n");
    return false;
  }
  size_t i = 4;
  uint8_t ver;
  if (!rd_u8(buf, n, &i, &ver) || ver != 1) {
    fprintf(stderr, "VLBC: bad ver\n");
    return false;
  }
  uint32_t nstr = 0;
  if (!rd_u32(buf, n, &i, &nstr)) {
    fprintf(stderr, "VLBC: trunc nstr\n");
    return false;
  }
  printf("; VLBC v%u, strings=%u\n", ver, nstr);
  const char **pool = (const char **)calloc(nstr, sizeof(char *));
  for (uint32_t s = 0; s < nstr; s++) {
    uint32_t sl;
    if (!rd_u32(buf, n, &i, &sl) || i + sl > n) {
      fprintf(stderr, "VLBC: trunc str\n");
      free(pool);
      return false;
    }
    printf(".str %u \"%.*s\"\n", s, (int)sl, (const char *)(buf + i));
    char *dup = (char *)malloc(sl + 1);
    memcpy(dup, buf + i, sl);
    dup[sl] = '\0';
    pool[s] = dup;
    i += sl;
  }
  uint32_t code_sz = 0;
  if (!rd_u32(buf, n, &i, &code_sz) || i + code_sz > n) {
    fprintf(stderr, "VLBC: trunc code\n");
    for (uint32_t s = 0; s < nstr; s++) free((void *)pool[s]);
    free(pool);
    return false;
  }
  size_t ip = i;
  size_t end = i + code_sz;
  printf(".code %u bytes\n", code_sz);
  while (ip < end) {
    uint8_t op = 0;
    rd_u8(buf, n, &ip, &op);
    printf("%04zu \t%s", ip - 1 - i, name_of(op));
    switch (op) {
      case OP_PUSHI: {
        uint64_t v;
        rd_u64(buf, n, &ip, &v);
        printf(" \t%" PRId64, (int64_t)v);
      } break;
      case OP_PUSHF: {
        double d;
        rd_f64(buf, n, &ip, &d);
        printf(" \t%g", d);
      } break;
      case OP_PUSHS:
      case OP_STOREG:
      case OP_LOADG: {
        uint32_t si;
        rd_u32(buf, n, &ip, &si);
        printf(" \t%u ; \"%s\"", si, si < nstr ? pool[si] : "<bad>");
      } break;
      case OP_CALLN: {
        uint32_t si;
        uint8_t argc;
        rd_u32(buf, n, &ip, &si);
        rd_u8(buf, n, &ip, &argc);
        printf(" \t%u,%u ; \"%s\"", si, argc, si < nstr ? pool[si] : "<bad>");
      } break;
      default:
        break;
    }
    printf("\n");
  }
  for (uint32_t s = 0; s < nstr; s++) free((void *)pool[s]);
  free(pool);
  return true;
}

// ───────────────────────── Demo source ─────────────────────────

static const char *DEMO_SRC =
    "; Démo: variables globales, natifs et arithmétique\n"
    "PUSHS \"Bonjour VitteLight\"\n"
    "CALLN print 1\n"
    "PUSHI 2\n"
    "PUSHI 40\n"
    "ADD\n"
    "STOREG result\n"
    "LOADG result\n"
    "CALLN print 1\n"
    "PUSHS now_ms\n"
    "CALLN now_ms 0\n"
    "CALLN print 1\n"
    "HALT\n";

// ───────────────────────── Main ─────────────────────────

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage:\n"
          "  %s run  <file.vlbc>\n"
          "  %s asm  <file.vlasm> -o <out.vlbc>\n"
          "  %s dis  <file.vlbc>\n"
          "  %s demo\n",
          argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }
  const char *cmd = argv[1];

  if (strcmp(cmd, "asm") == 0) {
    if (argc < 5 || strcmp(argv[3], "-o") != 0) {
      usage(argv[0]);
      return 2;
    }
    const char *in = argv[2];
    const char *out = argv[4];
    size_t n = 0;
    uint8_t *src = read_entire_file(in, &n);
    if (!src) {
      fprintf(stderr, "lecture échouée: %s\n", in);
      return 3;
    }
    Buf vlbc = {0};
    bool ok = assemble((const char *)src, n, &vlbc);
    free(src);
    if (!ok) {
      fprintf(stderr, "assemblage échoué\n");
      buf_free(&vlbc);
      return 4;
    }
    if (!write_entire_file(out, vlbc.data, vlbc.len)) {
      fprintf(stderr, "écriture échouée: %s\n", out);
      buf_free(&vlbc);
      return 5;
    }
    buf_free(&vlbc);
    return 0;
  }

  if (strcmp(cmd, "dis") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    size_t n = 0;
    uint8_t *buf = read_entire_file(argv[2], &n);
    if (!buf) {
      fprintf(stderr, "lecture échouée: %s\n", argv[2]);
      return 3;
    }
    bool ok = disassemble(buf, n);
    free(buf);
    return ok ? 0 : 4;
  }

  if (strcmp(cmd, "run") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    size_t n = 0;
    uint8_t *buf = read_entire_file(argv[2], &n);
    if (!buf) {
      fprintf(stderr, "lecture échouée: %s\n", argv[2]);
      return 3;
    }
    VL_Context *vm = vl_create_default();
    if (!vm) {
      fprintf(stderr, "vm: alloc\n");
      free(buf);
      return 4;
    }
    VL_Status rc = vl_load_program_from_memory(vm, buf, n);
    free(buf);
    if (rc != VL_OK) {
      fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
      vl_destroy(vm);
      return 5;
    }
    rc = vl_run(vm, 0);
    if (rc != VL_OK) {
      fprintf(stderr, "run: %s\n", vl_last_error(vm)->msg);
      vl_destroy(vm);
      return 6;
    }
    vl_destroy(vm);
    return 0;
  }

  if (strcmp(cmd, "demo") == 0) {
    Buf vlbc = {0};
    bool ok = assemble(DEMO_SRC, strlen(DEMO_SRC), &vlbc);
    if (!ok) {
      fprintf(stderr, "demo: assemblage échoué\n");
      return 7;
    }
    VL_Context *vm = vl_create_default();
    if (!vm) {
      fprintf(stderr, "vm: alloc\n");
      buf_free(&vlbc);
      return 8;
    }
    VL_Status rc = vl_load_program_from_memory(vm, vlbc.data, vlbc.len);
    if (rc != VL_OK) {
      fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
      buf_free(&vlbc);
      vl_destroy(vm);
      return 9;
    }
    rc = vl_run(vm, 0);
    if (rc != VL_OK) {
      fprintf(stderr, "run: %s\n", vl_last_error(vm)->msg);
      buf_free(&vlbc);
      vl_destroy(vm);
      return 10;
    }
    buf_free(&vlbc);
    vl_destroy(vm);
    return 0;
  }

  usage(argv[0]);
  return 1;
}
