// vitte-light/core/vm.c
// Machine virtuelle VitteLight (exécution, contexte, natives, état, traces).
// Compatible avec le set d'opcodes de core/opcodes.h et les introspections de
// core/state.h. Boucle d'exécution en switch; si jumptab.c est lié, vous pouvez
// aussi utiliser vl_run_jumptab() pour le hot path.
//
// Fourni:
//   - Contexte opaque VL_Context (pile, globals, kstr, code, traces)
//   - Cycle de vie: vl_ctx_new(), vl_ctx_free()
//   - Chargement: vl_ctx_attach_module()/vl_ctx_detach_module()
//   - Natives: registre par nom -> callback
//   - Exécution: vl_step(), vl_run(max_steps)
//   - State API: vl_state_* (IP, pile, globals, kstr, traces, dumps)
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/vm.c

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"      // VL_Value, VL_Status, vlv_*
#include "ctype.h"    // conversions
#include "object.h"   // VL_String, vl_make_strn
#include "opcodes.h"  // codes + vl_op_* helpers
#include "state.h"    // introspection
#include "undump.h"   // VL_Module

// ───────────────────────── Contexte interne ─────────────────────────
#ifndef VL_STACK_INIT
#define VL_STACK_INIT 256u
#endif
#ifndef VL_GLOBAL_INIT
#define VL_GLOBAL_INIT 64u
#endif

typedef struct VL_Native {
  uint32_t name_si;  // index dans kstr (résolu au moment de l'attache;
                     // UINT32_MAX si inconnu)
  char *name_c;      // fallback par cstr pour recherche tardive
  // Signature: args[0..argc-1], ret out peut être nil
  VL_Status (*fn)(struct VL_Context *ctx, const VL_Value *args, uint8_t argc,
                  VL_Value *ret, void *udata);
  void *udata;
} VL_Native;

struct VL_Context {
  // code + kstr
  const uint8_t *code;
  size_t code_len;
  size_t ip;
  char **kstr;
  uint32_t kcount;

  // pile
  VL_Value *stack;
  size_t sp, cap;

  // globals indexés par si (taille = kcount au moment de l'attache)
  VL_Value *globals;
  size_t gcap;  // gcap==kcount actuel

  // natives dynamiques
  VL_Native *nats;
  size_t nnat, ncap;

  // compteurs / traces
  uint64_t steps_total;
  uint32_t trace_mask;
  VL_StepHook step_hook;
  void *hook_user;

  // I/O
  FILE *out;
};

// ───────────────────────── Utils ─────────────────────────
static inline int rd_u8(const uint8_t *p, size_t n, size_t *io, uint8_t *out) {
  if (*io + 1 > n) return 0;
  *out = p[(*io)++];
  return 1;
}
static inline int rd_u32(const uint8_t *p, size_t n, size_t *io,
                         uint32_t *out) {
  if (*io + 4 > n) return 0;
  *out = (uint32_t)p[*io] | ((uint32_t)p[*io + 1] << 8) |
         ((uint32_t)p[*io + 2] << 16) | ((uint32_t)p[*io + 3] << 24);
  *io += 4;
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

static inline VL_Value vdup(VL_Value v) { return v; }

static int stack_reserve(struct VL_Context *ctx, size_t need) {
  if (need <= ctx->cap) return 1;
  size_t cap = ctx->cap ? ctx->cap * 2 : VL_STACK_INIT;
  while (cap < need) cap += cap / 2;
  VL_Value *np = (VL_Value *)realloc(ctx->stack, cap * sizeof(VL_Value));
  if (!np) return 0;
  ctx->stack = np;
  ctx->cap = cap;
  return 1;
}
static int push(struct VL_Context *ctx, VL_Value v) {
  if (!stack_reserve(ctx, ctx->sp + 1)) return 0;
  ctx->stack[ctx->sp++] = v;
  return 1;
}
static int pop(struct VL_Context *ctx, VL_Value *out) {
  if (ctx->sp == 0) return 0;
  VL_Value v = ctx->stack[--ctx->sp];
  if (out) *out = v;
  return 1;
}
static int peek(const struct VL_Context *ctx, size_t idx_from_top,
                VL_Value *out) {
  if (idx_from_top >= ctx->sp) return 0;
  if (out) *out = ctx->stack[ctx->sp - 1 - idx_from_top];
  return 1;
}

static VL_Status st_io(struct VL_Context *ctx) {
  return ctx ? VL_OK : VL_ERR_INVAL;
}

// Résolution si -> index pool
static uint32_t kstr_index_of(const struct VL_Context *ctx, const char *name) {
  if (!ctx || !name) return UINT32_MAX;
  for (uint32_t i = 0; i < ctx->kcount; i++) {
    if (ctx->kstr[i] && strcmp(ctx->kstr[i], name) == 0) return i;
  }
  return UINT32_MAX;
}

// ───────────────────────── Natives ─────────────────────────
static int nat_reserve(struct VL_Context *ctx, size_t need) {
  if (need <= ctx->ncap) return 1;
  size_t nc = ctx->ncap ? ctx->ncap * 2 : 8;
  while (nc < need) nc += nc / 2;
  VL_Native *np = (VL_Native *)realloc(ctx->nats, nc * sizeof(VL_Native));
  if (!np) return 0;
  ctx->nats = np;
  ctx->ncap = nc;
  return 1;
}

static int nat_find_by_si(const struct VL_Context *ctx, uint32_t si) {
  for (size_t i = 0; i < ctx->nnat; i++)
    if (ctx->nats[i].name_si == si) return (int)i;
  return -1;
}
static int nat_find_by_c(const struct VL_Context *ctx, const char *name) {
  for (size_t i = 0; i < ctx->nnat; i++) {
    const char *n = ctx->nats[i].name_si != UINT32_MAX
                        ? ctx->kstr[ctx->nats[i].name_si]
                        : ctx->nats[i].name_c;
    if (n && strcmp(n, name) == 0) return (int)i;
  }
  return -1;
}

static int nat_bind_si(struct VL_Context *ctx, size_t idx) {
  if (!ctx || idx >= ctx->nnat) return 0;
  if (ctx->nats[idx].name_si != UINT32_MAX) return 1;
  if (!ctx->kstr) return 1;
  uint32_t si = kstr_index_of(ctx, ctx->nats[idx].name_c);
  if (si != UINT32_MAX) ctx->nats[idx].name_si = si;
  return 1;
}

VL_Status vl_register_native(struct VL_Context *ctx, const char *name,
                             VL_Status (*fn)(struct VL_Context *,
                                             const VL_Value *, uint8_t,
                                             VL_Value *, void *),
                             void *ud) {
  if (!ctx || !name || !fn) return VL_ERR_INVAL;
  if (!nat_reserve(ctx, ctx->nnat + 1)) return VL_ERR_OOM;
  VL_Native n;
  n.name_si = UINT32_MAX;
  n.name_c = strdup(name);
  n.fn = fn;
  n.udata = ud;
  ctx->nats[ctx->nnat++] = n;  // binding si au moment attach module
  // essai d'association immédiate si kstr présente
  nat_bind_si(ctx, ctx->nnat - 1);
  return VL_OK;
}

// ───────────────────────── Contexte / module ─────────────────────────
struct VL_Context *vl_ctx_new(void) {
  struct VL_Context *c = (struct VL_Context *)calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->out = stdout;
  return c;
}

void vl_ctx_free(struct VL_Context *c) {
  if (!c) return;
  free(c->stack);
  free(c->globals);
  if (c->nats) {
    for (size_t i = 0; i < c->nnat; i++) free(c->nats[i].name_c);
    free(c->nats);
  }
  free(c);
}

VL_Status vl_ctx_attach_module(struct VL_Context *c, const VL_Module *m) {
  if (!c || !m) return VL_ERR_INVAL;
  c->code = m->code;
  c->code_len = m->code_len;
  c->ip = 0;
  c->kstr = m->kstr;
  c->kcount = m->kcount;  // globals = tableau aligné sur kcount
  free(c->globals);
  c->globals = (VL_Value *)calloc(
      c->kcount > VL_GLOBAL_INIT ? c->kcount : VL_GLOBAL_INIT,
      sizeof(VL_Value));
  if (!c->globals && c->kcount) return VL_ERR_OOM;
  c->gcap = (c->kcount > VL_GLOBAL_INIT ? c->kcount : VL_GLOBAL_INIT);
  // rebinder les natives par si si possible
  for (size_t i = 0; i < c->nnat; i++) nat_bind_si(c, i);
  return VL_OK;
}

void vl_ctx_detach_module(struct VL_Context *c) {
  if (!c) return;
  c->code = NULL;
  c->code_len = 0;
  c->ip = 0;
  c->kstr = NULL;
  c->kcount = 0;
  memset(c->globals, 0, c->gcap * sizeof(VL_Value));
}

// ───────────────────────── Builtins optionnels ─────────────────────────
static VL_Status nat_print(struct VL_Context *ctx, const VL_Value *args,
                           uint8_t argc, VL_Value *ret, void *ud) {
  FILE *out = ud ? (FILE *)ud : (ctx ? ctx->out : stdout);
  for (uint8_t i = 0; i < argc; i++) {
    vl_value_print(&args[i], out);
    if (i + 1 < argc) fputc(' ', out);
  }
  fputc('\n', out);
  if (ret) *ret = vlv_nil();
  return VL_OK;
}

void vl_ctx_register_std(struct VL_Context *c) {
  if (!c) return;
  vl_register_native(c, "print", nat_print, c->out);
}

// ───────────────────────── Exécution ─────────────────────────
static inline void trace_op(struct VL_Context *ctx, uint8_t op, size_t ip0) {
  if (!ctx) return;
  if (!(ctx->trace_mask & VL_TRACE_OP)) return;
  const char *name = vl_op_name(op);
  fprintf(ctx->out ? ctx->out : stdout, "[%08zu] %s\n", ip0, name ? name : "?");
}
static inline void trace_stack(struct VL_Context *ctx) {
  if (!ctx) return;
  if (!(ctx->trace_mask & VL_TRACE_STACK)) return;
  vl_state_dump_stack(ctx, ctx->out);
}

VL_Status vl_step(struct VL_Context *ctx) {
  if (!ctx || !ctx->code) return VL_ERR_INVAL;
  if (ctx->ip >= ctx->code_len) return VL_ERR_BAD_BYTECODE;
  size_t ip0 = ctx->ip;
  uint8_t op = ctx->code[ctx->ip++];
  if (ctx->step_hook) ctx->step_hook(ctx, op, ctx->hook_user);
  trace_op(ctx, op, ip0);

  switch (op) {
    case OP_NOP:
      break;
    case OP_PUSHI: {
      uint64_t u = 0;
      if (!rd_u64(ctx->code, ctx->code_len, &ctx->ip, &u))
        return VL_ERR_BAD_BYTECODE;
      if (!push(ctx, vlv_int((int64_t)u))) return VL_ERR_OOM;
    } break;
    case OP_PUSHF: {
      double d = 0;
      if (!rd_f64(ctx->code, ctx->code_len, &ctx->ip, &d))
        return VL_ERR_BAD_BYTECODE;
      if (!push(ctx, vlv_float(d))) return VL_ERR_OOM;
    } break;
    case OP_PUSHS: {
      uint32_t si = 0;
      if (!rd_u32(ctx->code, ctx->code_len, &ctx->ip, &si))
        return VL_ERR_BAD_BYTECODE;
      if (si >= ctx->kcount) return VL_ERR_BAD_BYTECODE;
      const char *s = ctx->kstr[si];
      uint32_t L = (uint32_t)strlen(s);
      VL_Value v = vl_make_strn(ctx, s, L);
      if (v.type != VT_STR) return VL_ERR_OOM;
      if (!push(ctx, v)) return VL_ERR_OOM;
    } break;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV: {
      VL_Value b, a;
      if (!pop(ctx, &b) || !pop(ctx, &a)) return VL_ERR_BAD_STATE;
      double x, y;
      int isf = 0;
      if (a.type == VT_FLOAT || b.type == VT_FLOAT) {
        isf = 1;
      }
      if (isf) {
        if (!vl_value_as_float(&a, &x) || !vl_value_as_float(&b, &y))
          return VL_ERR_TYPE;
        double r = (op == OP_ADD)   ? x + y
                   : (op == OP_SUB) ? x - y
                   : (op == OP_MUL) ? x * y
                                    : x / y;
        if (!push(ctx, vlv_float(r))) return VL_ERR_OOM;
      } else {
        int64_t ia, ib;
        if (!vl_value_as_int(&a, &ia) || !vl_value_as_int(&b, &ib))
          return VL_ERR_TYPE;
        int64_t r = (op == OP_ADD)   ? ia + ib
                    : (op == OP_SUB) ? ia - ib
                    : (op == OP_MUL) ? ia * ib
                                     : (ib == 0 ? 0 : ia / ib);
        if (!push(ctx, vlv_int(r))) return VL_ERR_OOM;
      }
    } break;
    case OP_EQ:
    case OP_NEQ:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE: {
      VL_Value b, a;
      if (!pop(ctx, &b) || !pop(ctx, &a)) return VL_ERR_BAD_STATE;
      int res = 0;
      if (a.type == VT_STR && b.type == VT_STR &&
          (op == OP_EQ || op == OP_NEQ)) {
        res = (a.as.s && b.as.s)
                  ? (a.as.s == b.as.s ||
                     (a.as.s->len == b.as.s->len &&
                      memcmp(a.as.s->data, b.as.s->data, a.as.s->len) == 0))
                  : (a.as.s == b.as.s);
        if (op == OP_NEQ) res = !res;
      } else {
        double x, y;
        if (!vl_value_as_float(&a, &x) || !vl_value_as_float(&b, &y))
          return VL_ERR_TYPE;
        switch (op) {
          case OP_EQ:
            res = (x == y);
            break;
          case OP_NEQ:
            res = (x != y);
            break;
          case OP_LT:
            res = (x < y);
            break;
          case OP_GT:
            res = (x > y);
            break;
          case OP_LE:
            res = (x <= y);
            break;
          case OP_GE:
            res = (x >= y);
            break;
          default:
            break;
        }
      }
      if (!push(ctx, vlv_bool(res))) return VL_ERR_OOM;
    } break;
    case OP_PRINT: {
      VL_Value v;
      if (!peek(ctx, 0, &v)) return VL_ERR_BAD_STATE;
      vl_value_print(&v, ctx->out ? ctx->out : stdout);
      fputc('\n', ctx->out ? ctx->out : stdout);
    } break;
    case OP_POP: {
      VL_Value tmp;
      if (!pop(ctx, &tmp)) return VL_ERR_BAD_STATE;
      (void)tmp;
    } break;
    case OP_STOREG: {
      uint32_t si = 0;
      if (!rd_u32(ctx->code, ctx->code_len, &ctx->ip, &si))
        return VL_ERR_BAD_BYTECODE;
      if (si >= ctx->gcap) return VL_ERR_BAD_STATE;
      VL_Value v;
      if (!pop(ctx, &v)) return VL_ERR_BAD_STATE;
      ctx->globals[si] = v;
      if (ctx->trace_mask & VL_TRACE_GLOBAL) {
        fprintf(ctx->out ? ctx->out : stdout, "STOREG[%u]=", (unsigned)si);
        vl_value_print(&v, ctx->out ? ctx->out : stdout);
        fputc('\n', ctx->out ? ctx->out : stdout);
      }
    } break;
    case OP_LOADG: {
      uint32_t si = 0;
      if (!rd_u32(ctx->code, ctx->code_len, &ctx->ip, &si))
        return VL_ERR_BAD_BYTECODE;
      if (si >= ctx->gcap) return VL_ERR_BAD_STATE;
      if (!push(ctx, vdup(ctx->globals[si]))) return VL_ERR_OOM;
      if (ctx->trace_mask & VL_TRACE_GLOBAL) {
        fprintf(ctx->out ? ctx->out : stdout, "LOADG[%u]\n", (unsigned)si);
      }
    } break;
    case OP_CALLN: {
      uint32_t si = 0;
      uint8_t argc = 0;
      if (!rd_u32(ctx->code, ctx->code_len, &ctx->ip, &si))
        return VL_ERR_BAD_BYTECODE;
      if (!rd_u8(ctx->code, ctx->code_len, &ctx->ip, &argc))
        return VL_ERR_BAD_BYTECODE;
      // collect args (ordre: a0..a{argc-1}) avec a{argc-1}=top
      VL_Value *args =
          argc ? (VL_Value *)alloca(sizeof(VL_Value) * argc) : NULL;
      for (int i = (int)argc - 1; i >= 0; i--) {
        if (!pop(ctx, &args[i])) return VL_ERR_BAD_STATE;
      }
      int idx = nat_find_by_si(ctx, si);
      if (idx < 0) {  // fallback par nom
        if (si >= ctx->kcount) return VL_ERR_BAD_BYTECODE;
        idx = nat_find_by_c(ctx, ctx->kstr[si]);
        if (idx < 0) return VL_ERR_NOT_FOUND;
      }
      VL_Value ret = vlv_nil();
      VL_Status st =
          ctx->nats[idx].fn(ctx, args, argc, &ret, ctx->nats[idx].udata);
      if (st != VL_OK) return st;
      if (ret.type != VT_NIL) {
        if (!push(ctx, ret)) return VL_ERR_OOM;
      }
      if (ctx->trace_mask & VL_TRACE_CALL) {
        fprintf(ctx->out ? ctx->out : stdout, "CALLN %s/%u\n",
                (si < ctx->kcount ? ctx->kstr[si] : "<bad>"), (unsigned)argc);
      }
    } break;
    case OP_STOREG + 1:
    default:
      return VL_ERR_BAD_BYTECODE;
    case OP_HALT:
      return VL_DONE;
  }

  ctx->steps_total++;
  trace_stack(ctx);
  return VL_OK;
}

VL_Status vl_run(struct VL_Context *ctx, uint64_t max_steps) {
  if (!ctx) return VL_ERR_INVAL;
  VL_Status st = VL_OK;
  uint64_t n = 0;
  while (st == VL_OK) {
    st = vl_step(ctx);
    if (st == VL_DONE) return VL_OK;
    if (st != VL_OK) return st;
    if (max_steps && ++n >= max_steps) break;
  }
  return st;
}

// ───────────────────────── State API ─────────────────────────
size_t vl_state_ip(const struct VL_Context *ctx) { return ctx ? ctx->ip : 0; }
VL_Status vl_state_set_ip(struct VL_Context *ctx, size_t ip) {
  if (!ctx || !ctx->code) return VL_ERR_INVAL;
  size_t sz = vl_insn_size_at(ctx->code, ctx->code_len, ip);
  if (!sz) return VL_ERR_BAD_BYTECODE;
  ctx->ip = ip;
  return VL_OK;
}
const uint8_t *vl_state_code(const struct VL_Context *ctx, size_t *out_len) {
  if (!ctx) return NULL;
  if (out_len) *out_len = ctx->code_len;
  return ctx->code;
}
uint64_t vl_state_steps_total(const struct VL_Context *ctx) {
  return ctx ? ctx->steps_total : 0;
}

size_t vl_state_stack_size(const struct VL_Context *ctx) {
  return ctx ? ctx->sp : 0;
}
int vl_state_stack_peek(const struct VL_Context *ctx, size_t idx,
                        VL_Value *out) {
  return peek(ctx, idx, out);
}
int vl_state_stack_at(const struct VL_Context *ctx, size_t index,
                      VL_Value *out) {
  if (!ctx || index >= ctx->sp) return 0;
  if (out) *out = ctx->stack[index];
  return 1;
}

size_t vl_state_globals_count(const struct VL_Context *ctx) {
  return ctx ? ctx->gcap : 0;
}
int vl_state_global_get(const struct VL_Context *ctx, uint32_t name_si,
                        VL_Value *out) {
  if (!ctx || name_si >= ctx->gcap) return 0;
  if (out) *out = ctx->globals[name_si];
  return 1;
}
VL_Status vl_state_global_set(struct VL_Context *ctx, uint32_t name_si,
                              VL_Value v) {
  if (!ctx || name_si >= ctx->gcap) return VL_ERR_INVAL;
  ctx->globals[name_si] = v;
  return VL_OK;
}

uint32_t vl_state_kstr_count(const struct VL_Context *ctx) {
  return ctx ? ctx->kcount : 0;
}
const char *vl_state_kstr_at(const struct VL_Context *ctx, uint32_t si,
                             uint32_t *out_len) {
  if (!ctx || si >= ctx->kcount) return NULL;
  if (out_len) *out_len = (uint32_t)strlen(ctx->kstr[si]);
  return ctx->kstr[si];
}

uint32_t vl_trace_mask(const struct VL_Context *ctx) {
  return ctx ? ctx->trace_mask : 0;
}
void vl_trace_enable(struct VL_Context *ctx, uint32_t mask) {
  if (ctx) ctx->trace_mask |= mask;
}
void vl_trace_disable(struct VL_Context *ctx, uint32_t mask) {
  if (ctx) ctx->trace_mask &= ~mask;
}
void vl_set_step_hook(struct VL_Context *ctx, VL_StepHook hook, void *user) {
  if (ctx) {
    ctx->step_hook = hook;
    ctx->hook_user = user;
  }
}

VL_Status vl_state_dump_stack(const struct VL_Context *ctx, FILE *out) {
  if (!ctx) return VL_ERR_INVAL;
  if (!out) out = stdout;
  fprintf(out, "-- stack size=%zu --\n", ctx->sp);
  for (size_t i = 0; i < ctx->sp; i++) {
    fprintf(out, "[%zu] ", i);
    vl_value_print(&ctx->stack[i], out);
    fputc('\n', out);
  }
  return VL_OK;
}

VL_Status vl_state_dump(const struct VL_Context *ctx, FILE *out,
                        uint32_t mask) {
  if (!ctx) return VL_ERR_INVAL;
  if (!out) out = stdout;
  size_t ip = ctx->ip;
  fprintf(out, "== VM state ==\nIP=%zu/%zu steps=%" PRIu64 "\n", ip,
          ctx->code_len, ctx->steps_total);  // fenêtre d'instruction
  if (ctx->code && ip < ctx->code_len) {
    char line[128];
    size_t insz = vl_insn_size_at(ctx->code, ctx->code_len, ip);
    vl_disasm_one(ctx->code, ctx->code_len, ip, line, sizeof(line));
    fprintf(out, "%04zu: %-16s  ", ip, line);
    for (size_t j = 0; j < insz; j++) fprintf(out, "%02X ", ctx->code[ip + j]);
    fputc('\n', out);
  }
  if (mask & VL_TRACE_STACK) vl_state_dump_stack(ctx, out);
  return VL_OK;
}

// ───────────────────────── Mini driver de test ─────────────────────────
#ifdef VL_VM_TEST_MAIN
int main(void) {
  // Construire un module trivial avec opcodes helpers
  const char *kstr[] = {"hello", "print", "x"};
  uint8_t buf[512];
  uint8_t *p = buf;
  vl_bc_emit_header(&p, (uint8_t)VLBC_VERSION);
  vl_bc_emit_kstr(&p, kstr, 3);
  uint8_t *code_size = vl_bc_begin_code(&p);
  uint8_t *cbeg = p;
  vl_emit_PUSHS(&p, 0);     // "hello"
  vl_emit_CALLN(&p, 1, 1);  // print(1)
  vl_emit_HALT(&p);
  uint8_t *cend = p;
  vl_bc_end_code(code_size, cbeg, cend);

  // Décoder le VLBC avec undump
  VL_Module mod;
  char err[128];
  VL_Status st =
      vl_module_from_buffer(buf, (size_t)(p - buf), &mod, err, sizeof(err));
  if (st != VL_OK) {
    fprintf(stderr, "undump: %s\n", err);
    return 1;
  }

  struct VL_Context *ctx = vl_ctx_new();
  vl_ctx_register_std(ctx);
  vl_ctx_attach_module(ctx, &mod);
  vl_trace_enable(ctx, VL_TRACE_OP);
  st = vl_run(ctx, 0);
  if (st != VL_OK) {
    fprintf(stderr, "run: %d\n", st);
  }
  vl_ctx_free(ctx);
  vl_module_free(&mod);
  return 0;
}
#endif
