// vitte-light/core/debug.c
// Outils de debug et d’inspection pour VitteLight.
// - hexdump et inspecteur VLBC
// - trace pas-à-pas de la VM
// - dump de stack et de globaux
// - mini désassembleur lisible
// - timers/profilage rudimentaires
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c debug.c
// Link avec: api.c, ctype.c, ctype_ext.c
//
// Note: ce module redéclare quelques structures internes pour l’inspection.
// Il doit rester synchronisé avec api.c.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "ctype.h"

// ───────────────────────── Opcodes (doivent matcher api.c/code.c)
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

// ───────────────────────── Redéclarations internes (sync api.c)
// ─────────────────────────

typedef struct VL_String {
  uint32_t hash, len;
  char data[];
} VL_String;

typedef struct {
  VL_String **keys;
  VL_Value *vals;
  size_t cap, len, tomb;
} VL_Map;

typedef struct {
  VL_NativeFn fn;
  void *ud;
} VL_Native;

struct VL_Context {
  // alloc/log
  void *ralloc;
  void *alloc_ud;
  void *log;
  void *log_ud;  // types masqués, non utilisés ici
  VL_Error last_error;
  // VM
  uint8_t *bc;
  size_t bc_len;
  size_t ip;
  VL_Value *stack;
  size_t sp;
  size_t stack_cap;
  VL_String **kstr;
  size_t kstr_len;
  VL_Map globals;
  VL_Map natives;
};

// ───────────────────────── Utils temps ─────────────────────────
static double now_ms(void) {
#if defined(_WIN32)
  // Fallback simple
  return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

// ───────────────────────── Hexdump ─────────────────────────
void vl_debug_hexdump(const void *data, size_t len, FILE *out) {
  if (!out) out = stdout;
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < len; i += 16) {
    fprintf(out, "%08zx  ", i);
    for (size_t j = 0; j < 16; j++) {
      if (i + j < len)
        fprintf(out, "%02x ", p[i + j]);
      else
        fprintf(out, "   ");
      if (j == 7) fputc(' ', out);
    }
    fputc(' ', out);
    for (size_t j = 0; j < 16 && i + j < len; j++) {
      unsigned char c = p[i + j];
      fputc((c >= 32 && c < 127) ? c : '.', out);
    }
    fputc('\n', out);
  }
}

// ───────────────────────── Helpers lecture VLBC ─────────────────────────
static int rd_u8(const uint8_t *p, size_t n, size_t *io, uint8_t *out) {
  if (*io + 1 > n) return 0;
  *out = p[(*io)++];
  return 1;
}
static int rd_u32(const uint8_t *p, size_t n, size_t *io, uint32_t *out) {
  if (*io + 4 > n) return 0;
  uint32_t v = (uint32_t)p[*io] | ((uint32_t)p[*io + 1] << 8) |
               ((uint32_t)p[*io + 2] << 16) | ((uint32_t)p[*io + 3] << 24);
  *io += 4;
  *out = v;
  return 1;
}
static int rd_u64(const uint8_t *p, size_t n, size_t *io, uint64_t *out) {
  if (*io + 8 > n) return 0;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)p[*io + i]) << (8 * i);
  *io += 8;
  *out = v;
  return 1;
}
static int rd_f64(const uint8_t *p, size_t n, size_t *io, double *out) {
  if (*io + 8 > n) return 0;
  union {
    uint64_t u;
    double d;
  } u;
  if (!rd_u64(p, n, io, &u.u)) return 0;
  *out = u.d;
  return 1;
}

// ───────────────────────── Inspecteur VLBC ─────────────────────────
// Affiche l’entête, le pool de chaînes et le code size.
bool vl_debug_vlbc_inspect(const uint8_t *buf, size_t n, FILE *out) {
  if (!out) out = stdout;
  if (!buf || n < 5) {
    fprintf(out, "VLBC: buffer trop court\n");
    return false;
  }
  if (memcmp(buf, "VLBC", 4) != 0) {
    fprintf(out, "VLBC: mauvaise magic\n");
    return false;
  }
  size_t i = 4;
  uint8_t ver = 0;
  if (!rd_u8(buf, n, &i, &ver)) {
    fprintf(out, "VLBC: tronc ver\n");
    return false;
  }
  fprintf(out, "> VLBC v%u\n", ver);
  uint32_t nstr = 0;
  if (!rd_u32(buf, n, &i, &nstr)) {
    fprintf(out, "VLBC: tronc nstr\n");
    return false;
  }
  fprintf(out, "  strings=%u\n", nstr);
  for (uint32_t s = 0; s < nstr; s++) {
    uint32_t sl = 0;
    if (!rd_u32(buf, n, &i, &sl) || i + sl > n) {
      fprintf(out, "VLBC: tronc str[%u]\n", s);
      return false;
    }
    fprintf(out, "  [%u] \"%.*s\"\n", s, (int)sl, (const char *)(buf + i));
    i += sl;
  }
  uint32_t code_sz = 0;
  if (!rd_u32(buf, n, &i, &code_sz) || i + code_sz > n) {
    fprintf(out, "VLBC: tronc code\n");
    return false;
  }
  fprintf(out, "  code=%u bytes\n", code_sz);
  return true;
}

// ───────────────────────── Désassembleur minimal ─────────────────────────
bool vl_debug_disassemble(const uint8_t *buf, size_t n, FILE *out) {
  if (!out) out = stdout;
  if (!buf || n < 5 || memcmp(buf, "VLBC", 4) != 0) {
    fprintf(out, "VLBC: magic invalide\n");
    return false;
  }
  size_t i = 4;
  uint8_t ver = 0;
  if (!rd_u8(buf, n, &i, &ver) || ver != 1) {
    fprintf(out, "VLBC: ver invalide\n");
    return false;
  }
  uint32_t nstr = 0;
  if (!rd_u32(buf, n, &i, &nstr)) {
    fprintf(out, "VLBC: tronc nstr\n");
    return false;
  }
  const char **pool = (const char **)calloc(nstr, sizeof(char *));
  if (!pool) return false;
  for (uint32_t s = 0; s < nstr; s++) {
    uint32_t sl = 0;
    if (!rd_u32(buf, n, &i, &sl) || i + sl > n) {
      fprintf(out, "VLBC: tronc str\n");
      goto fail;
    }
    char *dup = (char *)malloc(sl + 1);
    if (!dup) goto fail;
    memcpy(dup, buf + i, sl);
    dup[sl] = '\0';
    pool[s] = dup;
    i += sl;
  }
  uint32_t code_sz = 0;
  if (!rd_u32(buf, n, &i, &code_sz) || i + code_sz > n) {
    fprintf(out, "VLBC: tronc code\n");
    goto fail;
  }
  size_t ip = i, end = i + code_sz;
  fprintf(out, "; disassembly (%u bytes)\n", code_sz);
  while (ip < end) {
    uint8_t op = 0;
    rd_u8(buf, n, &ip, &op);
    fprintf(out, "%04zu\t%s", ip - 1 - i, op_name(op));
    switch (op) {
      case OP_PUSHI: {
        uint64_t v = 0;
        rd_u64(buf, n, &ip, &v);
        fprintf(out, "\t%" PRId64, (int64_t)v);
      } break;
      case OP_PUSHF: {
        double d = 0;
        rd_f64(buf, n, &ip, &d);
        fprintf(out, "\t%g", d);
      } break;
      case OP_PUSHS:
      case OP_STOREG:
      case OP_LOADG: {
        uint32_t si = 0;
        rd_u32(buf, n, &ip, &si);
        fprintf(out, "\t%u ; \"%s\"", si, si < nstr ? pool[si] : "<bad>");
      } break;
      case OP_CALLN: {
        uint32_t si = 0;
        uint8_t argc = 0;
        rd_u32(buf, n, &ip, &si);
        rd_u8(buf, n, &ip, &argc);
        fprintf(out, "\t%u,%u ; \"%s\"", si, argc,
                si < nstr ? pool[si] : "<bad>");
      } break;
      default:
        break;
    }
    fputc('\n', out);
  }
  for (uint32_t s = 0; s < nstr; s++) free((void *)pool[s]);
  free(pool);
  return true;
fail:
  for (uint32_t s = 0; s < nstr; s++) free((void *)pool[s]);
  free(pool);
  return false;
}

// ───────────────────────── Dump stack / globaux ─────────────────────────
void vl_debug_dump_stack(struct VL_Context *ctx, FILE *out) {
  if (!out) out = stdout;
  if (!ctx) {
    fprintf(out, "<no ctx>\n");
    return;
  }
  fprintf(out, "-- stack sp=%zu cap=%zu --\n", ctx->sp, ctx->stack_cap);
  for (size_t i = 0; i < ctx->sp; i++) {
    fprintf(out, "[%03zu] ", i);
    vl_value_print(&ctx->stack[i], out);
    fputc('\n', out);
  }
}

void vl_debug_dump_globals(struct VL_Context *ctx, FILE *out) {
  if (!out) out = stdout;
  if (!ctx) {
    fprintf(out, "<no ctx>\n");
    return;
  }
  fprintf(out, "-- globals len=%zu cap=%zu --\n", ctx->globals.len,
          ctx->globals.cap);
  for (size_t i = 0; i < ctx->globals.cap; i++) {
    VL_String *k = ctx->globals.keys ? ctx->globals.keys[i] : NULL;
    if (k && k != (VL_String *)(uintptr_t)1) {
      VL_Value v = ctx->globals.vals[i];
      fprintf(out, "[%03zu] %.*s = ", i, (int)k->len, k->data);
      vl_value_print(&v, out);
      fputc('\n', out);
    }
  }
}

// ───────────────────────── Trace exécution ─────────────────────────
// Exécute en traçant instruction, IP et top de pile. S’arrête sur HALT ou
// erreur.
VL_Status vl_debug_run_trace(struct VL_Context *ctx, uint64_t max_steps,
                             FILE *out) {
  if (!out) out = stdout;
  if (!ctx) return VL_ERR_BAD_ARG;
  size_t start_ip = ctx->ip;
  double t0 = now_ms();
  uint64_t steps = 0;
  VL_Status rc = VL_OK;
  int halted = 0;
  fprintf(out, "== TRACE: ip=%zu, steps<=%" PRIu64 " ==\n", (size_t)ctx->ip,
          max_steps);
  while (1) {
    if (ctx->ip >= ctx->bc_len) {
      fprintf(out, "ip past code\n");
      rc = VL_ERR_BAD_BYTECODE;
      break;
    }
    uint8_t op = ctx->bc[ctx->ip];
    fprintf(out, "%06zu  %s\tsp=%zu  top=", (size_t)ctx->ip, op_name(op),
            ctx->sp);
    if (ctx->sp > 0)
      vl_value_print(&ctx->stack[ctx->sp - 1], out);
    else
      fprintf(out, "<empty>");
    fputc('\n', out);
    rc = vl_step(ctx);
    if (rc != VL_OK) {
      // Check if it was HALT
      if (op == OP_HALT) {
        halted = 1;
        rc = VL_OK;
      }
      break;
    }
    steps++;
    if (op == OP_HALT) {
      halted = 1;
      break;
    }
    if (max_steps && steps >= max_steps) break;
  }
  double dt = now_ms() - t0;
  fprintf(out,
          "== END: rc=%d halted=%d steps=%" PRIu64
          " ip:%zu→%zu time=%.3f ms ==\n",
          rc, halted, steps, (size_t)start_ip, (size_t)ctx->ip, dt);
  if (rc != VL_OK && vl_last_error(ctx) && vl_last_error(ctx)->msg[0]) {
    fprintf(out, "error: %s\n", vl_last_error(ctx)->msg);
  }
  return rc;
}

// ───────────────────────── Aides d’assertion ─────────────────────────
int vl_debug_expect_true(int cond, const char *expr, const char *file,
                         int line) {
  if (!cond) {
    fprintf(stderr, "ASSERT FAIL at %s:%d: %s\n", file, line, expr);
    return 0;
  }
  return 1;
}

#define VL_EXPECT(x)                                        \
  do {                                                      \
    if (!vl_debug_expect_true((x), #x, __FILE__, __LINE__)) \
      return VL_ERR_RUNTIME;                                \
  } while (0)

// ───────────────────────── Test autonome ─────────────────────────
#ifdef VL_DEBUG_TEST_MAIN
int main(void) {
  // Programme simple: print("dbg"), 1+2 -> print, HALT
  uint8_t buf[256];
  size_t o = 0;
#define EMIT8(x)             \
  do {                       \
    buf[o++] = (uint8_t)(x); \
  } while (0)
#define EMIT32(x)               \
  do {                          \
    uint32_t _ = (uint32_t)(x); \
    memcpy(buf + o, &_, 4);     \
    o += 4;                     \
  } while (0)
#define EMIT64(x)               \
  do {                          \
    uint64_t _ = (uint64_t)(x); \
    memcpy(buf + o, &_, 8);     \
    o += 8;                     \
  } while (0)

  EMIT8('V');
  EMIT8('L');
  EMIT8('B');
  EMIT8('C');
  EMIT8(1);
  EMIT32(2);
  const char *s0 = "dbg";
  EMIT32((uint32_t)strlen(s0));
  memcpy(buf + o, s0, strlen(s0));
  o += strlen(s0);
  const char *s1 = "print";
  EMIT32((uint32_t)strlen(s1));
  memcpy(buf + o, s1, strlen(s1));
  o += strlen(s1);
  size_t pos = o;
  EMIT32(0);
  size_t cs = o;
  EMIT8(OP_PUSHS);
  EMIT32(0);
  EMIT8(OP_CALLN);
  EMIT32(1);
  EMIT8(1);
  EMIT8(OP_PUSHI);
  EMIT64(1);
  EMIT8(OP_PUSHI);
  EMIT64(2);
  EMIT8(OP_ADD);
  EMIT8(OP_CALLN);
  EMIT32(1);
  EMIT8(1);
  EMIT8(OP_HALT);
  uint32_t code_sz = (uint32_t)(o - cs);
  memcpy(buf + pos, &code_sz, 4);

  VL_Context *vm = vl_create_default();
  if (!vm) {
    fprintf(stderr, "vm alloc fail\n");
    return 1;
  }

  VL_Status rc = vl_load_program_from_memory(vm, buf, o);
  if (rc != VL_OK) {
    fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
    return 2;
  }

  puts("-- INSPECT --");
  vl_debug_vlbc_inspect(buf, o, stdout);
  puts("-- DISASM  --");
  vl_debug_disassemble(buf, o, stdout);
  puts("-- TRACE   --");
  rc = vl_debug_run_trace(vm, 0, stdout);
  if (rc != VL_OK) {
    fprintf(stderr, "trace rc=%d: %s\n", rc, vl_last_error(vm)->msg);
  }

  puts("-- STACK   --");
  vl_debug_dump_stack(vm, stdout);
  puts("-- GLOBALS --");
  vl_debug_dump_globals(vm, stdout);

  vl_destroy(vm);
  return 0;
}
#endif
