// vitte-light/core/gc.c
// Collecteur minimal optionnel pour VitteLight (marquage → balayage des
// chaînes). Objectif: gérer la vie des VL_String allouées dynamiquement par la
// VM, avec un registre externe au contexte et un balayage basé sur les racines
// visibles (pile, globaux, constantes VLBC, clés de maps). Sans intrusion dans
// api.c.
//
// ⚠️ Sécurité: par défaut, ce GC NE LIBÈRE PAS la mémoire des chaînes (mode
// "observateur"). Activez la libération en définissant l'ownership via
// vl_gc_set_ownership(ctx, true) ET en enregistrant chaque chaîne créée par la
// VM avec vl_gc_register_string(...). Sans ces deux conditions, le GC ne libère
// rien pour éviter les doubles free.
//
// API (implémentée ici; créez gc.h si besoin):
//   void      vl_gc_attach(struct VL_Context *ctx, size_t trigger_bytes);
//   void      vl_gc_detach(struct VL_Context *ctx);
//   void      vl_gc_set_ownership(struct VL_Context *ctx, bool own_strings);
//   void      vl_gc_register_string(struct VL_Context *ctx, struct VL_String
//   *s); VL_Status vl_gc_collect(struct VL_Context *ctx, int flags); void
//   vl_gc_stats(struct VL_Context *ctx, size_t *tracked, size_t *bytes, size_t
//   *frees);
//   // util debug
//   void      vl_gc_preindex_existing(struct VL_Context *ctx); // indexe
//   racines actuelles
//
// Flags vl_gc_collect:
//   0           : défaut (full mark-sweep)
//   1 (VERBOSE) : log minimal sur stderr
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/gc.c
// Link avec: api.c, ctype.c

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"

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
  VL_Map natives;  // natives keys = names (VL_String*)
};

// ───────────────────────── Registre GC par-contexte ─────────────────────────

typedef struct {
  VL_String *ptr;
  uint32_t marked : 1;
  uint32_t owned : 1;  // libérable par le GC
  uint32_t pad : 30;
  uint32_t size;  // approx (sizeof+len+1)
} GCNode;

typedef struct {
  GCNode *nodes;
  size_t len;
  size_t cap;
  size_t bytes;  // total estimé
  size_t freed_bytes;
  size_t freed_count;
  size_t trigger_bytes;  // seuil soft
  int own_strings;       // comportement par défaut de libération
} GCState;

typedef struct {
  struct VL_Context *ctx;
  GCState st;
} GCEntry;

static GCEntry *g_entries = NULL;
static size_t g_elen = 0, g_ecap = 0;

static GCState *gc_state_of(struct VL_Context *ctx) {
  for (size_t i = 0; i < g_elen; i++)
    if (g_entries[i].ctx == ctx) return &g_entries[i].st;
  return NULL;
}
static GCState *gc_state_ensure(struct VL_Context *ctx) {
  GCState *st = gc_state_of(ctx);
  if (st) return st;
  if (g_elen == g_ecap) {
    size_t nc = g_ecap ? g_ecap * 2 : 8;
    GCEntry *nv = (GCEntry *)realloc(g_entries, nc * sizeof(GCEntry));
    if (!nv) return NULL;
    g_entries = nv;
    g_ecap = nc;
  }
  g_entries[g_elen].ctx = ctx;
  memset(&g_entries[g_elen].st, 0, sizeof(GCState));
  g_entries[g_elen].st.trigger_bytes = 1 << 20;  // 1 MiB par défaut
  return &g_entries[g_elen++].st;
}

void vl_gc_attach(struct VL_Context *ctx, size_t trigger_bytes) {
  GCState *st = gc_state_ensure(ctx);
  if (!st) return;
  if (trigger_bytes) st->trigger_bytes = trigger_bytes;
}

void vl_gc_detach(struct VL_Context *ctx) {
  for (size_t i = 0; i < g_elen; i++)
    if (g_entries[i].ctx == ctx) {
      free(g_entries[i].st.nodes);
      memmove(&g_entries[i], &g_entries[g_elen - 1], sizeof(GCEntry));
      g_elen--;
      break;
    }
}

void vl_gc_set_ownership(struct VL_Context *ctx, bool own_strings) {
  GCState *st = gc_state_ensure(ctx);
  if (!st) return;
  st->own_strings = own_strings ? 1 : 0;
}

static size_t approx_vlstring_size(VL_String *s) {
  return s ? (sizeof(*s) + (size_t)s->len + 1) : 0;
}

static int gc_register_node(GCState *st, VL_String *s, int owned) {
  if (!s) return 0;  // ignore
  // dédup simple: ne pas dupliquer l'entrée si déjà vue
  for (size_t i = 0; i < st->len; i++) {
    if (st->nodes[i].ptr == s) {
      st->nodes[i].owned |= owned ? 1 : 0;
      return 1;
    }
  }
  if (st->len == st->cap) {
    size_t nc = st->cap ? st->cap * 2 : 64;
    GCNode *nv = (GCNode *)realloc(st->nodes, nc * sizeof(GCNode));
    if (!nv) return 0;
    st->nodes = nv;
    st->cap = nc;
  }
  st->nodes[st->len] = (GCNode){.ptr = s,
                                .marked = 0,
                                .owned = (uint32_t)owned,
                                .size = (uint32_t)approx_vlstring_size(s)};
  st->bytes += st->nodes[st->len].size;
  st->len++;
  return 1;
}

void vl_gc_register_string(struct VL_Context *ctx, struct VL_String *s) {
  GCState *st = gc_state_ensure(ctx);
  if (!st) return;
  gc_register_node(st, s, st->own_strings);
}

// Indexation opportuniste des chaînes déjà présentes dans les structures du
// contexte
void vl_gc_preindex_existing(struct VL_Context *ctx) {
  GCState *st = gc_state_ensure(ctx);
  if (!st) return;
  // pool constantes
  for (size_t i = 0; i < ctx->kstr_len; i++)
    if (ctx->kstr[i]) gc_register_node(st, ctx->kstr[i], 0);
  // globaux: clés et valeurs string
  for (size_t i = 0; i < ctx->globals.cap; i++) {
    VL_String *k = ctx->globals.keys ? ctx->globals.keys[i] : NULL;
    if (k && k != (VL_String *)(uintptr_t)1) gc_register_node(st, k, 0);
    if (ctx->globals.vals) {
      VL_Value v = ctx->globals.vals[i];
      if (v.type == VT_STR && v.as.s) gc_register_node(st, v.as.s, 0);
    }
  }
  // pile
  for (size_t i = 0; i < ctx->sp; i++) {
    VL_Value v = ctx->stack[i];
    if (v.type == VT_STR && v.as.s) gc_register_node(st, v.as.s, 0);
  }
  // natives: clés = noms
  for (size_t i = 0; i < ctx->natives.cap; i++) {
    VL_String *k = ctx->natives.keys ? ctx->natives.keys[i] : NULL;
    if (k && k != (VL_String *)(uintptr_t)1) gc_register_node(st, k, 0);
  }
}

// ───────────────────────── Marquage ─────────────────────────
static void mark_str(GCState *st, VL_String *s) {
  if (!s) return;
  for (size_t i = 0; i < st->len; i++) {
    if (st->nodes[i].ptr == s) {
      st->nodes[i].marked = 1;
      return;
    }
  }
}
static void mark_val(GCState *st, const VL_Value *v) {
  if (!v) return;
  if (v->type == VT_STR) mark_str(st, v->as.s);
}

static void mark_roots(struct VL_Context *ctx, GCState *st) {
  // pile
  for (size_t i = 0; i < ctx->sp; i++) mark_val(st, &ctx->stack[i]);
  // globaux
  for (size_t i = 0; i < ctx->globals.cap; i++) {
    VL_String *k = ctx->globals.keys ? ctx->globals.keys[i] : NULL;
    if (k && k != (VL_String *)(uintptr_t)1) mark_str(st, k);
    if (ctx->globals.vals) mark_val(st, &ctx->globals.vals[i]);
  }
  // pool constantes
  for (size_t i = 0; i < ctx->kstr_len; i++) mark_str(st, ctx->kstr[i]);
  // natives
  for (size_t i = 0; i < ctx->natives.cap; i++) {
    VL_String *k = ctx->natives.keys ? ctx->natives.keys[i] : NULL;
    if (k && k != (VL_String *)(uintptr_t)1) mark_str(st, k);
  }
}

// ───────────────────────── Balayage ─────────────────────────
static void sweep(struct VL_Context *ctx, GCState *st, int verbose) {
  (void)ctx;  // ctx unused for now
  size_t w = 0;
  for (size_t i = 0; i < st->len; i++) {
    GCNode *n = &st->nodes[i];
    if (n->ptr == NULL) continue;
    if (n->marked) {
      n->marked = 0;
      st->nodes[w++] = *n;
      continue;
    }
    // non marqué
    if (n->owned) {
      free(n->ptr);
      st->freed_bytes += n->size;
      st->freed_count++;
    }
    // sinon, on oublie juste la référence
    // ne pas copier dans la zone conservée
  }
  st->len = w;  // compacte implicitement
}

// ───────────────────────── Collect ─────────────────────────
VL_Status vl_gc_collect(struct VL_Context *ctx, int flags) {
  GCState *st = gc_state_of(ctx);
  if (!st) return VL_ERR_BAD_ARG;
  int verbose = (flags & 1) ? 1 : 0;
  size_t before = st->len;
  size_t bbytes = st->bytes;
  if (verbose)
    fprintf(stderr, "[gc] start: nodes=%zu bytes=%zu\n", before, bbytes);
  mark_roots(ctx, st);
  sweep(ctx, st, verbose);
  if (verbose)
    fprintf(stderr, "[gc] end: nodes=%zu freed=%zu objects, %zu bytes\n",
            st->len, st->freed_count, st->freed_bytes);
  return VL_OK;
}

void vl_gc_stats(struct VL_Context *ctx, size_t *tracked, size_t *bytes,
                 size_t *frees) {
  GCState *st = gc_state_of(ctx);
  if (!st) {
    if (tracked) *tracked = 0;
    if (bytes) *bytes = 0;
    if (frees) *frees = 0;
    return;
  }
  if (tracked) *tracked = st->len;
  if (bytes) *bytes = st->bytes;
  if (frees) *frees = st->freed_count;
}

// ───────────────────────── Déclenchement heuristique (optionnel)
// ─────────────────────────
static void maybe_trigger(struct VL_Context *ctx) {
  GCState *st = gc_state_of(ctx);
  if (!st) return;
  if (st->bytes > st->trigger_bytes) {
    vl_gc_collect(ctx, 0);  // silencieux
    // relever le seuil progressivement
    st->trigger_bytes = st->bytes * 2;
    if (st->trigger_bytes < (1 << 20)) st->trigger_bytes = (1 << 20);
  }
}

// Helper facultatif à appeler depuis la VM après allocation d’une chaîne
void vl_gc_on_string_alloc(struct VL_Context *ctx, struct VL_String *s) {
  if (!s) return;
  vl_gc_register_string(ctx, s);
  maybe_trigger(ctx);
}

// ───────────────────────── Test autonome ─────────────────────────
#ifdef VL_GC_TEST_MAIN
#include "debug.h"
static void emit_u8(uint8_t **p, uint8_t v) { *(*p)++ = v; }
static void emit_u32(uint8_t **p, uint32_t v) {
  memcpy(*p, &v, 4);
  *p += 4;
}
static void emit_u64(uint8_t **p, uint64_t v) {
  memcpy(*p, &v, 8);
  *p += 8;
}
int main(void) {
  uint8_t buf[512];
  uint8_t *p = buf;
  emit_u8(&p, 'V');
  emit_u8(&p, 'L');
  emit_u8(&p, 'B');
  emit_u8(&p, 'C');
  emit_u8(&p, 1);
  const char *s0 = "hello";
  const char *s1 = "print";
  emit_u32(&p, 2);
  emit_u32(&p, (uint32_t)strlen(s0));
  memcpy(p, s0, strlen(s0));
  p += strlen(s0);
  emit_u32(&p, (uint32_t)strlen(s1));
  memcpy(p, s1, strlen(s1));
  p += strlen(s1);
  uint8_t *csz = p;
  emit_u32(&p, 0);
  uint8_t *cs = p;
  emit_u8(&p, 3 /*PUSHS*/);
  emit_u32(&p, 0);
  emit_u8(&p, 18 /*CALLN*/);
  emit_u32(&p, 1);
  emit_u8(&p, 1);
  emit_u8(&p, 19 /*HALT*/);
  uint32_t code_sz = (uint32_t)(p - cs);
  memcpy(csz, &code_sz, 4);
  VL_Context *vm = vl_create_default();
  vl_gc_attach(vm, 0);
  vl_gc_set_ownership(vm, true);
  if (vl_load_program_from_memory(vm, buf, (size_t)(p - buf)) != VL_OK) {
    fprintf(stderr, "load fail: %s\n", vl_last_error(vm)->msg);
    return 2;
  }
  // préindexation pour les constantes
  vl_gc_preindex_existing(vm);
  // exécuter
  vl_run(vm, 0);
  // forcer GC
  vl_gc_collect(vm, 1);
  size_t n = 0, b = 0, f = 0;
  vl_gc_stats(vm, &n, &b, &f);
  fprintf(stderr, "tracked=%zu bytes=%zu freed=%zu\n", n, b, f);
  vl_destroy(vm);
  return 0;
}
#endif
