// vitte-light/core/dump.c
// Dump structuré de la VM et du bytecode VitteLight (JSON et texte).
// Fournit un export JSON complet de l'état: registres VM, pile, globaux,
// constantes (pool de chaînes), bytecode brut et désassemblage.
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/dump.c
// Link avec: api.c, ctype.c, ctype_ext.c (et optionnellement debug.c pour texte
// étendu)
//
// API suggérée (pas de header dédié: déclarations ici; créez dump.h si voulu):
//   VL_Status vl_dump_context_json(struct VL_Context *ctx, int flags, char
//   **out_json, size_t *out_len); void      vl_dump_context_text(struct
//   VL_Context *ctx, int flags, FILE *out); VL_Status vl_dump_vlbc_json(const
//   uint8_t *buf, size_t n, int flags, char **out_json, size_t *out_len); bool
//   vl_dump_write_file(const char *path, const char *json, size_t n);
//
// Flags (bitmask):
//   VLD_STATE=1, VLD_STACK=2, VLD_GLOBALS=4, VLD_CONSTS=8, VLD_BYTECODE=16,
//   VLD_DISASM=32, VLD_HEX=64 VLD_ALL = tout

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"

// ───────────────────────── Opcodes (doivent matcher api.c)
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
  void *ralloc;
  void *alloc_ud;
  void *log;
  void *log_ud;  // opaque
  VL_Error last_error;
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

// ───────────────────────── Flags ─────────────────────────
#define VLD_STATE 0x01
#define VLD_STACK 0x02
#define VLD_GLOBALS 0x04
#define VLD_CONSTS 0x08
#define VLD_BYTECODE 0x10
#define VLD_DISASM 0x20
#define VLD_HEX 0x40
#define VLD_ALL \
  (VLD_STATE | VLD_STACK | VLD_GLOBALS | VLD_CONSTS | VLD_BYTECODE | VLD_DISASM)

// ───────────────────────── JSON buffer util ─────────────────────────

typedef struct {
  char *d;
  size_t n;
  size_t cap;
  int err;
} JBuf;
static void jb_free(JBuf *b) {
  if (!b) return;
  free(b->d);
  b->d = NULL;
  b->n = b->cap = 0;
  b->err = 0;
}
static int jb_res(JBuf *b, size_t add) {
  if (b->err) return 0;
  size_t need = b->n + add + 1;
  if (need <= b->cap) return 1;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < need) cap *= 2;
  char *p = (char *)realloc(b->d, cap);
  if (!p) {
    b->err = 1;
    return 0;
  }
  b->d = p;
  b->cap = cap;
  return 1;
}
static int jb_put(JBuf *b, const char *s, size_t m) {
  if (!jb_res(b, m)) return 0;
  memcpy(b->d + b->n, s, m);
  b->n += m;
  b->d[b->n] = '\0';
  return 1;
}
static int jb_puts(JBuf *b, const char *s) { return jb_put(b, s, strlen(s)); }
static int jb_printf(JBuf *b, const char *fmt, ...) {
  if (b->err) return 0;
  va_list ap;
  va_start(ap, fmt);
  char tmp[512];
  int k = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (k < 0) {
    b->err = 1;
    return 0;
  }
  if (!jb_res(b, (size_t)k)) return 0;
  memcpy(b->d + b->n, tmp, (size_t)k);
  b->n += (size_t)k;
  b->d[b->n] = '\0';
  return 1;
}

static void json_escape_str(JBuf *b, const char *s, size_t n) {
  jb_puts(b, "\"");
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':
        jb_puts(b, "\\\"");
        break;
      case '\\':
        jb_puts(b, "\\\\");
        break;
      case '\n':
        jb_puts(b, "\\n");
        break;
      case '\r':
        jb_puts(b, "\\r");
        break;
      case '\t':
        jb_puts(b, "\\t");
        break;
      default:
        if (c < 0x20) {
          jb_printf(b, "\\u%04x", c);
        } else {
          jb_res(b, 1);
          b->d[b->n++] = (char)c;
          b->d[b->n] = '\0';
        }
    }
  }
  jb_puts(b, "\"");
}

// ───────────────────────── VLBC readers ─────────────────────────
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

// ───────────────────────── JSON helpers pour VL_Value
// ─────────────────────────
static void json_value(JBuf *b, const VL_Value *v) {
  char tmp[256];
  size_t k = vl_value_to_json(v, tmp, sizeof(tmp));
  if (k >= sizeof(tmp)) k = sizeof(tmp) - 1;
  jb_put(b, tmp, k);
}

// ───────────────────────── Dump JSON: contexte VM ─────────────────────────
VL_Status vl_dump_context_json(struct VL_Context *ctx, int flags,
                               char **out_json, size_t *out_len) {
  if (!ctx || !out_json) return VL_ERR_BAD_ARG;
  if (flags == 0) flags = VLD_ALL;
  JBuf b = {0};
  jb_puts(&b, "{");
  int first = 1;
#define SEP()                     \
  do {                            \
    if (!first) jb_puts(&b, ","); \
    first = 0;                    \
  } while (0)

  if (flags & VLD_STATE) {
    SEP();
    jb_puts(&b, "\"state\":{");
    jb_printf(&b, "\"ip\":%zu,\"bc_len\":%zu,\"sp\":%zu,\"stack_cap\":%zu",
              (size_t)ctx->ip, (size_t)ctx->bc_len, (size_t)ctx->sp,
              (size_t)ctx->stack_cap);
    jb_puts(&b, "}");
  }

  if ((flags & VLD_CONSTS) && ctx->kstr && ctx->kstr_len) {
    SEP();
    jb_puts(&b, "\"consts\":[");
    for (size_t i = 0; i < ctx->kstr_len; i++) {
      if (i) jb_puts(&b, ",");
      VL_String *s = ctx->kstr[i];
      if (s) {
        json_escape_str(&b, s->data, s->len);
      } else
        jb_puts(&b, "null");
    }
    jb_puts(&b, "]");
  }

  if (flags & VLD_STACK) {
    SEP();
    jb_puts(&b, "\"stack\":[");
    for (size_t i = 0; i < ctx->sp; i++) {
      if (i) jb_puts(&b, ",");
      json_value(&b, &ctx->stack[i]);
    }
    jb_puts(&b, "]");
  }

  if ((flags & VLD_GLOBALS) && ctx->globals.cap) {
    SEP();
    jb_puts(&b, "\"globals\":{");
    int gfirst = 1;
    for (size_t i = 0; i < ctx->globals.cap; i++) {
      VL_String *k = ctx->globals.keys ? ctx->globals.keys[i] : NULL;
      if (!k || k == (VL_String *)(uintptr_t)1) continue;
      if (!gfirst) jb_puts(&b, ",");
      gfirst = 0;
      json_escape_str(&b, k->data, k->len);
      jb_puts(&b, ":");
      VL_Value v = ctx->globals.vals[i];
      json_value(&b, &v);
    }
    jb_puts(&b, "}");
  }

  if ((flags & VLD_BYTECODE) && ctx->bc && ctx->bc_len) {
    SEP();
    jb_puts(&b, "\"bytecode\":{");
    jb_printf(&b, "\"size\":%zu", (size_t)ctx->bc_len);
    if (flags & VLD_HEX) {
      jb_puts(&b, ",\"hex\":\"");
      for (size_t i = 0; i < ctx->bc_len; i++) {
        jb_printf(&b, "%02x", ctx->bc[i]);
      }
      jb_puts(&b, "\"");
    }
    jb_puts(&b, "}");
  }

  if ((flags & VLD_DISASM) && ctx->bc && ctx->bc_len) {
    SEP();
    jb_puts(&b, "\"disasm\":[");
    size_t ip = 0;
    int firsti = 1;
    while (ip < ctx->bc_len) {
      uint8_t op = ctx->bc[ip++];
      if (!firsti) jb_puts(&b, ",");
      firsti = 0;
      jb_puts(&b, "{");
      jb_printf(&b, "\"off\":%zu,\"op\":\"%s\"", (size_t)ip - 1, op_name(op));
      switch (op) {
        case OP_PUSHI: {
          if (ip + 8 > ctx->bc_len) {
            jb_puts(&b, ",\"err\":\"trunc\"}");
            goto done_dis;
          }
          uint64_t u = 0;
          for (int i = 0; i < 8; i++)
            u |= ((uint64_t)ctx->bc[ip + i]) << (8 * i);
          ip += 8;
          jb_printf(&b, ",\"i64\":%" PRId64, (int64_t)u);
        } break;
        case OP_PUSHF: {
          if (ip + 8 > ctx->bc_len) {
            jb_puts(&b, ",\"err\":\"trunc\"}");
            goto done_dis;
          }
          union {
            uint64_t u;
            double d;
          } u = {0};
          for (int i = 0; i < 8; i++)
            u.u |= ((uint64_t)ctx->bc[ip + i]) << (8 * i);
          ip += 8;
          jb_printf(&b, ",\"f64\":%.*g", 17, u.d);
        } break;
        case OP_PUSHS:
        case OP_STOREG:
        case OP_LOADG: {
          if (ip + 4 > ctx->bc_len) {
            jb_puts(&b, ",\"err\":\"trunc\"}");
            goto done_dis;
          }
          uint32_t si = (uint32_t)ctx->bc[ip] |
                        ((uint32_t)ctx->bc[ip + 1] << 8) |
                        ((uint32_t)ctx->bc[ip + 2] << 16) |
                        ((uint32_t)ctx->bc[ip + 3] << 24);
          ip += 4;
          jb_printf(&b, ",\"s\":%u", si);
          if (ctx->kstr && si < ctx->kstr_len && ctx->kstr[si]) {
            jb_puts(&b, ",\"str\":");
            json_escape_str(&b, ctx->kstr[si]->data, ctx->kstr[si]->len);
          }
        } break;
        case OP_CALLN: {
          if (ip + 5 > ctx->bc_len) {
            jb_puts(&b, ",\"err\":\"trunc\"}");
            goto done_dis;
          }
          uint32_t si = (uint32_t)ctx->bc[ip] |
                        ((uint32_t)ctx->bc[ip + 1] << 8) |
                        ((uint32_t)ctx->bc[ip + 2] << 16) |
                        ((uint32_t)ctx->bc[ip + 3] << 24);
          ip += 4;
          uint8_t argc = ctx->bc[ip++];
          jb_printf(&b, ",\"s\":%u,\"argc\":%u", si, argc);
          if (ctx->kstr && si < ctx->kstr_len && ctx->kstr[si]) {
            jb_puts(&b, ",\"str\":");
            json_escape_str(&b, ctx->kstr[si]->data, ctx->kstr[si]->len);
          }
        } break;
        default:
          break;
      }
      jb_puts(&b, "}");
      if (op == OP_HALT) break;
    }
  done_dis:
    jb_puts(&b, "]");
  }

  jb_puts(&b, "}");
  if (b.err) {
    jb_free(&b);
    return VL_ERR_OOM;
  }
  *out_json = b.d;
  if (out_len) *out_len = b.n;
  return VL_OK;
}

// ───────────────────────── Dump VLBC JSON brut ─────────────────────────
VL_Status vl_dump_vlbc_json(const uint8_t *buf, size_t n, int flags,
                            char **out_json, size_t *out_len) {
  if (!buf || !out_json) return VL_ERR_BAD_ARG;
  if (flags == 0) flags = VLD_ALL;
  size_t i = 0;
  if (n < 5 || memcmp(buf, "VLBC", 4) != 0) return VL_ERR_BAD_BYTECODE;
  uint8_t ver = 0;
  i = 4;
  if (!rd_u8(buf, n, &i, &ver)) return VL_ERR_BAD_BYTECODE;
  uint32_t nstr = 0;
  if (!rd_u32(buf, n, &i, &nstr)) return VL_ERR_BAD_BYTECODE;
  const uint8_t *str_base = buf + i;
  size_t str_bytes = 0;  // for info
  JBuf b = {0};
  jb_puts(&b, "{");
  int first = 1;
#define SEP2() do {
  if (!first) jb_puts(&b, ",");
  first = 0;
}
while (0) SEP2();
jb_printf(&b, "\"version\":%u", ver);
if (flags & VLD_CONSTS) {
  SEP2();
  jb_puts(&b, "\"consts\":[");
  for (uint32_t s = 0; s < nstr; s++) {
    uint32_t sl = 0;
    if (!rd_u32(buf, n, &i, &sl) || i + sl > n) {
      jb_puts(&b, "]}");
      jb_free(&b);
      return VL_ERR_BAD_BYTECODE;
    }
    if (s) jb_puts(&b, ",");
    json_escape_str(&b, (const char *)(buf + i), sl);
    i += sl;
    str_bytes += sl;
  }
  jb_puts(&b, "]");
}
uint32_t code_sz = 0;
if (!rd_u32(buf, n, &i, &code_sz) || i + code_sz > n) {
  jb_free(&b);
  return VL_ERR_BAD_BYTECODE;
}
if (flags & VLD_BYTECODE) {
  SEP2();
  jb_printf(&b, "\"code_size\":%u", code_sz);
  if (flags & VLD_HEX) {
    jb_puts(&b, ",\"code_hex\":\"");
    for (uint32_t k = 0; k < code_sz; k++) jb_printf(&b, "%02x", buf[i + k]);
    jb_puts(&b, "\"");
  }
}
if (flags & VLD_DISASM) {
  SEP2();
  jb_puts(&b, "\"disasm\":[");
  size_t ip = i;
  size_t end = i + code_sz;
  int firsti = 1;
  while (ip < end) {
    uint8_t op = 0;
    rd_u8(buf, n, &ip, &op);
    if (!firsti) jb_puts(&b, ",");
    firsti = 0;
    jb_puts(&b, "{");
    jb_printf(&b, "\"off\":%zu,\"op\":\"%s\"", (size_t)ip - 1 - i, op_name(op));
    switch (op) {
      case OP_PUSHI: {
        uint64_t v = 0;
        rd_u64(buf, n, &ip, &v);
        jb_printf(&b, ",\"i64\":%" PRId64, (int64_t)v);
      } break;
      case OP_PUSHF: {
        double d = 0;
        rd_f64(buf, n, &ip, &d);
        jb_printf(&b, ",\"f64\":%.*g", 17, d);
      } break;
      case OP_PUSHS:
      case OP_STOREG:
      case OP_LOADG: {
        uint32_t si = 0;
        rd_u32(buf, n, &ip, &si);
        jb_printf(&b, ",\"s\":%u", si);
      } break;
      case OP_CALLN: {
        uint32_t si = 0;
        uint8_t argc = 0;
        rd_u32(buf, n, &ip, &si);
        rd_u8(buf, n, &ip, &argc);
        jb_printf(&b, ",\"s\":%u,\"argc\":%u", si, argc);
      } break;
      default:
        break;
    }
    jb_puts(&b, "}");
    if (op == OP_HALT) break;
  }
  jb_puts(&b, "]");
}
jb_puts(&b, "}");
if (b.err) {
  jb_free(&b);
  return VL_ERR_OOM;
}
*out_json = b.d;
if (out_len) *out_len = b.n;
return VL_OK;
}

// ───────────────────────── Dump texte: contexte VM ─────────────────────────
void vl_dump_context_text(struct VL_Context *ctx, int flags, FILE *out) {
  if (!out) out = stdout;
  if (!ctx) {
    fputs("<no ctx>\n", out);
    return;
  }
  if (flags == 0) flags = VLD_ALL;
  if (flags & VLD_STATE) {
    fprintf(out, "state: ip=%zu bc_len=%zu sp=%zu stack_cap=%zu\n",
            (size_t)ctx->ip, (size_t)ctx->bc_len, (size_t)ctx->sp,
            (size_t)ctx->stack_cap);
  }
  if ((flags & VLD_CONSTS) && ctx->kstr) {
    fprintf(out, "consts[%zu]:\n", ctx->kstr_len);
    for (size_t i = 0; i < ctx->kstr_len; i++) {
      VL_String *s = ctx->kstr[i];
      fprintf(out, "  [%03zu] ", i);
      if (s)
        fwrite(s->data, 1, s->len, out);
      else
        fputs("<null>", out);
      fputc('\n', out);
    }
  }
  if (flags & VLD_STACK) {
    fprintf(out, "stack sp=%zu:\n", ctx->sp);
    for (size_t i = 0; i < ctx->sp; i++) {
      fprintf(out, "  [%03zu] ", i);
      vl_value_print(&ctx->stack[i], out);
      fputc('\n', out);
    }
  }
  if ((flags & VLD_GLOBALS) && ctx->globals.cap) {
    fprintf(out, "globals len=%zu cap=%zu:\n", ctx->globals.len,
            ctx->globals.cap);
    for (size_t i = 0; i < ctx->globals.cap; i++) {
      VL_String *k = ctx->globals.keys ? ctx->globals.keys[i] : NULL;
      if (!k || k == (VL_String *)(uintptr_t)1) continue;
      fprintf(out, "  %.*s = ", (int)k->len, k->data);
      vl_value_print(&ctx->globals.vals[i], out);
      fputc('\n', out);
    }
  }
  if ((flags & VLD_BYTECODE) && ctx->bc) {
    fprintf(out, "bytecode size=%zu\n", ctx->bc_len);
    if (flags & VLD_HEX) {
      for (size_t i = 0; i < ctx->bc_len; i++) {
        if ((i % 16) == 0) fprintf(out, "%04zu ", i);
        fprintf(out, "%02x ", ctx->bc[i]);
        if ((i % 16) == 15) fputc('\n', out);
      }
      if ((ctx->bc_len % 16) != 0) fputc('\n', out);
    }
  }
  if ((flags & VLD_DISASM) && ctx->bc) {
    fprintf(out, "disasm:\n");
    size_t ip = 0;
    while (ip < ctx->bc_len) {
      uint8_t op = ctx->bc[ip++];
      fprintf(out, "%04zu\t%s", (size_t)ip - 1, op_name(op));
      switch (op) {
        case OP_PUSHI: {
          if (ip + 8 > ctx->bc_len) {
            fputs(" <trunc>\n", out);
            return;
          }
          uint64_t v = 0;
          for (int i = 0; i < 8; i++)
            v |= ((uint64_t)ctx->bc[ip + i]) << (8 * i);
          ip += 8;
          fprintf(out, "\t%" PRId64, (int64_t)v);
        } break;
        case OP_PUSHF: {
          if (ip + 8 > ctx->bc_len) {
            fputs(" <trunc>\n", out);
            return;
          }
          union {
            uint64_t u;
            double d;
          } u = {0};
          for (int i = 0; i < 8; i++)
            u.u |= ((uint64_t)ctx->bc[ip + i]) << (8 * i);
          ip += 8;
          fprintf(out, "\t%g", u.d);
        } break;
        case OP_PUSHS:
        case OP_STOREG:
        case OP_LOADG: {
          if (ip + 4 > ctx->bc_len) {
            fputs(" <trunc>\n", out);
            return;
          }
          uint32_t si = (uint32_t)ctx->bc[ip] |
                        ((uint32_t)ctx->bc[ip + 1] << 8) |
                        ((uint32_t)ctx->bc[ip + 2] << 16) |
                        ((uint32_t)ctx->bc[ip + 3] << 24);
          ip += 4;
          fprintf(out, "\t%u", si);
          if (ctx->kstr && si < ctx->kstr_len && ctx->kstr[si])
            fprintf(out, " ; \"%.*s\"", (int)ctx->kstr[si]->len,
                    ctx->kstr[si]->data);
        } break;
        case OP_CALLN: {
          if (ip + 5 > ctx->bc_len) {
            fputs(" <trunc>\n", out);
            return;
          }
          uint32_t si = (uint32_t)ctx->bc[ip] |
                        ((uint32_t)ctx->bc[ip + 1] << 8) |
                        ((uint32_t)ctx->bc[ip + 2] << 16) |
                        ((uint32_t)ctx->bc[ip + 3] << 24);
          ip += 4;
          uint8_t argc = ctx->bc[ip++];
          fprintf(out, "\t%u,%u", si, argc);
          if (ctx->kstr && si < ctx->kstr_len && ctx->kstr[si])
            fprintf(out, " ; \"%.*s\"", (int)ctx->kstr[si]->len,
                    ctx->kstr[si]->data);
        } break;
        default:
          break;
      }
      fputc('\n', out);
      if (op == OP_HALT) break;
    }
  }
}

// ───────────────────────── Ecriture fichier ─────────────────────────
bool vl_dump_write_file(const char *path, const char *json, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t wr = fwrite(json, 1, n ? n : strlen(json), f);
  fclose(f);
  return wr == (n ? n : strlen(json));
}

// ───────────────────────── Test autonome ─────────────────────────
#ifdef VL_DUMP_TEST_MAIN
int main(void) {
  // Programme simple: print("dump"), HALT
  uint8_t buf[256];
  size_t o = 0;
#define EMIT8(x)             \
  do {                       \
    buf[o++] = (uint8_t)(x); \
  }
  while (0)
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
  const char *s0 = "dump";
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
  EMIT8(OP_HALT);
  uint32_t code_sz = (uint32_t)(o - cs);
  memcpy(buf + pos, &code_sz, 4);

  VL_Context *vm = vl_create_default();
  if (!vm) {
    fprintf(stderr, "vm alloc\n");
    return 1;
  }
  if (vl_load_program_from_memory(vm, buf, o) != VL_OK) {
    fprintf(stderr, "load: %s\n", vl_last_error(vm)->msg);
    return 2;
  }

  char *json = NULL;
  size_t jn = 0;
  if (vl_dump_context_json(vm, VLD_ALL | VLD_HEX, &json, &jn) == VL_OK) {
    puts(json);
    free(json);
  }
  vl_dump_context_text(vm, VLD_ALL | VLD_HEX, stdout);

  vl_destroy(vm);
  return 0;
}
#endif
