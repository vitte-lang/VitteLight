// vitte-light/core/api.c
// C99 runtime API for VitteLight: context, values, bytecode VM, FFI, globals,
// errors, logging. Standalone. Pair with a minimal api.h if desired. Build: cc
// -std=c99 -O2 -Wall -Wextra -pedantic -o vitte_light_api_test api.c Define
// VL_API_TEST_MAIN to compile the built-in smoke test.

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdio>

// ───────────────────────── Public-ish types (would normally be in api.h)
// ─────────────────────────

typedef enum {
  VL_OK = 0,
  VL_ERR_OOM,
  VL_ERR_BAD_BYTECODE,
  VL_ERR_RUNTIME,
  VL_ERR_NOT_FOUND,
  VL_ERR_BAD_ARG,
} VL_Status;

typedef enum {
  VT_NIL = 0,
  VT_BOOL,
  VT_INT,
  VT_FLOAT,
  VT_STR,
  VT_ARRAY,  // reserved for future use
  VT_MAP,    // reserved for future use
  VT_FUNC,   // reserved for future use
  VT_NATIVE,
} VL_Type;

struct VL_Context;

typedef struct {
  VL_Type type;
  union {
    bool b;
    int64_t i;
    double f;
    struct VL_String *s;
    void *ptr;
  } as;
} VL_Value;

typedef struct {
  int code;       // VL_Status-like
  char msg[256];  // last error string
} VL_Error;

// Native function ABI: return VL_OK on success and set *ret if non-NULL.
typedef VL_Status (*VL_NativeFn)(struct VL_Context *ctx, VL_Value *args,
                                 size_t argc, VL_Value *ret, void *user);

// Config hooks (all optional). If NULL, defaults to malloc/free/realloc and
// stderr logging.
typedef void *(*VL_AllocFn)(void *ud, void *ptr, size_t oldsz,
                            size_t newsz);  // realloc-style. newsz=0 => free
typedef void (*VL_LogFn)(void *ud, const char *level, const char *fmt, ...);

typedef struct {
  VL_AllocFn alloc;  // optional
  void *alloc_ud;    // optional
  VL_LogFn log;      // optional
  void *log_ud;      // optional
  size_t stack_cap;  // optional, default 1024
} VL_Config;

// ───────────────────────── Internal structures ─────────────────────────

// Simple FNV-1a 32-bit
static uint32_t vl_hash(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h ? h : 1u;  // never 0
}

// String object (interned)
typedef struct VL_String {
  uint32_t hash;
  uint32_t len;
  char data[];  // NUL-terminated
} VL_String;

// Open addressing map for string keys -> VL_Value
typedef struct {
  VL_String **keys;  // interned keys
  VL_Value *vals;
  size_t cap;   // power of two or 0
  size_t len;   // number of occupied slots (excluding tombstones)
  size_t tomb;  // tombstones
} VL_Map;

// Native registry entry
typedef struct {
  VL_NativeFn fn;
  void *ud;
} VL_Native;

// VM context
typedef struct VL_Context {
  VL_AllocFn ralloc;
  void *alloc_ud;
  VL_LogFn log;
  void *log_ud;

  VL_Error last_error;

  // VM
  uint8_t *bc;  // bytecode buffer
  size_t bc_len;
  size_t ip;  // instruction pointer

  VL_Value *stack;
  size_t sp;
  size_t stack_cap;

  // Constants pool: interned strings (indexable)
  VL_String **kstr;
  size_t kstr_len;

  // Globals and natives
  VL_Map globals;
  VL_Map natives;  // value.as.ptr -> VL_Native*

} VL_Context;

// ───────────────────────── Alloc, logging, errors ─────────────────────────

static void *vl_sys_alloc(void *ud, void *ptr, size_t oldsz, size_t newsz) {
  (void)ud;
  (void)oldsz;
  if (newsz == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, newsz);
}

static void vl_sys_log(void *ud, const char *level, const char *fmt, ...) {
  (void)ud;
  fprintf(stderr, "[VL][%s] ", level ? level : "log");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static void *vl_alloc(VL_Context *ctx, void *ptr, size_t oldsz, size_t newsz) {
  return ctx->ralloc ? ctx->ralloc(ctx->alloc_ud, ptr, oldsz, newsz)
                     : vl_sys_alloc(NULL, ptr, oldsz, newsz);
}

static void vl_logf(VL_Context *ctx, const char *lvl, const char *fmt, ...) {
  VL_LogFn lg = ctx && ctx->log ? ctx->log : vl_sys_log;
  void *ud = ctx ? ctx->log_ud : NULL;
  va_list ap;
  va_start(ap, fmt);
  if (lg) {
    // We cannot pass va_list directly to user function with this signature, so
    // build a temp buffer.
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    lg(ud, lvl, "%s", buf);
  }
  va_end(ap);
}

static VL_Status vl_set_err(VL_Context *ctx, VL_Status code, const char *fmt,
                            ...) {
  if (!ctx) return code;
  ctx->last_error.code = code;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->last_error.msg, sizeof(ctx->last_error.msg), fmt, ap);
  va_end(ap);
  vl_logf(ctx, "error", "%s", ctx->last_error.msg);
  return code;
}

// ───────────────────────── Value helpers ─────────────────────────

static VL_Value vl_nil(void) {
  VL_Value v;
  v.type = VT_NIL;
  v.as.ptr = NULL;
  return v;
}
static VL_Value vl_bool(bool b) {
  VL_Value v;
  v.type = VT_BOOL;
  v.as.b = b;
  return v;
}
static VL_Value vl_int(int64_t i) {
  VL_Value v;
  v.type = VT_INT;
  v.as.i = i;
  return v;
}
static VL_Value vl_float(double f) {
  VL_Value v;
  v.type = VT_FLOAT;
  v.as.f = f;
  return v;
}
static VL_Value vl_str(VL_String *s) {
  VL_Value v;
  v.type = VT_STR;
  v.as.s = s;
  return v;
}
static const char *vl_type_name(VL_Type t) {
  switch (t) {
    case VT_NIL:
      return "nil";
    case VT_BOOL:
      return "bool";
    case VT_INT:
      return "int";
    case VT_FLOAT:
      return "float";
    case VT_STR:
      return "str";
    case VT_ARRAY:
      return "array";
    case VT_MAP:
      return "map";
    case VT_FUNC:
      return "func";
    case VT_NATIVE:
      return "native";
    default:
      return "?";
  }
}

// ───────────────────────── Map (open addressing, linear probe)
// ─────────────────────────

static uint32_t vl_str_hash(VL_String *s) { return s ? s->hash : 0; }

static bool vl_str_eq(VL_String *a, VL_String *b) {
  return a == b || (a && b && a->len == b->len && a->hash == b->hash &&
                    memcmp(a->data, b->data, a->len) == 0);
}

static bool vl_map_init(VL_Context *ctx, VL_Map *m, size_t cap_pow2) {
  memset(m, 0, sizeof(*m));
  size_t cap = cap_pow2 < 4 ? 8 : (size_t)1 << cap_pow2;
  m->keys = (VL_String **)vl_alloc(ctx, NULL, 0, cap * sizeof(VL_String *));
  m->vals = (VL_Value *)vl_alloc(ctx, NULL, 0, cap * sizeof(VL_Value));
  if (!m->keys || !m->vals) return false;
  memset(m->keys, 0, cap * sizeof(VL_String *));
  for (size_t i = 0; i < cap; i++) m->vals[i] = vl_nil();
  m->cap = cap;
  m->len = 0;
  m->tomb = 0;
  return true;
}

static void vl_map_free(VL_Context *ctx, VL_Map *m) {
  if (!m || !m->cap) return;
  vl_alloc(ctx, m->keys, m->cap * sizeof(VL_String *), 0);
  vl_alloc(ctx, m->vals, m->cap * sizeof(VL_Value), 0);
  memset(m, 0, sizeof(*m));
}

static size_t vl_map_probe(VL_Map *m, VL_String *key, bool *found,
                           bool *is_tomb) {
  uint32_t h = vl_str_hash(key);
  size_t mask = m->cap - 1;
  size_t idx = h & mask;
  size_t first_tomb = SIZE_MAX;
  for (;;) {
    VL_String *k = m->keys[idx];
    if (k == NULL) {
      *found = false;
      if (is_tomb) *is_tomb = false;
      return (first_tomb != SIZE_MAX) ? first_tomb : idx;
    }
    if (k == (VL_String *)(uintptr_t)1) {  // tombstone marker
      if (first_tomb == SIZE_MAX) first_tomb = idx;
    } else if (vl_str_eq(k, key)) {
      *found = true;
      if (is_tomb) *is_tomb = false;
      return idx;
    }
    idx = (idx + 1) & mask;
  }
}

static bool vl_map_rehash(VL_Context *ctx, VL_Map *m, size_t new_cap) {
  VL_String **oldk = m->keys;
  VL_Value *oldv = m->vals;
  size_t oldcap = m->cap;
  if (!vl_map_init(ctx, m,
                   (size_t)(new_cap ? new_cap : oldcap * 2) == 0
                       ? 4
                       : (size_t)(new_cap ? new_cap : oldcap * 2)))
    return false;
  for (size_t i = 0; i < oldcap; i++) {
    VL_String *k = oldk[i];
    if (k && k != (VL_String *)(uintptr_t)1) {
      bool found, tomb;
      size_t idx = vl_map_probe(m, k, &found, &tomb);
      m->keys[idx] = k;
      m->vals[idx] = oldv[i];
      m->len++;
    }
  }
  vl_alloc(ctx, oldk, oldcap * sizeof(VL_String *), 0);
  vl_alloc(ctx, oldv, oldcap * sizeof(VL_Value), 0);
  return true;
}

static bool vl_map_put(VL_Context *ctx, VL_Map *m, VL_String *key,
                       VL_Value val) {
  if ((m->len + m->tomb) * 100 / m->cap > 70) {
    if (!vl_map_rehash(ctx, m, m->cap * 2)) return false;
  }
  bool found, tomb;
  size_t idx = vl_map_probe(m, key, &found, &tomb);
  if (found) {
    m->vals[idx] = val;
    return true;
  }
  if (m->keys[idx] == (VL_String *)(uintptr_t)1) m->tomb--;  // reuse tomb
  m->keys[idx] = key;
  m->vals[idx] = val;
  m->len++;
  return true;
}

static bool vl_map_get(VL_Map *m, VL_String *key, VL_Value *out) {
  if (m->cap == 0) return false;
  bool found, tomb;
  size_t idx = vl_map_probe(m, key, &found, &tomb);
  if (!found) return false;
  if (out) *out = m->vals[idx];
  return true;
}

static bool vl_map_del(VL_Map *m, VL_String *key) {
  if (m->cap == 0) return false;
  bool found, tomb;
  size_t idx = vl_map_probe(m, key, &found, &tomb);
  if (!found) return false;
  m->keys[idx] = (VL_String *)(uintptr_t)1;  // tombstone
  m->vals[idx] = vl_nil();
  m->len--;
  m->tomb++;
  return true;
}

// ───────────────────────── String interning ─────────────────────────

static VL_String *vl_str_new(VL_Context *ctx, const char *s, size_t n) {
  uint32_t h = vl_hash(s, n);
  size_t total = sizeof(VL_String) + n + 1;
  VL_String *st = (VL_String *)vl_alloc(ctx, NULL, 0, total);
  if (!st) return NULL;
  st->hash = h;
  st->len = (uint32_t)n;
  memcpy(st->data, s, n);
  st->data[n] = '\0';
  return st;
}

static VL_String *vl_intern(VL_Context *ctx, VL_Map *table, const char *s,
                            size_t n) {
  VL_String tmp = {vl_hash(s, n), (uint32_t)n, {0}};
  VL_String *key_probe = &tmp;  // stack key for equality/hash

  // search
  if (table->cap) {
    bool found, tomb;
    size_t mask = table->cap - 1;
    size_t idx = tmp.hash & mask;
    for (;;) {
      VL_String *k = table->keys[idx];
      if (k == NULL) {
        break;
      }
      if (k != (VL_String *)(uintptr_t)1 && k->len == tmp.len &&
          k->hash == tmp.hash && memcmp(k->data, s, n) == 0) {
        return k;  // already interned
      }
      idx = (idx + 1) & mask;
    }
  }

  // create new and insert
  VL_String *st = vl_str_new(ctx, s, n);
  if (!st) return NULL;
  if (table->cap == 0 && !vl_map_init(ctx, table, 4)) return NULL;
  if (!vl_map_put(ctx, table, st, vl_nil())) return NULL;
  return st;
}

// ───────────────────────── Bytecode format ─────────────────────────
// Layout (little-endian):
//   magic "VLBC" (4 bytes)
//   u8 version (=1)
//   u32 nstrings
//     repeat nstrings: u32 len, bytes[len]
//   u32 code_size
//   u8  code[code_size]
// Execution starts at IP=0.

// Opcodes
enum {
  OP_NOP = 0,
  OP_PUSHI = 1,  // int64
  OP_PUSHF = 2,  // double
  OP_PUSHS = 3,  // u32 string index
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
  OP_PRINT = 14,  // pop 1, print
  OP_POP = 15,
  OP_STOREG = 16,  // u32 str idx, pop -> globals[name]
  OP_LOADG = 17,   // u32 str idx, push globals[name]
  OP_CALLN = 18,   // u32 str idx, u8 argc  ; call native(name, argc)
  OP_HALT = 19
};

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

// ───────────────────────── VM core ─────────────────────────

static bool vl_stack_grow(VL_Context *ctx) {
  size_t newcap = ctx->stack_cap ? ctx->stack_cap * 2 : 1024;
  VL_Value *ns =
      (VL_Value *)vl_alloc(ctx, ctx->stack, ctx->stack_cap * sizeof(VL_Value),
                           newcap * sizeof(VL_Value));
  if (!ns) return false;
  ctx->stack = ns;
  ctx->stack_cap = newcap;
  return true;
}

static bool vl_push(VL_Context *ctx, VL_Value v) {
  if (ctx->sp >= ctx->stack_cap && !vl_stack_grow(ctx)) return false;
  ctx->stack[ctx->sp++] = v;
  return true;
}

static VL_Value vl_pop(VL_Context *ctx) {
  if (ctx->sp == 0) {
    return vl_nil();
  }
  return ctx->stack[--ctx->sp];
}

static VL_String *vl_kstr_at(VL_Context *ctx, uint32_t idx) {
  return (idx < ctx->kstr_len) ? ctx->kstr[idx] : NULL;
}

static VL_Status vl_exec_step(VL_Context *ctx) {
  if (ctx->ip >= ctx->bc_len)
    return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "IP past code");
  uint8_t op = ctx->bc[ctx->ip++];
  switch (op) {
    case OP_NOP:
      break;
    case OP_HALT:
      return VL_OK;  // signal as OK, upper loop stops on HALT
    case OP_PUSHI: {
      if (ctx->ip + 8 > ctx->bc_len)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "PUSHI truncated");
      int64_t v = 0;
      for (int i = 0; i < 8; i++)
        v |= ((int64_t)ctx->bc[ctx->ip + i]) << (8 * i);
      ctx->ip += 8;
      if (!vl_push(ctx, vl_int(v)))
        return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
    } break;
    case OP_PUSHF: {
      if (ctx->ip + 8 > ctx->bc_len)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "PUSHF truncated");
      union {
        uint64_t u;
        double d;
      } u = {0};
      for (int i = 0; i < 8; i++)
        u.u |= ((uint64_t)ctx->bc[ctx->ip + i]) << (8 * i);
      ctx->ip += 8;
      if (!vl_push(ctx, vl_float(u.d)))
        return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
    } break;
    case OP_PUSHS: {
      if (ctx->ip + 4 > ctx->bc_len)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "PUSHS truncated");
      uint32_t si = (uint32_t)ctx->bc[ctx->ip] |
                    ((uint32_t)ctx->bc[ctx->ip + 1] << 8) |
                    ((uint32_t)ctx->bc[ctx->ip + 2] << 16) |
                    ((uint32_t)ctx->bc[ctx->ip + 3] << 24);
      ctx->ip += 4;
      VL_String *s = vl_kstr_at(ctx, si);
      if (!s)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "PUSHS bad idx %u", si);
      if (!vl_push(ctx, vl_str(s)))
        return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
    } break;
    case OP_POP: {
      (void)vl_pop(ctx);
    } break;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV: {
      VL_Value b = vl_pop(ctx);
      VL_Value a = vl_pop(ctx);
      double x, y;  // numeric promotion: int->double when needed
      if ((a.type == VT_INT || a.type == VT_FLOAT) &&
          (b.type == VT_INT || b.type == VT_FLOAT)) {
        x = (a.type == VT_FLOAT) ? a.as.f : (double)a.as.i;
        y = (b.type == VT_FLOAT) ? b.as.f : (double)b.as.i;
        double r = 0.0;
        if (op == OP_ADD)
          r = x + y;
        else if (op == OP_SUB)
          r = x - y;
        else if (op == OP_MUL)
          r = x * y;
        else {
          if (y == 0.0)
            return vl_set_err(ctx, VL_ERR_RUNTIME, "division by zero");
          r = x / y;
        }
        if (!vl_push(ctx, vl_float(r)))
          return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
      } else {
        return vl_set_err(ctx, VL_ERR_RUNTIME, "arith on non-numbers (%s,%s)",
                          vl_type_name(a.type), vl_type_name(b.type));
      }
    } break;
    case OP_EQ:
    case OP_NEQ:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE: {
      VL_Value b = vl_pop(ctx);
      VL_Value a = vl_pop(ctx);
      if ((a.type == VT_INT || a.type == VT_FLOAT) &&
          (b.type == VT_INT || b.type == VT_FLOAT)) {
        double x = (a.type == VT_FLOAT) ? a.as.f : (double)a.as.i;
        double y = (b.type == VT_FLOAT) ? b.as.f : (double)b.as.i;
        bool r = false;
        if (op == OP_EQ)
          r = (x == y);
        else if (op == OP_NEQ)
          r = (x != y);
        else if (op == OP_LT)
          r = (x < y);
        else if (op == OP_GT)
          r = (x > y);
        else if (op == OP_LE)
          r = (x <= y);
        else if (op == OP_GE)
          r = (x >= y);
        if (!vl_push(ctx, vl_bool(r)))
          return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
      } else if (a.type == VT_STR && b.type == VT_STR &&
                 (op == OP_EQ || op == OP_NEQ)) {
        bool eq = vl_str_eq(a.as.s, b.as.s);
        if (!vl_push(ctx, vl_bool(op == OP_EQ ? eq : !eq)))
          return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
      } else {
        return vl_set_err(ctx, VL_ERR_RUNTIME, "cmp on types (%s,%s)",
                          vl_type_name(a.type), vl_type_name(b.type));
      }
    } break;
    case OP_PRINT: {
      VL_Value v = vl_pop(ctx);
      switch (v.type) {
        case VT_NIL:
          printf("nil\n");
          break;
        case VT_BOOL:
          printf(v.as.b ? "true\n" : "false\n");
          break;
        case VT_INT:
          printf("%" PRId64 "\n", v.as.i);
          break;
        case VT_FLOAT:
          printf("%g\n", v.as.f);
          break;
        case VT_STR:
          printf("%.*s\n", (int)v.as.s->len, v.as.s->data);
          break;
        default:
          printf("<%s>\n", vl_type_name(v.type));
          break;
      }
    } break;
    case OP_STOREG: {
      if (ctx->ip + 4 > ctx->bc_len)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "STOREG truncated");
      uint32_t si = (uint32_t)ctx->bc[ctx->ip] |
                    ((uint32_t)ctx->bc[ctx->ip + 1] << 8) |
                    ((uint32_t)ctx->bc[ctx->ip + 2] << 16) |
                    ((uint32_t)ctx->bc[ctx->ip + 3] << 24);
      ctx->ip += 4;
      VL_String *name = vl_kstr_at(ctx, si);
      if (!name)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "STOREG bad idx %u", si);
      VL_Value v = vl_pop(ctx);
      if (!vl_map_put(ctx, &ctx->globals, name, v))
        return vl_set_err(ctx, VL_ERR_OOM, "globals put OOM");
    } break;
    case OP_LOADG: {
      if (ctx->ip + 4 > ctx->bc_len)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "LOADG truncated");
      uint32_t si = (uint32_t)ctx->bc[ctx->ip] |
                    ((uint32_t)ctx->bc[ctx->ip + 1] << 8) |
                    ((uint32_t)ctx->bc[ctx->ip + 2] << 16) |
                    ((uint32_t)ctx->bc[ctx->ip + 3] << 24);
      ctx->ip += 4;
      VL_String *name = vl_kstr_at(ctx, si);
      if (!name)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "LOADG bad idx %u", si);
      VL_Value v;
      if (!vl_map_get(&ctx->globals, name, &v)) v = vl_nil();
      if (!vl_push(ctx, v)) return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
    } break;
    case OP_CALLN: {
      if (ctx->ip + 5 > ctx->bc_len)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "CALLN truncated");
      uint32_t si = (uint32_t)ctx->bc[ctx->ip] |
                    ((uint32_t)ctx->bc[ctx->ip + 1] << 8) |
                    ((uint32_t)ctx->bc[ctx->ip + 2] << 16) |
                    ((uint32_t)ctx->bc[ctx->ip + 3] << 24);
      ctx->ip += 4;
      uint8_t argc = ctx->bc[ctx->ip++];
      VL_String *name = vl_kstr_at(ctx, si);
      if (!name)
        return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "CALLN bad idx %u", si);
      VL_Value entry;
      if (!vl_map_get(&ctx->natives, name, &entry) || entry.type != VT_NATIVE ||
          !entry.as.ptr) {
        return vl_set_err(ctx, VL_ERR_NOT_FOUND, "native '%.*s' not found",
                          name->len, name->data);
      }
      VL_Native *nat = (VL_Native *)entry.as.ptr;
      if (argc > ctx->sp)
        return vl_set_err(ctx, VL_ERR_RUNTIME, "stack underflow in call");
      VL_Value *args = &ctx->stack[ctx->sp - argc];
      VL_Value ret = vl_nil();
      VL_Status rc = nat->fn(ctx, args, argc, &ret, nat->ud);
      // pop args
      ctx->sp -= argc;
      if (rc != VL_OK)
        return vl_set_err(ctx, rc, "native '%.*s' failed", name->len,
                          name->data);
      if (!vl_push(ctx, ret)) return vl_set_err(ctx, VL_ERR_OOM, "stack OOM");
    } break;
    default:
      return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "unknown opcode %u at %zu",
                        op, ctx->ip - 1);
  }
  return VL_OK;
}

// ───────────────────────── Public API ─────────────────────────

static VL_Context *vl_create(const VL_Config *cfg) {
  VL_Context *ctx =
      (VL_Context *)(cfg && cfg->alloc
                         ? cfg->alloc(cfg->alloc_ud, NULL, 0,
                                      sizeof(VL_Context))
                         : vl_sys_alloc(NULL, NULL, 0, sizeof(VL_Context)));
  if (!ctx) return NULL;
  memset(ctx, 0, sizeof(*ctx));
  ctx->ralloc = cfg && cfg->alloc ? cfg->alloc : vl_sys_alloc;
  ctx->alloc_ud = cfg ? cfg->alloc_ud : NULL;
  ctx->log = cfg && cfg->log ? cfg->log : vl_sys_log;
  ctx->log_ud = cfg ? cfg->log_ud : NULL;
  ctx->stack_cap = (cfg && cfg->stack_cap) ? cfg->stack_cap : 1024;
  ctx->stack =
      (VL_Value *)vl_alloc(ctx, NULL, 0, ctx->stack_cap * sizeof(VL_Value));
  if (!ctx->stack) {
    vl_alloc(ctx, ctx, sizeof(VL_Context), 0);
    return NULL;
  }
  if (!vl_map_init(ctx, &ctx->globals, 4)) {
    vl_alloc(ctx, ctx->stack, ctx->stack_cap * sizeof(VL_Value), 0);
    vl_alloc(ctx, ctx, sizeof(VL_Context), 0);
    return NULL;
  }
  if (!vl_map_init(ctx, &ctx->natives, 4)) {
    vl_map_free(ctx, &ctx->globals);
    vl_alloc(ctx, ctx->stack, ctx->stack_cap * sizeof(VL_Value), 0);
    vl_alloc(ctx, ctx, sizeof(VL_Context), 0);
    return NULL;
  }
  return ctx;
}

static void vl_destroy(VL_Context *ctx) {
  if (!ctx) return;
  // free bytecode
  if (ctx->bc) vl_alloc(ctx, ctx->bc, ctx->bc_len, 0);
  // free interned strings array
  for (size_t i = 0; i < ctx->kstr_len; i++)
    if (ctx->kstr[i])
      vl_alloc(ctx, ctx->kstr[i], sizeof(VL_String) + ctx->kstr[i]->len + 1, 0);
  vl_alloc(ctx, ctx->kstr, ctx->kstr_len * sizeof(VL_String *), 0);
  // free natives table values (VL_Native)
  if (ctx->natives.cap) {
    for (size_t i = 0; i < ctx->natives.cap; i++) {
      VL_String *k = ctx->natives.keys[i];
      if (k && k != (VL_String *)(uintptr_t)1) {
        VL_Value v = ctx->natives.vals[i];
        if (v.type == VT_NATIVE && v.as.ptr)
          vl_alloc(ctx, v.as.ptr, sizeof(VL_Native), 0);
      }
    }
  }
  vl_map_free(ctx, &ctx->natives);
  vl_map_free(ctx, &ctx->globals);
  vl_alloc(ctx, ctx->stack, ctx->stack_cap * sizeof(VL_Value), 0);
  vl_alloc(ctx, ctx, sizeof(VL_Context), 0);
}

static const VL_Error *vl_last_error(VL_Context *ctx) {
  return ctx ? &ctx->last_error : NULL;
}
static void vl_clear_error(VL_Context *ctx) {
  if (ctx) {
    ctx->last_error.code = VL_OK;
    ctx->last_error.msg[0] = '\0';
  }
}

// Load VLBC buffer
static VL_Status vl_load_program_from_memory(VL_Context *ctx, const void *buf,
                                             size_t len) {
  if (!ctx || !buf || len < 4)
    return vl_set_err(ctx, VL_ERR_BAD_ARG, "bad args to load");
  const uint8_t *p = (const uint8_t *)buf;
  size_t i = 0;
  if (!(len >= 5 && p[0] == 'V' && p[1] == 'L' && p[2] == 'B' && p[3] == 'C'))
    return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "bad magic");
  i = 4;
  uint8_t ver;
  if (!rd_u8(p, len, &i, &ver))
    return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "truncated ver");
  if (ver != 1)
    return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "unsupported ver %u", ver);
  uint32_t nstr = 0;
  if (!rd_u32(p, len, &i, &nstr))
    return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "truncated nstr");

  // free previous
  for (size_t k = 0; k < ctx->kstr_len; k++)
    if (ctx->kstr[k])
      vl_alloc(ctx, ctx->kstr[k], sizeof(VL_String) + ctx->kstr[k]->len + 1, 0);
  vl_alloc(ctx, ctx->kstr, ctx->kstr_len * sizeof(VL_String *), 0);
  ctx->kstr = NULL;
  ctx->kstr_len = 0;

  ctx->kstr = (VL_String **)vl_alloc(ctx, NULL, 0, nstr * sizeof(VL_String *));
  if (!ctx->kstr) return vl_set_err(ctx, VL_ERR_OOM, "kstr OOM");
  ctx->kstr_len = nstr;
  for (uint32_t s = 0; s < nstr; s++) {
    uint32_t slen = 0;
    if (!rd_u32(p, len, &i, &slen))
      return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "str len trunc");
    if (i + slen > len)
      return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "str bytes trunc");
    VL_String *st = vl_str_new(ctx, (const char *)(p + i), slen);
    if (!st) return vl_set_err(ctx, VL_ERR_OOM, "str OOM");
    ctx->kstr[s] = st;
    i += slen;
  }

  uint32_t code_sz = 0;
  if (!rd_u32(p, len, &i, &code_sz))
    return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "code sz trunc");
  if (i + code_sz > len)
    return vl_set_err(ctx, VL_ERR_BAD_BYTECODE, "code bytes trunc");

  if (ctx->bc) vl_alloc(ctx, ctx->bc, ctx->bc_len, 0);
  ctx->bc = (uint8_t *)vl_alloc(ctx, NULL, 0, code_sz);
  if (!ctx->bc) return vl_set_err(ctx, VL_ERR_OOM, "code OOM");
  memcpy(ctx->bc, p + i, code_sz);
  ctx->bc_len = code_sz;
  ctx->ip = 0;
  ctx->sp = 0;
  return VL_OK;
}

// Run until HALT or step limit (0 => no limit). Returns status. On HALT,
// returns VL_OK.
static VL_Status vl_run(VL_Context *ctx, uint64_t max_steps) {
  if (!ctx) return VL_ERR_BAD_ARG;
  uint64_t steps = 0;
  for (;;) {
    if (max_steps && steps++ >= max_steps)
      return VL_OK;  // soft stop for embedding
    size_t prev_ip = ctx->ip;
    VL_Status rc = vl_exec_step(ctx);
    if (rc != VL_OK) {
      if (prev_ip < ctx->bc_len && ctx->bc[prev_ip] == OP_HALT)
        return VL_OK;  // graceful halt
      return rc;
    }
    // stop on HALT opcode (exec_step returns VL_OK but we detect by prev
    // opcode)
    if (prev_ip < ctx->bc_len && ctx->bc[prev_ip] == OP_HALT) return VL_OK;
  }
}

static VL_Status vl_step(VL_Context *ctx) {
  return ctx ? vl_exec_step(ctx) : VL_ERR_BAD_ARG;
}

static VL_Status vl_register_native(VL_Context *ctx, const char *name,
                                    VL_NativeFn fn, void *ud) {
  if (!ctx || !name || !fn) return VL_ERR_BAD_ARG;
  // intern key in natives map's keyspace. We reuse the map's own pool by
  // interning into its keys set
  if (!ctx->natives.cap && !vl_map_init(ctx, &ctx->natives, 4))
    return VL_ERR_OOM;
  VL_String *ks = vl_intern(ctx, &ctx->natives, name, strlen(name));
  if (!ks) return VL_ERR_OOM;
  VL_Native *nat = (VL_Native *)vl_alloc(ctx, NULL, 0, sizeof(VL_Native));
  if (!nat) return VL_ERR_OOM;
  nat->fn = fn;
  nat->ud = ud;
  VL_Value v;
  v.type = VT_NATIVE;
  v.as.ptr = nat;
  if (!vl_map_put(ctx, &ctx->natives, ks, v)) {
    vl_alloc(ctx, nat, sizeof(VL_Native), 0);
    return VL_ERR_OOM;
  }
  return VL_OK;
}

static VL_Status vl_set_global(VL_Context *ctx, const char *name, VL_Value v) {
  if (!ctx || !name) return VL_ERR_BAD_ARG;
  if (!ctx->globals.cap && !vl_map_init(ctx, &ctx->globals, 4))
    return VL_ERR_OOM;
  VL_String *ks = vl_intern(ctx, &ctx->globals, name, strlen(name));
  if (!ks) return VL_ERR_OOM;
  return vl_map_put(ctx, &ctx->globals, ks, v) ? VL_OK : VL_ERR_OOM;
}

static VL_Status vl_get_global(VL_Context *ctx, const char *name,
                               VL_Value *out) {
  if (!ctx || !name) return VL_ERR_BAD_ARG;
  if (!ctx->globals.cap) return VL_ERR_NOT_FOUND;
  VL_String tmp = {vl_hash(name, strlen(name)), (uint32_t)strlen(name), {0}};
  bool found, tomb;
  size_t idx = vl_map_probe(&ctx->globals, &tmp, &found, &tomb);
  if (!found) return VL_ERR_NOT_FOUND;
  if (out) *out = ctx->globals.vals[idx];
  return VL_OK;
}

// Helper creators for API users
static VL_Value vl_make_str(VL_Context *ctx, const char *s) {
  VL_String *st = vl_str_new(ctx, s, strlen(s));
  return st ? vl_str(st) : vl_nil();
}

// ───────────────────────── Example built-in natives ─────────────────────────

static VL_Status native_now_ms(VL_Context *ctx, VL_Value *args, size_t argc,
                               VL_Value *ret, void *ud) {
  (void)ctx;
  (void)args;
  (void)ud;
  (void)argc;
// very rough portable time in milliseconds
#ifdef _WIN32
#include <windows.h>
  ULONGLONG ms = GetTickCount64();
  if (ret) *ret = vl_float((double)ms);
#else
#include <time.h>
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
  if (ret) *ret = vl_float(ms);
#endif
  return VL_OK;
}

static VL_Status native_print(VL_Context *ctx, VL_Value *args, size_t argc,
                              VL_Value *ret, void *ud) {
  (void)ud;
  (void)ret;
  (void)ctx;
  for (size_t i = 0; i < argc; i++) {
    VL_Value v = args[i];
    switch (v.type) {
      case VT_NIL:
        printf("nil");
        break;
      case VT_BOOL:
        printf(v.as.b ? "true" : "false");
        break;
      case VT_INT:
        printf("%" PRId64, v.as.i);
        break;
      case VT_FLOAT:
        printf("%g", v.as.f);
        break;
      case VT_STR:
        printf("%.*s", (int)v.as.s->len, v.as.s->data);
        break;
      default:
        printf("<%s>", vl_type_name(v.type));
        break;
    }
    if (i + 1 < argc) printf(" ");
  }
  printf("\n");
  return VL_OK;
}

// ───────────────────────── Convenience factory for embedders
// ─────────────────────────

// Create a context with defaults and a couple of convenience natives.
static VL_Context *vl_create_default(void) {
  VL_Config c = {0};
  VL_Context *ctx = vl_create(&c);
  if (!ctx) return NULL;
  vl_register_native(ctx, "now_ms", native_now_ms, NULL);
  vl_register_native(ctx, "print", native_print, NULL);
  return ctx;
}

// ───────────────────────── Public API exposure (optional header-less)
// ───────────────────────── If you do not want a separate header yet, you can
// expose the symbols with the following names:
//   vl_create, vl_destroy, vl_last_error, vl_clear_error,
//   vl_load_program_from_memory, vl_run, vl_step,
//   vl_register_native, vl_set_global, vl_get_global,
//   vl_make_str, vl_create_default

// ───────────────────────── Embedded smoke test ─────────────────────────
#ifdef VL_API_TEST_MAIN
int main(void) {
  VL_Context *vm = vl_create_default();
  if (!vm) {
    fprintf(stderr, "no vm\n");
    return 1;
  }

  // Build a tiny program: print("hello"), 1+2 => print, HALT
  // kstr: ["hello", "print"]
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

  // header
  EMIT8('V');
  EMIT8('L');
  EMIT8('B');
  EMIT8('C');
  EMIT8(1);   // ver
  EMIT32(2);  // nstr
  const char *s0 = "hello";
  EMIT32((uint32_t)strlen(s0));
  memcpy(buf + o, s0, strlen(s0));
  o += strlen(s0);
  const char *s1 = "print";
  EMIT32((uint32_t)strlen(s1));
  memcpy(buf + o, s1, strlen(s1));
  o += strlen(s1);

  // code placeholder size
  size_t code_sz_pos = o;
  EMIT32(0);
  size_t code_start = o;

  EMIT8(OP_PUSHS);
  EMIT32(0);  // "hello"
  EMIT8(OP_CALLN);
  EMIT32(1);
  EMIT8(1);  // call native "print" with 1 arg

  EMIT8(OP_PUSHI);
  EMIT64(1);
  EMIT8(OP_PUSHI);
  EMIT64(2);
  EMIT8(OP_ADD);
  EMIT8(OP_CALLN);
  EMIT32(1);
  EMIT8(1);  // print result

  EMIT8(OP_HALT);

  // patch code size
  uint32_t code_sz = (uint32_t)(o - code_start);
  memcpy(buf + code_sz_pos, &code_sz, 4);

  VL_Status rc = vl_load_program_from_memory(vm, buf, o);
  if (rc != VL_OK) {
    fprintf(stderr, "load failed: %s\n", vl_last_error(vm)->msg);
    return 2;
  }
  rc = vl_run(vm, 0);
  if (rc != VL_OK) {
    fprintf(stderr, "run failed: %s\n", vl_last_error(vm)->msg);
    return 3;
  }

  vl_destroy(vm);
  return 0;
}
#endif
