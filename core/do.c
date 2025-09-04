// vitte-light/core/do.c
// Outil "do" tout-en-un pour VitteLight
//  - run/asm/dis/inspect/trace/bench
//  - REPL assembleur minimal
//  - set/get globals, hexdump
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic \
//      core/api.c core/ctype.c core/ctype_ext.c core/debug.c core/do.c \
//      -o vitl-do
//
// Remarque: ce fichier réimplémente un assembleur minimal (comme core/code.c)
// pour ne dépendre d'aucune API externe.

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "ctype.h"
#include "debug.h"

// ───────────────────────── Opcodes (doivent matcher la VM)
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

static const char *op_name(uint8_t op) {
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
      return "EQ";
    case OP_NEQ:
      return "NEQ";
    case OP_LT:
      return "LT";
    case OP_GT:
      return "GT";
    case OP_LE:
      return "LE";
    case OP_GE:
      return "GE";
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

// ───────────────────────── Small buffer utils ─────────────────────────

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
  b->data[b->len++] = (uint8_t)v;
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

// ───────────────────────── Lexer ASM ─────────────────────────

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
    if (lx->i < lx->n && (lx->src[lx->i] == '#' || lx->src[lx->i] == ';' ||
                          (lx->src[lx->i] == '/' && lx->i + 1 < lx->n &&
                           lx->src[lx->i + 1] == '/'))) {
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

static bool assemble_src(const char *src, size_t n, Buf *out_vlbc) {
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
    }
    // consommer jusqu’à fin de ligne
    while (lx.i < lx.n && lx.src[lx.i] != '\n') {
      if (!isspace((unsigned char)lx.src[lx.i])) {
        fprintf(stderr, "ASM:%d: trailing garbage\n", lx.line);
        asm_free(&a);
        return false;
      }
      lx.i++;
    }
  }
  // VLBC
  Buf vlbc = {0};
  buf_u8(&vlbc, 'V');
  buf_u8(&vlbc, 'L');
  buf_u8(&vlbc, 'B');
  buf_u8(&vlbc, 'C');
  buf_u8(&vlbc, 1);
  buf_u32(&vlbc, (uint32_t)a.pool.len);
  for (size_t i = 0; i < a.pool.len; i++) {
    buf_u32(&vlbc, a.pool.v[i].len);
    buf_bytes(&vlbc, a.pool.v[i].s, a.pool.v[i].len);
  }
  buf_u32(&vlbc, (uint32_t)a.code.len);
  buf_bytes(&vlbc, a.code.data, a.code.len);
  *out_vlbc = vlbc;
  buf_free(&a.code);
  spool_free(&a.pool);
  return true;
}

// ───────────────────────── VM helpers ─────────────────────────

static VL_Context *make_vm_from_env(void) {
  VL_Config cfg = {0};
  const char *sc = getenv("VL_STACK_CAP");
  if (sc) {
    long v = strtol(sc, NULL, 10);
    if (v > 0) cfg.stack_cap = (size_t)v;
  }
  VL_Context *vm = vl_create(&cfg);
  if (!vm) return NULL;
  vl_register_native(vm, "now_ms", NULL,
                     NULL);  // sera écrasé par create_default si utilisée
  // on préfère l’usine fournie pour print/now_ms
  vl_destroy(vm);
  vm = vl_create_default();
  return vm;
}

static int run_vlbc(VL_Context *vm, const uint8_t *buf, size_t n, int trace,
                    uint64_t max_steps) {
  VL_Status rc = vl_load_program_from_memory(vm, buf, n);
  if (rc != VL_OK) {
    fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
    return 2;
  }
  if (trace) {
    rc = vl_debug_run_trace(vm, max_steps, stdout);
  } else {
    rc = vl_run(vm, max_steps);
  }
  if (rc != VL_OK) {
    fprintf(stderr, "run: %s\n", vl_last_error(vm)->msg);
    return 3;
  }
  return 0;
}

static double now_ms(void) {
#if defined(_WIN32)
  return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

// ───────────────────────── REPL ─────────────────────────
static void repl(void) {
  puts("VitteLight REPL (asm). Tapez .help pour l’aide.");
  VL_Context *vm = make_vm_from_env();
  if (!vm) {
    fputs("vm alloc fail\n", stderr);
    return;
  }
  char *line = NULL;
  size_t cap = 0;
  Buf prog = {0};
  for (;;) {
    fputs("> ", stdout);
    fflush(stdout);
    ssize_t n = getline(&line, &cap, stdin);
    if (n <= 0) break;
    if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
    if (line[0] == '.') {
      if (strcmp(line, ".help") == 0) {
        puts(
            ".help               – aide\n.quit               – quitter\n.load "
            "<f.vlbc>      – charger et exécuter\n.asm  <f.vlasm>     – "
            "assemble et exécute\n.eval <asm...>      – assemble la ligne et "
            "exécute\n.trace <f.vlbc>     – exécuter en trace\n.stack          "
            "    – dump de stack\n.globals            – dump des globaux\n.set "
            "<k> <v>        – set global string\n.get <k>            – get "
            "global\n.bench <f> <n>      – exécuter f n fois\n.dis  <f.vlbc>   "
            "   – désassembler\n.inspect <f.vlbc>   – inspecter VLBC\n.hex <f> "
            "           – hexdump");
      } else if (strcmp(line, ".quit") == 0) {
        break;
      } else if (strncmp(line, ".load ", 7) == 0) {
        size_t ln = 0;
        uint8_t *buf = read_entire_file(line + 7, &ln);
        if (!buf) {
          perror("load");
          continue;
        }
        int rc = run_vlbc(vm, buf, ln, 0, 0);
        free(buf);
        if (rc) continue;
      } else if (strncmp(line, ".trace ", 8) == 0) {
        size_t ln = 0;
        uint8_t *buf = read_entire_file(line + 8, &ln);
        if (!buf) {
          perror("trace");
          continue;
        }
        int rc = run_vlbc(vm, buf, ln, 1, 0);
        free(buf);
        if (rc) continue;
      } else if (strncmp(line, ".asm ", 6) == 0) {
        size_t sn = 0;
        uint8_t *src = read_entire_file(line + 6, &sn);
        if (!src) {
          perror("asm");
          continue;
        }
        Buf vlbc = {0};
        if (!assemble_src((const char *)src, sn, &vlbc)) {
          free(src);
          continue;
        }
        free(src);
        int rc = run_vlbc(vm, vlbc.data, vlbc.len, 0, 0);
        buf_free(&vlbc);
        if (rc) continue;
      } else if (strncmp(line, ".eval ", 7) == 0) {
        const char *src = line + 7;
        Buf vlbc = {0};
        if (!assemble_src(src, strlen(src), &vlbc)) continue;
        int rc = run_vlbc(vm, vlbc.data, vlbc.len, 0, 0);
        buf_free(&vlbc);
        if (rc) continue;
      } else if (strcmp(line, ".stack") == 0) {
        vl_debug_dump_stack(vm, stdout);
      } else if (strcmp(line, ".globals") == 0) {
        vl_debug_dump_globals(vm, stdout);
      } else if (strncmp(line, ".set ", 6) == 0) {
        char k[128];
        char v[256];
        if (sscanf(line + 6, "%127s %255[^\n]", k, v) == 2) {
          VL_Value sv = vl_make_str(vm, v);
          vl_set_global(vm, k, sv);
        }
      } else if (strncmp(line, ".get ", 6) == 0) {
        char k[128];
        if (sscanf(line + 6, "%127s", k) == 1) {
          VL_Value out;
          if (vl_get_global(vm, k, &out) == VL_OK) {
            vl_value_print(&out, stdout);
            fputc('\n', stdout);
          } else
            puts("<not found>");
        }
      } else if (strncmp(line, ".bench ", 8) == 0) {
        char path[512];
        long it = 0;
        if (sscanf(line + 8, "%511s %ld", path, &it) == 2 && it > 0) {
          size_t n = 0;
          uint8_t *buf = read_entire_file(path, &n);
          if (!buf) {
            perror("bench");
            continue;
          }
          double t0 = now_ms();
          for (long i = 0; i < it; i++) {
            vm->ip = 0;
            vm->sp = 0;
            if (vl_load_program_from_memory(vm, buf, n) != VL_OK) {
              fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
              break;
            }
            if (vl_run(vm, 0) != VL_OK) {
              fprintf(stderr, "run: %s\n", vl_last_error(vm)->msg);
              break;
            }
          }
          double dt = now_ms() - t0;
          printf("%ld runs in %.3f ms  =>  %.3f ms/run\n", it, dt,
                 dt / (double)it);
          free(buf);
        }
      } else if (strncmp(line, ".dis ", 5) == 0) {
        size_t n = 0;
        uint8_t *buf = read_entire_file(line + 5, &n);
        if (!buf) {
          perror("dis");
          continue;
        }
        vl_debug_disassemble(buf, n, stdout);
        free(buf);
      } else if (strncmp(line, ".inspect ", 9) == 0) {
        size_t n = 0;
        uint8_t *buf = read_entire_file(line + 9, &n);
        if (!buf) {
          perror("inspect");
          continue;
        }
        vl_debug_vlbc_inspect(buf, n, stdout);
        free(buf);
      } else if (strncmp(line, ".hex ", 5) == 0) {
        size_t n = 0;
        uint8_t *buf = read_entire_file(line + 5, &n);
        if (!buf) {
          perror("hex");
          continue;
        }
        vl_debug_hexdump(buf, n, stdout);
        free(buf);
      } else {
        puts("commande inconnue. .help pour l’aide");
      }
      continue;
    }
    // mode: la ligne est de l'ASM
    Buf vlbc = {0};
    if (!assemble_src(line, strlen(line), &vlbc)) continue;
    int rc = run_vlbc(vm, vlbc.data, vlbc.len, 0, 0);
    buf_free(&vlbc);
    if (rc) continue;
  }
  free(line);
  vl_destroy(vm);
}

// ───────────────────────── Commandes de haut niveau ─────────────────────────
static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage:\n"
          "  %s run <prog.vlbc> [--trace] [--max-steps N] [--dump-stack] "
          "[--dump-globals]\n"
          "  %s asm <src.vlasm> -o <out.vlbc>\n"
          "  %s dis <prog.vlbc>\n"
          "  %s inspect <prog.vlbc>\n"
          "  %s bench <prog.vlbc> -n N\n"
          "  %s eval \"ASM one-liner\" [--trace]\n"
          "  %s repl\n"
          "  %s hex <file>\n",
          argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }
  const char *cmd = argv[1];

  if (strcmp(cmd, "repl") == 0) {
    repl();
    return 0;
  }

  if (strcmp(cmd, "asm") == 0) {
    if (argc < 5 || strcmp(argv[3], "-o") != 0) {
      usage(argv[0]);
      return 2;
    }
    size_t sn = 0;
    uint8_t *src = read_entire_file(argv[2], &sn);
    if (!src) {
      perror("asm");
      return 3;
    }
    Buf vlbc = {0};
    bool ok = assemble_src((const char *)src, sn, &vlbc);
    free(src);
    if (!ok) {
      return 4;
    }
    if (!write_entire_file(argv[4], vlbc.data, vlbc.len)) {
      perror("write");
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
      perror("dis");
      return 3;
    }
    bool ok = vl_debug_disassemble(buf, n, stdout);
    free(buf);
    return ok ? 0 : 4;
  }

  if (strcmp(cmd, "inspect") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    size_t n = 0;
    uint8_t *buf = read_entire_file(argv[2], &n);
    if (!buf) {
      perror("inspect");
      return 3;
    }
    bool ok = vl_debug_vlbc_inspect(buf, n, stdout);
    free(buf);
    return ok ? 0 : 4;
  }

  if (strcmp(cmd, "hex") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    size_t n = 0;
    uint8_t *buf = read_entire_file(argv[2], &n);
    if (!buf) {
      perror("hex");
      return 3;
    }
    vl_debug_hexdump(buf, n, stdout);
    free(buf);
    return 0;
  }

  if (strcmp(cmd, "bench") == 0) {
    if (argc < 5 || strcmp(argv[3], "-n") != 0) {
      usage(argv[0]);
      return 2;
    }
    long it = strtol(argv[4], NULL, 10);
    if (it <= 0) {
      fputs("-n doit être >0\n", stderr);
      return 2;
    }
    size_t n = 0;
    uint8_t *buf = read_entire_file(argv[2], &n);
    if (!buf) {
      perror("bench");
      return 3;
    }
    VL_Context *vm = make_vm_from_env();
    if (!vm) {
      fputs("vm alloc\n", stderr);
      free(buf);
      return 4;
    }
    double t0 = now_ms();
    for (long i = 0; i < it; i++) {
      if (vl_load_program_from_memory(vm, buf, n) != VL_OK) {
        fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
        break;
      }
      if (vl_run(vm, 0) != VL_OK) {
        fprintf(stderr, "run: %s\n", vl_last_error(vm)->msg);
        break;
      }
    }
    double dt = now_ms() - t0;
    printf("%ld runs in %.3f ms  =>  %.3f ms/run\n", it, dt, dt / (double)it);
    vl_destroy(vm);
    free(buf);
    return 0;
  }

  if (strcmp(cmd, "eval") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    int trace = 0;
    for (int i = 3; i < argc; i++) {
      if (strcmp(argv[i], "--trace") == 0) trace = 1;
    }
    VL_Context *vm = make_vm_from_env();
    if (!vm) {
      fputs("vm alloc\n", stderr);
      return 4;
    }
    Buf vlbc = {0};
    if (!assemble_src(argv[2], strlen(argv[2]), &vlbc)) {
      vl_destroy(vm);
      return 5;
    }
    int rc = run_vlbc(vm, vlbc.data, vlbc.len, trace, 0);
    buf_free(&vlbc);
    vl_destroy(vm);
    return rc;
  }

  if (strcmp(cmd, "run") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    int trace = 0, dump_stack = 0, dump_globals = 0;
    uint64_t max_steps = 0;
    for (int i = 3; i < argc; i++) {
      if (strcmp(argv[i], "--trace") == 0)
        trace = 1;
      else if (strcmp(argv[i], "--dump-stack") == 0)
        dump_stack = 1;
      else if (strcmp(argv[i], "--dump-globals") == 0)
        dump_globals = 1;
      else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
        max_steps = strtoull(argv[++i], NULL, 10);
      }
    }
    size_t n = 0;
    uint8_t *buf = read_entire_file(argv[2], &n);
    if (!buf) {
      perror("run");
      return 3;
    }
    VL_Context *vm = make_vm_from_env();
    if (!vm) {
      fputs("vm alloc\n", stderr);
      free(buf);
      return 4;
    }
    int rc = run_vlbc(vm, buf, n, trace, max_steps);
    if (dump_stack) vl_debug_dump_stack(vm, stdout);
    if (dump_globals) vl_debug_dump_globals(vm, stdout);
    vl_destroy(vm);
    free(buf);
    return rc;
  }

  usage(argv[0]);
  return 1;
}
