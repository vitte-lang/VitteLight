/* ============================================================================
   vm.c — Machine virtuelle stack-based pour bytecode VTBC (C17, portable)
   - Boucle d’exécution avec dispatch en switch
   - Pile de valeurs, frames d’appel, globals, constantes
   - Arithmétique entière/flottante, comparaisons, sauts, tables/strings
   - Appels natifs (C) et fonctions bytecode, variadiques simples
   - Chargement d’images via undump.{h,c} ("VTBC": CODE,KCON,STRS,SYMS,FUNC)
   - Gestion des erreurs, pas de dépendances système non-portables
   - Licence: MIT.
   ============================================================================
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Headers du projet (tous optionnels mais recommandés) ----------------- */
#if defined(__has_include)
#if __has_include("debug.h")
#include "debug.h"
#endif
#if __has_include("object.h")
#include "object.h"
#endif
#if __has_include("string.h")
#include "string.h"
#endif
#if __has_include("table.h")
#include "table.h"
#endif
#if __has_include("gc.h")
#include "gc.h"
#endif
#if __has_include("undump.h")
#include "undump.h"
#endif
#if __has_include("opcodes.h")
#include "opcodes.h"
#endif
#if __has_include("state.h")
#include "state.h"
#endif
#endif

/* --- Fallbacks si certains headers ne sont pas encore disponibles ----------
 */
#ifndef VT_DEBUG
#define VT_TRACE(...) ((void)0)
#define VT_DEBUG(...) ((void)0)
#define VT_INFO(...) ((void)0)
#define VT_WARN(...) ((void)0)
#define VT_ERROR(...) ((void)0)
#define VT_FATAL(...)                           \
  do {                                          \
    fprintf(stderr, "FATAL: vm: " __VA_ARGS__); \
    fputc('\n', stderr);                        \
    abort();                                    \
  } while (0)
#endif

#ifndef VT_MALLOC
#define VT_MALLOC(sz) malloc(sz)
#define VT_FREE(p) free(p)
#define VT_REALLOC(p, sz) realloc((p), (sz))
#endif

/* Valeur dynamique de base, si object.h indisponible.
   Adapter à votre representation réelle le moment venu. */
#ifndef VT_OBJECT_H
typedef enum {
  VT_T_NIL = 0,
  VT_T_BOOL,
  VT_T_INT,
  VT_T_FLOAT,
  VT_T_STR,
  VT_T_NATIVE,
  VT_T_OBJ
} vt_type;

typedef struct vt_string_stub {
  size_t len;
  char* data;
} vt_string_stub;

typedef struct vt_value {
  uint8_t t;
  union {
    int64_t i;
    double f;
    int b;
    vt_string_stub* s;
    void* p;
  };
} vt_value;

static inline vt_value vt_nil(void) {
  vt_value v;
  v.t = VT_T_NIL;
  v.i = 0;
  return v;
}
static inline vt_value vt_bool(int b) {
  vt_value v;
  v.t = VT_T_BOOL;
  v.b = !!b;
  return v;
}
static inline vt_value vt_int(int64_t x) {
  vt_value v;
  v.t = VT_T_INT;
  v.i = x;
  return v;
}
static inline vt_value vt_float(double x) {
  vt_value v;
  v.t = VT_T_FLOAT;
  v.f = x;
  return v;
}
static inline int vt_truthy(vt_value v) {
  switch (v.t) {
    case VT_T_NIL:
      return 0;
    case VT_T_BOOL:
      return v.b;
    case VT_T_INT:
      return v.i != 0;
    case VT_T_FLOAT:
      return v.f != 0.0;
    default:
      return 1;
  }
}
static inline const char* vt_typename(uint8_t t) {
  switch (t) {
    case VT_T_NIL:
      return "nil";
    case VT_T_BOOL:
      return "bool";
    case VT_T_INT:
      return "int";
    case VT_T_FLOAT:
      return "float";
    case VT_T_STR:
      return "str";
    case VT_T_NATIVE:
      return "native";
    default:
      return "obj";
  }
}
#endif /* VT_OBJECT_H */

/* Opcodes fallback si opcodes.h pas encore écrit. Ajustez pour votre ISA. */
#ifndef VT_OPCODES_H
typedef enum {
  OP_HALT = 0,
  OP_CONST, /* u16 kidx                   -> push KCON[kidx]        */
  OP_POP,   /*                            -> pop                     */
  OP_DUP,   /*                            -> dup top                 */
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_NEG,
  OP_NOT,
  OP_EQ,
  OP_NE,
  OP_LT,
  OP_LE,
  OP_GT,
  OP_GE,
  OP_JMP,       /* s32 rel                    -> pc += rel               */
  OP_JMP_IF,    /* s32 rel  (pop cond)        -> if cond pc+=rel         */
  OP_JMP_IFNOT, /* s32 rel                    -> if !cond pc+=rel        */
  OP_LOADG,     /* u16 sym                    -> push globals[sym]       */
  OP_STOREG,    /* u16 sym  (pop v)           -> globals[sym]=v          */
  OP_LOADL,     /* u8 slot                    -> push locals[base+slot]  */
  OP_STOREL,    /* u8 slot (pop v)            -> locals[base+slot]=v     */
  OP_CALL,      /* u8 argc                    -> call top-argc-...       */
  OP_RET,       /* u8 nret                    -> return nret             */
  OP_PRINT,     /* (pop v) print              -> side effect             */
  OP_CONV_I2F,  /* (int -> float) */
  OP_CONV_F2I,  /* (float -> int, trunc) */
  OP_ASSERT,    /* (pop cond,stridx) if !cond -> runtime error           */
  OP_NARGS,     /* push argc courant (variadique) */
  OP__COUNT
} vt_opcode;
#endif

/* -------------------------------------------------------------------------- */
/*                              VM DEFINITIONS */
/* -------------------------------------------------------------------------- */

typedef vt_value (*vt_cfunc)(struct vt_vm* vm, int argc, vt_value* argv);

typedef struct vt_global {
  /* mapping symbol -> value: dans une vraie impl, utilisez table.h */
  vt_value v;
  int inited;
} vt_global;

typedef struct vt_chunk {
  const uint8_t* code;
  size_t size;
} vt_chunk;

typedef struct vt_frame {
  const uint8_t* code;
  size_t code_sz;
  const vt_value* kcon; /* pool des constantes typées */
  size_t kcon_sz;
  int base;   /* base index dans la pile */
  int pc;     /* compteur en octets */
  int ret_sp; /* pile sp de retour */
} vt_frame;

typedef struct vt_vm {
  /* Pile de valeurs */
  vt_value* stack;
  int sp; /* next free index */
  int cap;

  /* Frames d'appel */
  vt_frame* frames;
  int fp; /* next free frame index */
  int fcap;

  /* Globals */
  vt_global* globals;
  size_t gcount;

  /* Constantes (kcon) décodées en vt_value si dispo, sinon brutes */
  vt_value* kcon;
  size_t kcount;

  /* Image chargée */
#ifdef VT_UNDUMP_H
  vt_img* image;
#endif

  /* Strings/STRS brutes pour messages */
  const uint8_t* strs;
  size_t strs_sz;

  /* Limites d’exécution */
  uint64_t step_limit;
  uint64_t steps;

  /* Dernière erreur */
  char errmsg[256];

  /* Natives (table simple indexée par symbole si souhaité) */
  vt_cfunc* natives;
  size_t n_natives;
} vt_vm;

/* -------------------------------------------------------------------------- */
/*                              UTILITAIRES */
/* -------------------------------------------------------------------------- */

static inline void vm_set_err(vt_vm* vm, const char* msg) {
  if (!vm) return;
  size_t n = strlen(msg);
  if (n >= sizeof(vm->errmsg)) n = sizeof(vm->errmsg) - 1;
  memcpy(vm->errmsg, msg, n);
  vm->errmsg[n] = 0;
}

static inline void vm_oob(void) { VT_FATAL("bytecode out of bounds"); }

/* Lecture little-endian depuis code */
static inline uint8_t rd_u8(const uint8_t* p) { return p[0]; }
static inline int8_t rd_i8(const uint8_t* p) { return (int8_t)p[0]; }
static inline uint16_t rd_u16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static inline int16_t rd_i16(const uint8_t* p) { return (int16_t)rd_u16(p); }
static inline uint32_t rd_u32(const uint8_t* p) {
  return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static inline int32_t rd_i32(const uint8_t* p) { return (int32_t)rd_u32(p); }

/* Pile */
static inline int vm_stack_ok(vt_vm* vm, int need) {
  if (vm->sp + need <= vm->cap) return 1;
  int newcap = vm->cap ? vm->cap * 2 : 128;
  while (vm->sp + need > newcap) newcap *= 2;
  vt_value* s2 =
      (vt_value*)VT_REALLOC(vm->stack, (size_t)newcap * sizeof(vt_value));
  if (!s2) return 0;
  vm->stack = s2;
  vm->cap = newcap;
  return 1;
}
static inline void push(vt_vm* vm, vt_value v) {
  if (!vm_stack_ok(vm, 1)) VT_FATAL("OOM stack");
  vm->stack[vm->sp++] = v;
}
static inline vt_value pop(vt_vm* vm) {
  if (vm->sp <= 0) VT_FATAL("stack underflow");
  return vm->stack[--vm->sp];
}
static inline vt_value* topn(vt_vm* vm, int n) { /* 0=top-1 */
  if (vm->sp - 1 - n < 0) VT_FATAL("stack access OOB");
  return &vm->stack[vm->sp - 1 - n];
}

/* Frames */
static inline int vm_frames_ok(vt_vm* vm, int need) {
  if (vm->fp + need <= vm->fcap) return 1;
  int newcap = vm->fcap ? vm->fcap * 2 : 16;
  while (vm->fp + need > newcap) newcap *= 2;
  vt_frame* f2 =
      (vt_frame*)VT_REALLOC(vm->frames, (size_t)newcap * sizeof(vt_frame));
  if (!f2) return 0;
  vm->frames = f2;
  vm->fcap = newcap;
  return 1;
}

/* Constantes KCON par défaut: entier signé 64, double, bool, nil, string.
   Ici on suppose que KCON a été décodé en vt_value ailleurs. */
static inline vt_value kget(vt_vm* vm, uint16_t idx) {
  if (idx >= vm->kcount) VT_FATAL("kconst index OOB");
  return vm->kcon[idx];
}

/* Globals simples indexées par symbole id. */
static inline vt_value gget(vt_vm* vm, uint16_t sym) {
  if (sym >= vm->gcount || !vm->globals[sym].inited) return vt_nil();
  return vm->globals[sym].v;
}
static inline void gset(vt_vm* vm, uint16_t sym, vt_value v) {
  if (sym >= vm->gcount) {
    size_t newc = vm->gcount ? vm->gcount * 2 : 64;
    while (sym >= newc) newc *= 2;
    vt_global* g2 =
        (vt_global*)VT_REALLOC(vm->globals, newc * sizeof(vt_global));
    if (!g2) VT_FATAL("OOM globals");
    /* init tail */
    for (size_t i = vm->gcount; i < newc; i++) {
      g2[i].inited = 0;
      g2[i].v = vt_nil();
    }
    vm->globals = g2;
    vm->gcount = newc;
  }
  vm->globals[sym].v = v;
  vm->globals[sym].inited = 1;
}

/* Conversions dynamiques */
static inline int is_num(vt_value v) {
  return v.t == VT_T_INT || v.t == VT_T_FLOAT;
}
static inline double as_f(vt_value v) {
  return (v.t == VT_T_FLOAT) ? v.f : (double)v.i;
}
static inline vt_value add_num(vt_value a, vt_value b) {
  if (a.t == VT_T_FLOAT || b.t == VT_T_FLOAT)
    return vt_float(as_f(a) + as_f(b));
  return vt_int(a.i + b.i);
}
static inline vt_value sub_num(vt_value a, vt_value b) {
  if (a.t == VT_T_FLOAT || b.t == VT_T_FLOAT)
    return vt_float(as_f(a) - as_f(b));
  return vt_int(a.i - b.i);
}
static inline vt_value mul_num(vt_value a, vt_value b) {
  if (a.t == VT_T_FLOAT || b.t == VT_T_FLOAT)
    return vt_float(as_f(a) * as_f(b));
  return vt_int(a.i * b.i);
}
static inline vt_value div_num(vt_value a, vt_value b) {
  double denom = as_f(b);
  if (denom == 0.0) VT_FATAL("division by zero");
  return vt_float(as_f(a) / denom);
}
static inline vt_value mod_num(vt_value a, vt_value b) {
  if (a.t == VT_T_FLOAT || b.t == VT_T_FLOAT)
    return vt_float(fmod(as_f(a), as_f(b)));
  if (b.i == 0) VT_FATAL("mod by zero");
  return vt_int(a.i % b.i);
}

/* Impression simple pour OP_PRINT (debug). Adapter à votre String API. */
static void vm_print_value(vt_value v) {
  switch (v.t) {
    case VT_T_NIL:
      fputs("nil", stdout);
      break;
    case VT_T_BOOL:
      fputs(v.b ? "true" : "false", stdout);
      break;
    case VT_T_INT:
      printf("%lld", (long long)v.i);
      break;
    case VT_T_FLOAT:
      printf("%.17g", v.f);
      break;
    case VT_T_STR:
      if (v.s && v.s->data)
        fwrite(v.s->data, 1, v.s->len, stdout);
      else
        fputs("<str?>", stdout);
      break;
    default:
      printf("<%s@%p>", vt_typename(v.t), (void*)v.p);
      break;
  }
}

/* -------------------------------------------------------------------------- */
/*                              API PUBLIQUE VM */
/* -------------------------------------------------------------------------- */

typedef struct {
  int dummy;
} vt_vm_config;

vt_vm* vt_vm_new(const vt_vm_config* cfg) {
  (void)cfg;
  vt_vm* vm = (vt_vm*)calloc(1, sizeof *vm);
  if (!vm) return NULL;
  vm->cap = vm->fcap = 0;
  vm->stack = NULL;
  vm->frames = NULL;
  vm->sp = vm->fp = 0;
  vm->gcount = 0;
  vm->globals = NULL;
  vm->kcon = NULL;
  vm->kcount = 0;
  vm->n_natives = 0;
  vm->natives = NULL;
  vm->strs = NULL;
  vm->strs_sz = 0;
  vm->step_limit = 0;
  vm->steps = 0;
  vm->errmsg[0] = 0;
  return vm;
}

void vt_vm_free(vt_vm* vm) {
  if (!vm) return;
  VT_FREE(vm->stack);
  VT_FREE(vm->frames);
  VT_FREE(vm->globals);
  VT_FREE(vm->kcon);
  VT_FREE(vm->natives);
#ifdef VT_UNDUMP_H
  if (vm->image) vt_img_release(vm->image);
#endif
  free(vm);
}

/* Enregistre une native dans un slot symbol (option simple). */
int vt_vm_set_native(vt_vm* vm, uint16_t sym, vt_cfunc fn) {
  if (!vm || !fn) return -EINVAL;
  /* On schématise: stocker la fonction comme global VT_T_NATIVE via pointeur.
   */
  vt_value v;
  v.t = VT_T_NATIVE;
  v.p = (void*)fn;
  gset(vm, sym, v);
  return 0;
}

/* Charge une image VTBC depuis un chemin. Remplit CODE/KCON/STRS. */
int vt_vm_load_image(vt_vm* vm, const char* path) {
#ifndef VT_UNDUMP_H
  (void)vm;
  (void)path;
  vm_set_err(vm, "undump.h absent");
  return -ENOSYS;
#else
  if (!vm || !path) return -EINVAL;
  if (vm->image) {
    vt_img_release(vm->image);
    vm->image = NULL;
  }
  int rc = vt_img_load_file(path, &vm->image);
  if (rc != 0) {
    vm_set_err(vm, "load image failed");
    return rc;
  }

  /* Map sections */
  const uint8_t *code = NULL, *strs = NULL;
  size_t code_sz = 0, strs_sz = 0;
  vt_img_find(vm->image, (const char[4]){'C', 'O', 'D', 'E'}, &code, &code_sz);
  vt_img_find(vm->image, (const char[4]){'S', 'T', 'R', 'S'}, &strs, &strs_sz);
  vm->strs = strs;
  vm->strs_sz = strs_sz;

  /* KCON: dans une impl réelle, KCON contient un flux sérialisé → décoder.
     Ici on suppose KCON = tableau de vt_value matérialisé (didactique). */
  const uint8_t* kraw = NULL;
  size_t kraw_sz = 0;
  if (vt_img_find(vm->image, (const char[4]){'K', 'C', 'O', 'N'}, &kraw,
                  &kraw_sz) == 0) {
    /* Format jouet: [u32 count][count x {u8 tag, union payload}] */
    if (kraw_sz < 4) VT_FATAL("KCON too small");
    uint32_t cnt = rd_u32(kraw);
    size_t off = 4;
    vm->kcon = (vt_value*)VT_MALLOC(cnt * sizeof(vt_value));
    if (!vm->kcon) VT_FATAL("OOM kcon");
    vm->kcount = cnt;
    for (uint32_t i = 0; i < cnt; i++) {
      if (off >= kraw_sz) VT_FATAL("KCON OOB");
      uint8_t tag = kraw[off++];
      switch (tag) {
        case 0:
          vm->kcon[i] = vt_nil();
          break;
        case 1:
          if (off + 8 > kraw_sz) VT_FATAL("KCON int OOB");
          {
            int64_t x;
            memcpy(&x, kraw + off, 8);
            off += 8;
            vm->kcon[i] = vt_int(x);
          }
          break;
        case 2:
          if (off + 8 > kraw_sz) VT_FATAL("KCON float OOB");
          {
            double d;
            memcpy(&d, kraw + off, 8);
            off += 8;
            vm->kcon[i] = vt_float(d);
          }
          break;
        case 3: { /* string: u32 len + bytes */
          if (off + 4 > kraw_sz) VT_FATAL("KCON str hdr OOB");
          uint32_t n = rd_u32(kraw + off);
          off += 4;
          if (off + n > kraw_sz) VT_FATAL("KCON str bytes OOB");
          vt_string_stub* s = (vt_string_stub*)VT_MALLOC(sizeof(*s));
          s->len = n;
          s->data = (char*)VT_MALLOC(n + 1);
          memcpy(s->data, kraw + off, n);
          s->data[n] = 0;
          off += n;
          vt_value v;
          v.t = VT_T_STR;
          v.s = s;
          vm->kcon[i] = v;
        } break;
        case 4: /* bool */
          if (off >= kraw_sz) VT_FATAL("KCON bool OOB");
          vm->kcon[i] = vt_bool(kraw[off++] != 0);
          break;
        default:
          VT_FATAL("KCON unknown tag");
      }
    }
  } else {
    vm->kcon = NULL;
    vm->kcount = 0;
  }

  /* CODE: on crée un frame racine qui pointe sur le code module principal. */
  if (code && code_sz > 0) {
    if (!vm_frames_ok(vm, 1)) VT_FATAL("OOM frames");
    vm->frames[vm->fp++] = (vt_frame){.code = code,
                                      .code_sz = code_sz,
                                      .kcon = vm->kcon,
                                      .kcon_sz = vm->kcount,
                                      .base = 0,
                                      .pc = 0,
                                      .ret_sp = 0};
  }
  return 0;
#endif
}

/* Exécute jusqu’à HALT ou fin code. step_limit=0 => illimité. */
int vt_vm_run(vt_vm* vm, uint64_t step_limit) {
  if (!vm) return -EINVAL;
  vm->step_limit = step_limit;
  vm->steps = 0;
  if (vm->fp <= 0) return 0;

  vt_frame* f = &vm->frames[vm->fp - 1];

#define FETCH1() \
  ((f->pc < 0 || (size_t)f->pc >= f->code_sz) ? vm_oob(), 0 : f->code[f->pc++])
#define FETCH2U()                 \
  ({                              \
    uint16_t x;                   \
    uint8_t a = FETCH1();         \
    uint8_t b = FETCH1();         \
    x = (uint16_t)(a | (b << 8)); \
    x;                            \
  })
#define FETCH4S()                                                   \
  ({                                                                \
    int32_t x;                                                      \
    uint8_t a = FETCH1(), b = FETCH1(), c = FETCH1(), d = FETCH1(); \
    x = (int32_t)(a | (b << 8) | (c << 16) | (d << 24));            \
    x;                                                              \
  })

  for (;;) {
    if (vm->step_limit && vm->steps++ >= vm->step_limit) {
      vm_set_err(vm, "step limit reached");
      return -EAGAIN;
    }
    uint8_t op = FETCH1();

    switch (op) {
      case OP_HALT:
        VT_TRACE("HALT");
        return 0;

      case OP_CONST: {
        uint16_t k = FETCH2U();
        push(vm, kget(vm, k));
      } break;

      case OP_POP:
        (void)pop(vm);
        break;
      case OP_DUP: {
        vt_value v = *topn(vm, 0);
        push(vm, v);
      } break;

      case OP_NEG: {
        vt_value a = pop(vm);
        if (a.t == VT_T_INT)
          push(vm, vt_int(-a.i));
        else
          push(vm, vt_float(-as_f(a)));
      } break;

      case OP_NOT: {
        vt_value a = pop(vm);
        push(vm, vt_bool(!vt_truthy(a)));
      } break;

      case OP_ADD: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, add_num(a, b));
      } break;
      case OP_SUB: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, sub_num(a, b));
      } break;
      case OP_MUL: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, mul_num(a, b));
      } break;
      case OP_DIV: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, div_num(a, b));
      } break;
      case OP_MOD: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, mod_num(a, b));
      } break;

      case OP_EQ: {
        vt_value b = pop(vm), a = pop(vm);
        int eq = 0;
        if (a.t == b.t) {
          switch (a.t) {
            case VT_T_NIL:
              eq = 1;
              break;
            case VT_T_BOOL:
              eq = (a.b == b.b);
              break;
            case VT_T_INT:
              eq = (a.i == b.i);
              break;
            case VT_T_FLOAT:
              eq = (a.f == b.f);
              break;
            case VT_T_STR:
              eq = (a.s && b.s && a.s->len == b.s->len &&
                    memcmp(a.s->data, b.s->data, a.s->len) == 0);
              break;
            default:
              eq = (a.p == b.p);
              break;
          }
        } else if (is_num(a) && is_num(b)) {
          eq = (as_f(a) == as_f(b));
        } else
          eq = 0;
        push(vm, vt_bool(eq));
      } break;
      case OP_NE: {
        vt_value b = pop(vm), a = pop(vm); /* reuse EQ then not */
        int eq = 0;
        if (a.t == b.t) {
          switch (a.t) {
            case VT_T_NIL:
              eq = 1;
              break;
            case VT_T_BOOL:
              eq = (a.b == b.b);
              break;
            case VT_T_INT:
              eq = (a.i == b.i);
              break;
            case VT_T_FLOAT:
              eq = (a.f == b.f);
              break;
            case VT_T_STR:
              eq = (a.s && b.s && a.s->len == b.s->len &&
                    memcmp(a.s->data, b.s->data, a.s->len) == 0);
              break;
            default:
              eq = (a.p == b.p);
              break;
          }
        } else if (is_num(a) && is_num(b)) {
          eq = (as_f(a) == as_f(b));
        } else
          eq = 0;
        push(vm, vt_bool(!eq));
      } break;
      case OP_LT: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, vt_bool(as_f(a) < as_f(b)));
      } break;
      case OP_LE: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, vt_bool(as_f(a) <= as_f(b)));
      } break;
      case OP_GT: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, vt_bool(as_f(a) > as_f(b)));
      } break;
      case OP_GE: {
        vt_value b = pop(vm), a = pop(vm);
        push(vm, vt_bool(as_f(a) >= as_f(b)));
      } break;

      case OP_JMP: {
        int32_t rel = FETCH4S();
        f->pc += rel;
        if ((size_t)f->pc > f->code_sz) vm_oob();
      } break;
      case OP_JMP_IF: {
        int32_t rel = FETCH4S();
        vt_value c = pop(vm);
        if (vt_truthy(c)) {
          f->pc += rel;
          if ((size_t)f->pc > f->code_sz) vm_oob();
        }
      } break;
      case OP_JMP_IFNOT: {
        int32_t rel = FETCH4S();
        vt_value c = pop(vm);
        if (!vt_truthy(c)) {
          f->pc += rel;
          if ((size_t)f->pc > f->code_sz) vm_oob();
        }
      } break;

      case OP_LOADG: {
        uint16_t s = FETCH2U();
        push(vm, gget(vm, s));
      } break;
      case OP_STOREG: {
        uint16_t s = FETCH2U();
        vt_value v = pop(vm);
        gset(vm, s, v);
      } break;

      case OP_LOADL: {
        uint8_t slot = FETCH1();
        int idx = f->base + (int)slot;
        if (idx < 0 || idx >= vm->sp) VT_FATAL("LOADL OOB");
        push(vm, vm->stack[idx]);
      } break;
      case OP_STOREL: {
        uint8_t slot = FETCH1();
        int idx = f->base + (int)slot;
        if (idx < 0 || idx >= vm->sp) VT_FATAL("STOREL OOB");
        vm->stack[idx] = pop(vm);
      } break;

      case OP_CONV_I2F: {
        vt_value a = pop(vm);
        if (a.t == VT_T_FLOAT)
          push(vm, a);
        else if (a.t == VT_T_INT)
          push(vm, vt_float((double)a.i));
        else
          VT_FATAL("I2F on non-number");
      } break;
      case OP_CONV_F2I: {
        vt_value a = pop(vm);
        if (a.t == VT_T_INT)
          push(vm, a);
        else if (a.t == VT_T_FLOAT)
          push(vm, vt_int((int64_t)a.f));
        else
          VT_FATAL("F2I on non-number");
      } break;

      case OP_ASSERT: {
        /* Format exemple: u16 str_idx (dans STRS), pop cond */
        uint16_t sidx = FETCH2U();
        vt_value cond = pop(vm);
        if (!vt_truthy(cond)) {
          const char* msg = "<assert>";
          if (vm->strs && sidx < vm->strs_sz) {
            const char* z = (const char*)vm->strs + sidx;
            msg = z;
          }
          vm_set_err(vm, msg);
          VT_ERROR("assert failed: %s", msg);
          return -EINVAL;
        }
      } break;

      case OP_PRINT: {
        vt_value v = pop(vm);
        vm_print_value(v);
        fputc('\n', stdout);
      } break;

      case OP_NARGS: {
        /* Pousse un nombre arbitraire: ici on ne stocke pas argc implicite,
           donc on met 0 par défaut. Adapter si vous encodez argc dans frame. */
        push(vm, vt_int(0));
      } break;

      case OP_CALL: {
        uint8_t argc = FETCH1();
        /* Convention: pile [..., callee, arg0, arg1, ... argN-1]
           On supporte deux types de callee: native (VT_T_NATIVE) et
           fonction bytecode représentée par une adresse PC + base. Dans
           cette version didactique, on suppose callee est un global VT_T_NATIVE
           ou un KCON VT_T_NATIVE. */
        if (vm->sp < 1 + argc) VT_FATAL("CALL stack underflow");
        vt_value* argv = &vm->stack[vm->sp - argc];
        vt_value callee = vm->stack[vm->sp - argc - 1];

        if (callee.t == VT_T_NATIVE) {
          vt_cfunc fn = (vt_cfunc)callee.p;
          vt_value ret = fn(vm, argc, argv);
          /* pop callee + args */
          vm->sp -= (1 + argc);
          push(vm, ret);
        } else {
          /* Bytecode call: on attend que callee soit un entier KCON indiquant
             PC, ou on pourrait avoir un proto structuré dans FUNC. Par
             simplicité: callee INT = offset relatif en octets dans le même
             chunk. */
          int new_base = vm->sp - argc; /* arguments deviennent locals[0..] */
          int ret_sp = new_base - 1; /* position de callee, remplacée par ret */
          int32_t entry_pc = 0;
          if (callee.t == VT_T_INT)
            entry_pc = (int32_t)callee.i;
          else
            VT_FATAL("bytecode call target unsupported");

          if (!vm_frames_ok(vm, 1)) VT_FATAL("OOM frames");
          /* Empiler frame courant et passer sur le nouveau */
          vt_frame nf = *f;
          nf.base = new_base;
          nf.ret_sp = ret_sp;
          nf.pc = entry_pc;
          vm->frames[vm->fp++] = nf;
          f = &vm->frames[vm->fp - 1];
          /* Continuer dans la boucle */
        }
      } break;

      case OP_RET: {
        uint8_t nret = FETCH1();
        vt_value rv = vt_nil();
        if (nret > 0) {
          if (vm->sp <= 0) VT_FATAL("RET stack underflow");
          rv = pop(vm);
        }
        if (vm->fp <= 1) {
          /* Retour de frame racine => HALT implicite */
          if (nret > 0) push(vm, rv);
          return 0;
        }
        /* Revenir au caller */
        vt_frame done = vm->frames[--vm->fp];
        f = &vm->frames[vm->fp - 1]; /* caller frame */
        /* Restaure sp au ret_sp et poser la valeur de retour */
        vm->sp = done.ret_sp;
        if (nret > 0) push(vm, rv);
      } break;

      default:
        VT_FATAL("unknown opcode %u at pc=%d", (unsigned)op, f->pc - 1);
    }
  }

#undef FETCH1
#undef FETCH2U
#undef FETCH4S
}

/* Expose une fonction utilitaire pour appeler une native directe. */
vt_value vt_make_native(vt_cfunc fn) {
  vt_value v;
  v.t = VT_T_NATIVE;
  v.p = (void*)fn;
  return v;
}

/* Exemple de native: print(args...) → nil */
static vt_value vt_native_print(vt_vm* vm, int argc, vt_value* argv) {
  (void)vm;
  for (int i = 0; i < argc; i++) {
    if (i) fputc(' ', stdout);
    vm_print_value(argv[i]);
  }
  fputc('\n', stdout);
  return vt_nil();
}

/* Option de démarrage simple si vous souhaitez tester en exécutable. */
#ifdef VM_MAIN
int main(int argc, char** argv) {
  vt_vm* vm = vt_vm_new(NULL);
  if (!vm) {
    fprintf(stderr, "OOM\n");
    return 1;
  }
  /* Registre une native sur le symbole 0 (exemple) */
  vt_vm_set_native(vm, 0, vt_native_print);

#ifdef VT_UNDUMP_H
  if (argc > 1) {
    int rc = vt_vm_load_image(vm, argv[1]);
    if (rc != 0) {
      fprintf(stderr, "load error: %s (%d)\n", vm->errmsg, rc);
      return 2;
    }
  } else {
    fprintf(stderr, "usage: %s <image.vtbc>\n", argv[0]);
    return 2;
  }
  int rc = vt_vm_run(vm, 0);
  if (rc != 0) {
    fprintf(stderr, "runtime error: %s (%d)\n", vm->errmsg, rc);
  }
#else
  fprintf(stderr, "undump.h manquant. Re-compiler avec le loader.\n");
#endif
  vt_vm_free(vm);
  return 0;
}
#endif
