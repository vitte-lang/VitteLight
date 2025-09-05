/* ============================================================================
   parser.c — Analyseur syntaxique Vitte/Vitl (EBNF canonique 2025).
   - Pratt parser pour les expressions (assign → mul, unaire, postfix)
   - Descente récursive pour items, blocs, statements, types et patterns
   - Support des modules, use/import, structs, fonctions, impl, tests
   - Gestion d’erreurs robuste: synchronisation par FIRST/FOLLOW, diagnostics
   - Mémoire: arène interne ou branchement sur vt_arena (mem.h) si présent
   - C17, UTF-8, licence MIT.

   Dépendances attendues (faibles):
     - lex.h : lexer/tokens (VT_LEX_H_SENTINEL défini)
     - mem.h : vt_arena_* (optionnel; sinon arène locale)
     - debug.h : VT_ERROR/V T_WARN (optionnel)

   Entrées exposées:
     - vt_parse_source(), vt_parse_file()
     - vt_ast_dump() pour debug
     - vt_ast_free() si arène locale (no-op si vt_arena externe)

   Remarque: Un header parser.h pourra déclarer l’AST public si besoin.
   Ici, l’AST minimal est défini dans ce .c pour permettre un usage direct.
   ============================================================================
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lex.h" /* attend VT_LEX_H_SENTINEL, vt_lexer, vt_token, vt_tok_kind */
#ifdef VT_LEX_H_SENTINEL
/* ok */
#else
#warning "lex.h manquant ou ancien: ce parser suppose VT_LEX_H_SENTINEL"
#endif

/* ---------------------------------------------------------------------------
   Arène mémoire (fallback si mem.h absent)
--------------------------------------------------------------------------- */
#ifdef VT_MEM_H_SENTINEL
#include "mem.h"
typedef vt_arena parser_arena_t;
#define PAR_ARENA_INIT(a) vt_arena_init((a), 64 * 1024)
#define PAR_ARENA_NEW(a, T, n) \
  (T*)vt_arena_alloc((a), sizeof(T) * (n), _Alignof(T))
#define PAR_ARENA_FREE(a) vt_arena_free((a))
#else
typedef struct parser_block {
  struct parser_block* next;
  size_t cap, used;
  unsigned char data[];
} parser_block;
typedef struct parser_arena {
  parser_block* head;
  size_t slab;
} parser_arena_t;

static void PAR_ARENA_INIT(parser_arena_t* A) {
  A->head = NULL;
  A->slab = 64 * 1024;
}
static void* par__alloc_block(size_t cap) {
  parser_block* b = (parser_block*)malloc(sizeof(parser_block) + cap);
  if (!b) return NULL;
  b->next = NULL;
  b->cap = cap;
  b->used = 0;
  return b;
}
static void* PAR_ARENA_ALLOC(parser_arena_t* A, size_t sz, size_t align) {
  if (align < sizeof(void*)) align = sizeof(void*);
  parser_block* b = A->head;
  if (!b || (b->used + align - 1) / align * align + sz > b->cap) {
    size_t need = sz + align + 64;
    size_t cap = A->slab;
    while (cap < need) cap <<= 1;
    parser_block* nb = (parser_block*)par__alloc_block(cap);
    if (!nb) return NULL;
    nb->next = A->head;
    A->head = nb;
    b = nb;
  }
  size_t p = (b->used + (align - 1)) & ~(align - 1);
  if (p + sz > b->cap) return NULL;
  void* ptr = b->data + p;
  b->used = p + sz;
  return ptr;
}
#define PAR_ARENA_NEW(A, T, n) \
  (T*)PAR_ARENA_ALLOC((A), sizeof(T) * (n), _Alignof(T))
static void PAR_ARENA_FREE(parser_arena_t* A) {
  parser_block* b = A->head;
  while (b) {
    parser_block* nx = b->next;
    free(b);
    b = nx;
  }
  A->head = NULL;
}
#endif

/* ---------------------------------------------------------------------------
   Diagnostics
--------------------------------------------------------------------------- */
typedef struct vt_diag {
  int line, col;
  const char* file;
  char* msg;
} vt_diag;

typedef struct vt_diags {
  vt_diag* v;
  size_t len, cap;
  parser_arena_t* arena;
} vt_diags;

static void diags_init(vt_diags* D, parser_arena_t* arena) {
  D->v = NULL;
  D->len = 0;
  D->cap = 0;
  D->arena = arena;
}
static void diags_pushf(vt_diags* D, const vt_token* t, const char* fmt, ...) {
  if (D->len == D->cap) {
    size_t ncap = D->cap ? D->cap * 2 : 16;
    vt_diag* nv = PAR_ARENA_NEW(D->arena, vt_diag, ncap);
    if (!nv) return;
    if (D->v) memcpy(nv, D->v, sizeof(*nv) * D->len);
    D->v = nv;
    D->cap = ncap;
  }
  vt_diag* d = &D->v[D->len++];
  d->line = t ? t->line : 0;
  d->col = t ? t->col : 0;
  d->file = t ? t->file : NULL;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  size_t mlen = strlen(buf) + 1;
  d->msg = PAR_ARENA_NEW(D->arena, char, mlen);
  if (d->msg) memcpy(d->msg, buf, mlen);
}

/* ---------------------------------------------------------------------------
   AST minimal (public interne pour ce fichier)
--------------------------------------------------------------------------- */
typedef enum vt_ast_kind {
  AST_MODULE,
  AST_USE,
  AST_IMPORT,
  AST_MOD,
  AST_CONST,
  AST_TYPEALIAS,
  AST_STRUCT,
  AST_FIELD,
  AST_FN,
  AST_PARAM,
  AST_IMPL,
  AST_TEST,
  AST_BLOCK,
  AST_LET,
  AST_STMT_EXPR,
  AST_RETURN,
  AST_BREAK,
  AST_CONTINUE,
  AST_IF,
  AST_WHILE,
  AST_FOR,
  AST_MATCH,
  AST_MATCH_ARM,
  AST_EXPR,
  AST_TYPE,
} vt_ast_kind;

typedef struct vt_ast_node vt_ast_node;

typedef struct vec_node {
  vt_ast_node** data;
  size_t len, cap;
} vec_node;

typedef struct vec_tok {
  vt_token* data;
  size_t len, cap;
} vec_tok;

typedef struct vt_ast_node {
  vt_ast_kind kind;
  vt_token at; /* position de référence */
  union {
    /* MODULE */
    struct {
      vt_token name_path;
      vec_node items;
    } module;

    /* USE/IMPORT/MOD */
    struct {
      vt_token path;
      vt_token alias;
    } use_decl;
    struct {
      vt_token path;
    } import_decl;
    struct {
      vt_token name;
      vec_node items;
      bool inline_body;
    } mod_decl;

    /* CONST */
    struct {
      bool is_pub;
      vt_token name;
      vt_ast_node* type;
      vt_ast_node* value;
    } const_decl;

    /* TYPE ALIAS */
    struct {
      bool is_pub;
      vt_token name;
      vt_ast_node* aliased;
    } type_alias;

    /* STRUCT */
    struct {
      bool is_pub;
      vt_token name;
      vec_node fields;
    } struct_decl;
    struct {
      vt_token name;
      vt_ast_node* type;
    } field;

    /* FUNCTION */
    struct {
      bool is_pub;
      vt_token name;
      vec_node params;
      vt_ast_node* ret;
      vt_ast_node* where;
      vt_ast_node* body;
    } fn_decl;
    struct {
      bool is_mut;
      vt_token name;
      vt_ast_node* type;
    } param;

    /* IMPL */
    struct {
      vt_ast_node* ty;
      vec_node items;
    } impl_block;

    /* TEST */
    struct {
      vt_token name;
      vt_ast_node* body;
    } test_block;

    /* BLOCK / STMTS */
    struct {
      vec_node stmts;
    } block;
    struct {
      bool is_mut;
      vt_token name;
      vt_ast_node* ty;
      vt_ast_node* init;
    } let_stmt;
    struct {
      vt_ast_node* expr;
    } expr_stmt;
    struct {
      vt_ast_node* expr;
    } ret_stmt;

    /* IF/WHILE/FOR/MATCH */
    struct {
      vt_ast_node* cond;
      vt_ast_node* thenb;
      vt_ast_node* elseb;
    } if_expr;
    struct {
      vt_ast_node* cond;
      vt_ast_node* body;
    } while_expr;
    struct {
      vt_token iter;
      vt_ast_node* range;
      vt_ast_node* body;
    } for_expr;
    struct {
      vt_ast_node* scrut;
      vec_node arms;
    } match_expr;
    struct {
      vt_ast_node* pat;
      vt_ast_node* expr;
    } match_arm;

    /* TYPE node (path/generic/slice/ref/array/tuple) encodé comme EXPR-like */
    struct {
      vec_node children;
      vt_token tag;
    } type;

    /* EXPR node (Pratt) */
    struct {
      vec_node children;
      vt_token op;
    } expr;
  } as;
} vt_ast_node;

/* Vectors helpers */
static void vec_node_push(parser_arena_t* A, vec_node* v, vt_ast_node* n) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    vt_ast_node** nd = PAR_ARENA_NEW(A, vt_ast_node*, nc);
    if (!nd) return;
    if (v->data) memcpy(nd, v->data, sizeof(*nd) * v->len);
    v->data = nd;
    v->cap = nc;
  }
  v->data[v->len++] = n;
}

/* Node builder */
static vt_ast_node* N(parser_arena_t* A, vt_ast_kind k, const vt_token* at) {
  vt_ast_node* n = PAR_ARENA_NEW(A, vt_ast_node, 1);
  memset(n, 0, sizeof(*n));
  n->kind = k;
  if (at) n->at = *at;
  return n;
}

/* ---------------------------------------------------------------------------
   Parser context
--------------------------------------------------------------------------- */
typedef struct parser {
  parser_arena_t arena;
  bool arena_inited;
  vt_lexer lx;
  bool own_lexer;
  vt_token cur, nxt;
  bool has_nxt;
  vt_diags diags;
} parser;

/* lexer bridge */
static void p_init_lexer_from_source(parser* P, const char* src,
                                     const char* file) {
  vt_lex_init(&P->lx, src, file);
  P->own_lexer = true;
}
static void p_init_lexer_from_file(parser* P, const char* path) {
  vt_lex_init_from_file(&P->lx, path);
  P->own_lexer = true;
}
static void p_destroy_lexer(parser* P) {
  if (P->own_lexer) vt_lex_destroy(&P->lx);
}

/* token stream */
static const vt_token* p_peek(parser* P) {
  if (!P->has_nxt) {
    vt_lex_next(&P->lx, &P->nxt);
    P->has_nxt = true;
  }
  return &P->nxt;
}
static const vt_token* p_cur(parser* P) {
  return &P->cur.kind ? &P->cur : p_peek(P);
}
static void p_advance(parser* P) {
  if (!P->has_nxt) vt_lex_next(&P->lx, &P->nxt), P->has_nxt = true;
  P->cur = P->nxt;
  P->has_nxt = false;
}
static bool p_accept(parser* P, vt_tok_kind k) {
  const vt_token* t = p_peek(P);
  if (t->kind == k) {
    p_advance(P);
    return true;
  }
  return false;
}
static bool p_expect(parser* P, vt_tok_kind k, const char* what) {
  const vt_token* t = p_peek(P);
  if (t->kind != k) {
    diags_pushf(&P->diags, t, "attendu %s", what);
    return false;
  }
  p_advance(P);
  return true;
}

/* synchronisation sur tokens sentinelles */
static void p_sync_to(parser* P, const vt_tok_kind* set, size_t n) {
  for (;;) {
    const vt_token* t = p_peek(P);
    if (t->kind == TK_EOF) return;
    for (size_t i = 0; i < n; i++)
      if (t->kind == set[i]) return;
    p_advance(P);
  }
}

/* ---------------------------------------------------------------------------
   Prototypes parsing
--------------------------------------------------------------------------- */
static vt_ast_node* parse_program(parser* P);
static vt_ast_node* parse_item(parser* P);
static vt_ast_node* parse_block(parser* P);
static vt_ast_node* parse_stmt(parser* P);
static vt_ast_node* parse_type(parser* P);
static vt_ast_node* parse_expr(parser* P);

/* utils */
static bool is_start_of_type(vt_tok_kind k) {
  switch (k) {
    case TK_IDENT:
    case TK_AMP:
    case TK_LBRACK:
    case TK_LPAREN: /* path/ref/slice/array/tuple */
      return true;
    default:
      return false;
  }
}
static bool is_start_of_expr(vt_tok_kind k) {
  switch (k) {
    case TK_IDENT:
    case TK_INT:
    case TK_FLOAT:
    case TK_STR:
    case TK_CHAR:
    case TK_TRUE:
    case TK_FALSE:
    case TK_LPAREN:
    case TK_LBRACK:
    case TK_BANG:
    case TK_MINUS:
    case TK_AMP:
      return true;
    default:
      return false;
  }
}

/* ---------------------------------------------------------------------------
   Items
--------------------------------------------------------------------------- */
static vt_ast_node* parse_path_as_token(parser* P, const char* what) {
  /* simplifié: consomme IDENT { :: IDENT } et retourne dernier token comme
   * repère */
  if (!p_expect(P, TK_IDENT, "identifiant")) return NULL;
  vt_token base = P->cur;
  while (p_accept(P, TK_DCOLON)) {
    if (!p_expect(P, TK_IDENT, "identifiant après '::'")) break;
    base = P->cur;
  }
  vt_ast_node* t = N(&P->arena, AST_EXPR, &base);
  t->as.expr.op = base;
  return t;
}

static vt_ast_node* parse_use(parser* P) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_USE, "'use'");
  /* use path [as alias] ;  |  use path::{...} ;  */
  /* Variante minimale: path [as IDENT] ; */
  vt_ast_node* n = N(&P->arena, AST_USE, &at);
  n->as.use_decl.path = *p_peek(P);
  parse_path_as_token(P, "chemin");
  if (p_accept(P, TK_AS)) {
    if (!p_expect(P, TK_IDENT, "alias")) { /* noop */
    } else
      n->as.use_decl.alias = P->cur;
  }
  p_expect(P, TK_SEMI, "';'");
  return n;
}

static vt_ast_node* parse_import(parser* P) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_IMPORT, "'import'");
  vt_ast_node* n = N(&P->arena, AST_IMPORT, &at);
  n->as.import_decl.path = *p_peek(P);
  parse_path_as_token(P, "chemin");
  p_expect(P, TK_SEMI, "';'");
  return n;
}

static vt_ast_node* parse_field(parser* P) {
  if (!p_expect(P, TK_IDENT, "nom de champ")) return NULL;
  vt_token name = P->cur;
  p_expect(P, TK_COLON, "':'");
  vt_ast_node* ty = parse_type(P);
  vt_ast_node* f = N(&P->arena, AST_FIELD, &name);
  f->as.field.name = name;
  f->as.field.type = ty;
  return f;
}

static vt_ast_node* parse_struct(parser* P, bool is_pub) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_STRUCT, "'struct'");
  if (!p_expect(P, TK_IDENT, "nom de struct")) return NULL;
  vt_token name = P->cur;
  p_expect(P, TK_LBRACE, "'{'");
  vec_node fields = {0};
  while (!p_accept(P, TK_RBRACE)) {
    vt_ast_node* f = parse_field(P);
    if (f) vec_node_push(&P->arena, &fields, f);
    if (!p_accept(P, TK_COMMA)) {
      p_expect(P, TK_RBRACE, "'}'");
      break;
    }
  }
  vt_ast_node* st = N(&P->arena, AST_STRUCT, &at);
  st->as.struct_decl.is_pub = is_pub;
  st->as.struct_decl.name = name;
  st->as.struct_decl.fields = fields;
  return st;
}

static vt_ast_node* parse_type_alias(parser* P, bool is_pub) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_TYPE, "'type'");
  if (!p_expect(P, TK_IDENT, "nom d'alias")) return NULL;
  vt_token name = P->cur;
  p_expect(P, TK_ASSIGN, "'='");
  vt_ast_node* aliased = parse_type(P);
  p_expect(P, TK_SEMI, "';'");
  vt_ast_node* n = N(&P->arena, AST_TYPEALIAS, &at);
  n->as.type_alias.is_pub = is_pub;
  n->as.type_alias.name = name;
  n->as.type_alias.aliased = aliased;
  return n;
}

static vt_ast_node* parse_const(parser* P, bool is_pub) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_CONST, "'const'");
  if (!p_expect(P, TK_IDENT, "nom de constante")) return NULL;
  vt_token name = P->cur;
  p_expect(P, TK_COLON, "':'");
  vt_ast_node* ty = parse_type(P);
  p_expect(P, TK_ASSIGN, "'='");
  vt_ast_node* value = parse_expr(P);
  p_expect(P, TK_SEMI, "';'");
  vt_ast_node* n = N(&P->arena, AST_CONST, &at);
  n->as.const_decl.is_pub = is_pub;
  n->as.const_decl.name = name;
  n->as.const_decl.type = ty;
  n->as.const_decl.value = value;
  return n;
}

static vt_ast_node* parse_param(parser* P) {
  bool is_mut = p_accept(P, TK_MUT);
  if (!p_expect(P, TK_IDENT, "nom de paramètre")) return NULL;
  vt_token name = P->cur;
  p_expect(P, TK_COLON, "':'");
  vt_ast_node* ty = parse_type(P);
  vt_ast_node* p = N(&P->arena, AST_PARAM, &name);
  p->as.param.is_mut = is_mut;
  p->as.param.name = name;
  p->as.param.type = ty;
  return p;
}

static vt_ast_node* parse_params(parser* P) {
  vec_node ps = {0};
  if (!p_accept(P, TK_RPAREN)) {
    for (;;) {
      vt_ast_node* pr = parse_param(P);
      if (pr) vec_node_push(&P->arena, &ps, pr);
      if (p_accept(P, TK_COMMA)) {
        if (p_accept(P, TK_RPAREN)) break;
        continue;
      }
      p_expect(P, TK_RPAREN, "')'");
      break;
    }
  }
  vt_ast_node* node = N(&P->arena, AST_EXPR, p_peek(P));
  node->as.expr.children = ps;
  return node;
}

static vt_ast_node* parse_fn(parser* P, bool is_pub) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_FN, "'fn'");
  if (!p_expect(P, TK_IDENT, "nom de fonction")) return NULL;
  vt_token name = P->cur;
  p_expect(P, TK_LPAREN, "'('");
  vt_ast_node* params = parse_params(P);
  vt_ast_node* ret = NULL;
  if (p_accept(P, TK_ARROW)) ret = parse_type(P);
  vt_ast_node* body = parse_block(P);
  vt_ast_node* n = N(&P->arena, AST_FN, &at);
  n->as.fn_decl.is_pub = is_pub;
  n->as.fn_decl.name = name;
  n->as.fn_decl.params = params->as.expr.children;
  n->as.fn_decl.ret = ret;
  n->as.fn_decl.body = body;
  return n;
}

static vt_ast_node* parse_impl(parser* P) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_IMPL, "'impl'");
  vt_ast_node* ty = parse_type(P);
  p_expect(P, TK_LBRACE, "'{'");
  vec_node items = {0};
  while (!p_accept(P, TK_RBRACE)) {
    vt_ast_node* it = parse_item(P);
    if (it)
      vec_node_push(&P->arena, &items, it);
    else {
      /* sync to '}' */
      const vt_tok_kind follow[] = {TK_RBRACE, TK_EOF};
      diags_pushf(&P->diags, p_peek(P), "synchronisation après erreur d'impl");
      p_sync_to(P, follow, 2);
      p_accept(P, TK_RBRACE);
      break;
    }
  }
  vt_ast_node* n = N(&P->arena, AST_IMPL, &at);
  n->as.impl_block.ty = ty;
  n->as.impl_block.items = items;
  return n;
}

static vt_ast_node* parse_test(parser* P) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_TEST, "'test'");
  if (!p_expect(P, TK_STR, "libellé de test")) {
    /* continue avec un nom fake */
  }
  vt_token label = P->cur;
  vt_ast_node* body = parse_block(P);
  vt_ast_node* n = N(&P->arena, AST_TEST, &at);
  n->as.test_block.name = label;
  n->as.test_block.body = body;
  return n;
}

static vt_ast_node* parse_mod(parser* P) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_MOD, "'mod'");
  if (!p_expect(P, TK_IDENT, "nom de module")) return NULL;
  vt_token name = P->cur;
  vt_ast_node* n = N(&P->arena, AST_MOD, &at);
  n->as.mod_decl.name = name;
  if (p_accept(P, TK_SEMI)) {
    n->as.mod_decl.inline_body = false;
    return n;
  }
  n->as.mod_decl.inline_body = true;
  p_expect(P, TK_LBRACE, "'{'");
  vec_node items = {0};
  while (!p_accept(P, TK_RBRACE)) {
    vt_ast_node* it = parse_item(P);
    if (it)
      vec_node_push(&P->arena, &items, it);
    else {
      const vt_tok_kind follow[] = {TK_RBRACE, TK_EOF};
      p_sync_to(P, follow, 2);
      p_accept(P, TK_RBRACE);
      break;
    }
  }
  n->as.mod_decl.items = items;
  return n;
}

static vt_ast_node* parse_item(parser* P) {
  bool is_pub = p_accept(P, TK_PUB);
  const vt_token* t = p_peek(P);
  switch (t->kind) {
    case TK_USE:
      return parse_use(P);
    case TK_IMPORT:
      return parse_import(P);
    case TK_MOD:
      return parse_mod(P);
    case TK_CONST:
      return parse_const(P, is_pub);
    case TK_TYPE:
      return parse_type_alias(P, is_pub);
    case TK_STRUCT:
      return parse_struct(P, is_pub);
    case TK_FN:
      return parse_fn(P, is_pub);
    case TK_IMPL:
      return parse_impl(P);
    case TK_TEST:
      return parse_test(P);
    default:
      diags_pushf(&P->diags, t,
                  "déclaration attendue "
                  "(use/import/mod/const/type/struct/fn/impl/test)");
      /* sync jusqu’au prochain item plausible ou fin de bloc */
      {
        const vt_tok_kind follow[] = {TK_USE,  TK_IMPORT, TK_MOD, TK_CONST,
                                      TK_TYPE, TK_STRUCT, TK_FN,  TK_IMPL,
                                      TK_TEST, TK_RBRACE, TK_EOF};
        p_sync_to(P, follow, sizeof(follow) / sizeof(follow[0]));
      }
      return NULL;
  }
}

/* ---------------------------------------------------------------------------
   Types (path, ref, slice, array, tuple, generic<T,...>)
--------------------------------------------------------------------------- */
static vt_ast_node* parse_type_path_or_generic(parser* P) {
  /* path (IDENT (:: IDENT)*) [ '<' type {',' type} '>' ] */
  if (!p_expect(P, TK_IDENT, "type/path")) return NULL;
  vt_token last = P->cur;
  while (p_accept(P, TK_DCOLON)) {
    p_expect(P, TK_IDENT, "identifiant de chemin");
    last = P->cur;
  }
  vt_ast_node* node = N(&P->arena, AST_TYPE, &last);
  node->as.type.tag = last;
  if (p_accept(P, TK_LT)) {
    vec_node args = {0};
    for (;;) {
      vt_ast_node* a = parse_type(P);
      if (a) vec_node_push(&P->arena, &args, a);
      if (p_accept(P, TK_COMMA)) continue;
      p_expect(P, TK_GT, "'>'");
      break;
    }
    node->as.type.children = args;
  }
  return node;
}
static vt_ast_node* parse_type_tuple(parser* P) {
  /* '(' T ',' T {',' T} ')' */
  vt_token at = *p_peek(P);
  p_expect(P, TK_LPAREN, "'('");
  vt_ast_node* a = parse_type(P);
  p_expect(P, TK_COMMA, "','");
  vt_ast_node* b = parse_type(P);
  vec_node xs = {0};
  vec_node_push(&P->arena, &xs, a);
  vec_node_push(&P->arena, &xs, b);
  while (p_accept(P, TK_COMMA)) {
    if (p_accept(P, TK_RPAREN)) break;
    vt_ast_node* t = parse_type(P);
    if (t) vec_node_push(&P->arena, &xs, t);
  }
  if (!P->cur.kind || P->cur.kind != TK_RPAREN) p_expect(P, TK_RPAREN, "')'");
  vt_ast_node* n = N(&P->arena, AST_TYPE, &at);
  n->as.type.tag = at;
  n->as.type.children = xs;
  return n;
}
static vt_ast_node* parse_type(parser* P) {
  const vt_token* t = p_peek(P);
  switch (t->kind) {
    case TK_AMP: { /* & [mut] type  OR  &[ type ] slice ? => grammar sépare
                      slice: "&" "[" T "]" */
      vt_token at = *t;
      p_advance(P);
      if (p_accept(P, TK_LBRACK)) {
        vt_ast_node* inner = parse_type(P);
        p_expect(P, TK_RBRACK, "']'");
        vt_ast_node* n = N(&P->arena, AST_TYPE, &at);
        n->as.type.tag = at; /* tag='&[' slice */
        vec_node ch = {0};
        vec_node_push(&P->arena, &ch, inner);
        n->as.type.children = ch;
        return n;
      }
      /* & mut? type */
      bool is_mut = p_accept(P, TK_MUT);
      vt_ast_node* base = parse_type(P);
      vt_ast_node* n = N(&P->arena, AST_TYPE, &at);
      n->as.type.tag = at;
      vec_node ch = {0};
      vec_node_push(&P->arena, &ch, base);
      n->as.type.children = ch;
      (void)is_mut; /* sémantique ultérieure */
      return n;
    }
    case TK_LBRACK: { /* [ T ; expr ] */
      vt_token at = *t;
      p_advance(P);
      vt_ast_node* elem = parse_type(P);
      p_expect(P, TK_SEMI, "';'");
      vt_ast_node* len = parse_expr(P);
      p_expect(P, TK_RBRACK, "']'");
      vt_ast_node* n = N(&P->arena, AST_TYPE, &at);
      n->as.type.tag = at;
      vec_node ch = {0};
      vec_node_push(&P->arena, &ch, elem);
      vec_node_push(&P->arena, &ch, len);
      n->as.type.children = ch;
      return n;
    }
    case TK_LPAREN:
      return parse_type_tuple(P);
    case TK_IDENT:
      return parse_type_path_or_generic(P);
    default:
      diags_pushf(&P->diags, t, "type attendu");
      /* fabriquer un nœud trou pour continuer */
      return N(&P->arena, AST_TYPE, t);
  }
}

/* ---------------------------------------------------------------------------
   Expressions — Pratt
--------------------------------------------------------------------------- */

typedef enum {
  PREC_LOWEST = 0,
  PREC_ASSIGN,
  PREC_OR,
  PREC_AND,
  PREC_BOR,
  PREC_BXOR,
  PREC_BAND,
  PREC_EQ,
  PREC_REL,
  PREC_SH,
  PREC_ADD,
  PREC_MUL,
  PREC_UNARY,
  PREC_POSTFIX,
  PREC_PRIMARY
} prec_t;

static prec_t infix_prec(vt_tok_kind k, bool* right_assoc) {
  *right_assoc = false;
  switch (k) {
    case TK_ASSIGN:
    case TK_PLUSEQ:
    case TK_MINUSEQ:
    case TK_STAREQ:
    case TK_SLASHEQ:
    case TK_PERCENTEQ:
    case TK_SHLEQ:
    case TK_SHREQ:
    case TK_ANDEQ:
    case TK_XOREQ:
    case TK_OREQ:
      *right_assoc = true;
      return PREC_ASSIGN;
    case TK_OROR:
      return PREC_OR;
    case TK_ANDAND:
      return PREC_AND;
    case TK_PIPE:
      return PREC_BOR;
    case TK_CARET:
      return PREC_BXOR;
    case TK_AMP:
      return PREC_BAND;
    case TK_EQEQ:
    case TK_NEQ:
      return PREC_EQ;
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
      return PREC_REL;
    case TK_SHL:
    case TK_SHR:
      return PREC_SH;
    case TK_PLUS:
    case TK_MINUS:
      return PREC_ADD;
    case TK_STAR:
    case TK_SLASH:
    case TK_PERCENT:
      return PREC_MUL;
    case TK_DOT:
    case TK_LPAREN:
    case TK_LBRACK:
    case TK_RANGE:
    case TK_RANGEEQ:
      return PREC_POSTFIX;
    default:
      return PREC_LOWEST;
  }
}

static vt_ast_node* parse_primary(parser* P) {
  const vt_token* t = p_peek(P);
  switch (t->kind) {
    case TK_IDENT: {
      p_advance(P);
      /* path: IDENT (:: IDENT)*  (sans generics ici) */
      vt_token last = P->cur;
      while (p_accept(P, TK_DCOLON)) {
        p_expect(P, TK_IDENT, "identifiant");
        last = P->cur;
      }
      vt_ast_node* n = N(&P->arena, AST_EXPR, &last);
      n->as.expr.op = last;
      return n;
    }
    case TK_INT:
    case TK_FLOAT:
    case TK_STR:
    case TK_CHAR:
    case TK_TRUE:
    case TK_FALSE: {
      p_advance(P);
      vt_ast_node* n = N(&P->arena, AST_EXPR, t);
      n->as.expr.op = *t;
      return n;
    }
    case TK_LPAREN: {
      p_advance(P);
      vt_ast_node* first = parse_expr(P);
      if (p_accept(P, TK_COMMA)) {
        /* tuple literal */
        vec_node xs = {0};
        vec_node_push(&P->arena, &xs, first);
        for (;;) {
          vt_ast_node* e = parse_expr(P);
          vec_node_push(&P->arena, &xs, e);
          if (!p_accept(P, TK_COMMA)) break;
        }
        p_expect(P, TK_RPAREN, "')'");
        vt_ast_node* n = N(&P->arena, AST_EXPR, t);
        n->as.expr.op = *t;
        n->as.expr.children = xs;
        return n;
      }
      p_expect(P, TK_RPAREN, "')'");
      return first;
    }
    case TK_LBRACK: { /* array literal [ a, b, ... ] */
      p_advance(P);
      vec_node xs = {0};
      if (!p_accept(P, TK_RBRACK)) {
        for (;;) {
          vt_ast_node* e = parse_expr(P);
          vec_node_push(&P->arena, &xs, e);
          if (p_accept(P, TK_COMMA)) {
            if (p_accept(P, TK_RBRACK)) break;
            continue;
          }
          p_expect(P, TK_RBRACK, "']'");
          break;
        }
      }
      vt_ast_node* n = N(&P->arena, AST_EXPR, t);
      n->as.expr.op = *t;
      n->as.expr.children = xs;
      return n;
    }
    case TK_IF: { /* if expr block else (block|if) */
      p_advance(P);
      vt_ast_node* cond = parse_expr(P);
      vt_ast_node* thenb = parse_block(P);
      vt_ast_node* elseb = NULL;
      if (p_accept(P, TK_ELSE)) {
        if (p_peek(P)->kind == TK_IF)
          elseb = parse_primary(P); /* else-if enchaîné */
        else
          elseb = parse_block(P);
      }
      vt_ast_node* n = N(&P->arena, AST_IF, t);
      n->as.if_expr.cond = cond;
      n->as.if_expr.thenb = thenb;
      n->as.if_expr.elseb = elseb;
      return n;
    }
    case TK_WHILE: {
      p_advance(P);
      vt_ast_node* cond = parse_expr(P);
      vt_ast_node* body = parse_block(P);
      vt_ast_node* n = N(&P->arena, AST_WHILE, t);
      n->as.while_expr.cond = cond;
      n->as.while_expr.body = body;
      return n;
    }
    case TK_FOR: {
      p_advance(P);
      p_expect(P, TK_IDENT, "itérateur");
      vt_token it = P->cur;
      p_expect(P, TK_IN, "'in'");
      vt_ast_node* range = parse_expr(P);
      vt_ast_node* body = parse_block(P);
      vt_ast_node* n = N(&P->arena, AST_FOR, t);
      n->as.for_expr.iter = it;
      n->as.for_expr.range = range;
      n->as.for_expr.body = body;
      return n;
    }
    case TK_MATCH: {
      p_advance(P);
      vt_ast_node* scr = parse_expr(P);
      p_expect(P, TK_LBRACE, "'{'");
      vec_node arms = {0};
      while (!p_accept(P, TK_RBRACE)) {
        /* pattern (minimal: '_' | literal | path) */
        vt_ast_node* pat = NULL;
        const vt_token* k = p_peek(P);
        if (k->kind == TK_UNDERSCORE || k->kind == TK_INT ||
            k->kind == TK_STR || k->kind == TK_TRUE || k->kind == TK_FALSE ||
            k->kind == TK_IDENT) {
          p_advance(P);
          vt_ast_node* e = N(&P->arena, AST_EXPR, k);
          e->as.expr.op = *k;
          pat = e;
        } else {
          diags_pushf(&P->diags, k, "pattern invalide");
          pat = N(&P->arena, AST_EXPR, k);
        }
        p_expect(P, TK_FATARROW, "'=>'");
        vt_ast_node* ex = parse_expr(P);
        p_expect(P, TK_SEMI, "';'");
        vt_ast_node* arm = N(&P->arena, AST_MATCH_ARM, &ex->at);
        arm->as.match_arm.pat = pat;
        arm->as.match_arm.expr = ex;
        vec_node_push(&P->arena, &arms, arm);
      }
      vt_ast_node* n = N(&P->arena, AST_MATCH, t);
      n->as.match_expr.scrut = scr;
      n->as.match_expr.arms = arms;
      return n;
    }
    case TK_RETURN: {
      p_advance(P);
      vt_ast_node* e = NULL;
      if (is_start_of_expr(p_peek(P)->kind)) e = parse_expr(P);
      vt_ast_node* n = N(&P->arena, AST_RETURN, t);
      n->as.ret_stmt.expr = e;
      return n;
    }
    case TK_BREAK: {
      p_advance(P);
      return N(&P->arena, AST_BREAK, t);
    }
    case TK_CONTINUE: {
      p_advance(P);
      return N(&P->arena, AST_CONTINUE, t);
    }
    default:
      diags_pushf(&P->diags, t, "expression attendue");
      p_advance(P);
      return N(&P->arena, AST_EXPR, t);
  }
}

static vt_ast_node* parse_unary(parser* P) {
  const vt_token* t = p_peek(P);
  if (t->kind == TK_BANG || t->kind == TK_MINUS || t->kind == TK_AMP) {
    p_advance(P);
    vt_ast_node* rhs = parse_unary(P);
    vt_ast_node* n = N(&P->arena, AST_EXPR, t);
    n->as.expr.op = *t;
    vec_node ch = {0};
    vec_node_push(&P->arena, &ch, rhs);
    n->as.expr.children = ch;
    return n;
  }
  return parse_primary(P);
}

static vt_ast_node* parse_postfix(parser* P) {
  vt_ast_node* lhs = parse_unary(P);
  for (;;) {
    const vt_token* t = p_peek(P);
    if (t->kind == TK_LPAREN) {
      /* appel */
      vt_token at = *t;
      p_advance(P);
      vec_node args = {0};
      if (!p_accept(P, TK_RPAREN)) {
        for (;;) {
          vt_ast_node* e = parse_expr(P);
          vec_node_push(&P->arena, &args, e);
          if (p_accept(P, TK_COMMA)) continue;
          p_expect(P, TK_RPAREN, "')'");
          break;
        }
      }
      vt_ast_node* call = N(&P->arena, AST_EXPR, &at);
      call->as.expr.op = at;
      vec_node ch = {0};
      vec_node_push(&P->arena, &ch, lhs);
      /* push args */
      for (size_t i = 0; i < args.len; i++)
        vec_node_push(&P->arena, &ch, args.data[i]);
      call->as.expr.children = ch;
      lhs = call;
      continue;
    }
    if (t->kind == TK_DOT) {
      vt_token at = *t;
      p_advance(P);
      p_expect(P, TK_IDENT, "nom de champ/méthode");
      vt_token id = P->cur;
      /* méthode si prochaine est '(' */
      if (p_accept(P, TK_LPAREN)) {
        vec_node args = {0};
        if (!p_accept(P, TK_RPAREN)) {
          for (;;) {
            vt_ast_node* e = parse_expr(P);
            vec_node_push(&P->arena, &args, e);
            if (p_accept(P, TK_COMMA)) continue;
            p_expect(P, TK_RPAREN, "')'");
            break;
          }
        }
        vt_ast_node* meth = N(&P->arena, AST_EXPR, &id);
        meth->as.expr.op = id; /* nom méthode */
        vec_node ch = {0};
        vec_node_push(&P->arena, &ch, lhs);
        for (size_t i = 0; i < args.len; i++)
          vec_node_push(&P->arena, &ch, args.data[i]);
        meth->as.expr.children = ch;
        lhs = meth;
      } else {
        vt_ast_node* field = N(&P->arena, AST_EXPR, &id);
        field->as.expr.op = id;
        vec_node ch = {0};
        vec_node_push(&P->arena, &ch, lhs);
        field->as.expr.children = ch;
        lhs = field;
      }
      continue;
    }
    if (t->kind == TK_LBRACK) {
      vt_token at = *t;
      p_advance(P);
      if (p_accept(P, TK_COLON)) { /* [:hi] */
        vt_ast_node* hi = NULL;
        if (is_start_of_expr(p_peek(P)->kind)) hi = parse_expr(P);
        p_expect(P, TK_RBRACK, "']'");
        vt_ast_node* sl = N(&P->arena, AST_EXPR, &at);
        sl->as.expr.op = at;
        vec_node ch = {0};
        vec_node_push(&P->arena, &ch, lhs);
        if (hi) vec_node_push(&P->arena, &ch, hi);
        sl->as.expr.children = ch;
        lhs = sl;
        continue;
      }
      vt_ast_node* i = parse_expr(P);
      if (p_accept(P, TK_COLON)) { /* [lo:hi] */
        vt_ast_node* hi = NULL;
        if (is_start_of_expr(p_peek(P)->kind)) hi = parse_expr(P);
        p_expect(P, TK_RBRACK, "']'");
        vt_ast_node* sl = N(&P->arena, AST_EXPR, &at);
        sl->as.expr.op = at;
        vec_node ch = {0};
        vec_node_push(&P->arena, &ch, lhs);
        vec_node_push(&P->arena, &ch, i);
        if (hi) vec_node_push(&P->arena, &ch, hi);
        sl->as.expr.children = ch;
        lhs = sl;
        continue;
      }
      p_expect(P, TK_RBRACK, "']'");
      vt_ast_node* idx = N(&P->arena, AST_EXPR, &at);
      idx->as.expr.op = at;
      vec_node ch = {0};
      vec_node_push(&P->arena, &ch, lhs);
      vec_node_push(&P->arena, &ch, i);
      idx->as.expr.children = ch;
      lhs = idx;
      continue;
    }
    if (t->kind == TK_RANGE || t->kind == TK_RANGEEQ) {
      vt_token op = *t;
      p_advance(P);
      vt_ast_node* rhs = parse_expr(P);
      vt_ast_node* rng = N(&P->arena, AST_EXPR, &op);
      rng->as.expr.op = op;
      vec_node ch = {0};
      vec_node_push(&P->arena, &ch, lhs);
      vec_node_push(&P->arena, &ch, rhs);
      rng->as.expr.children = ch;
      lhs = rng;
      continue;
    }
    break;
  }
  return lhs;
}

static vt_ast_node* parse_bin_rhs(parser* P, prec_t min_prec,
                                  vt_ast_node* lhs) {
  for (;;) {
    const vt_token* t = p_peek(P);
    bool right_assoc = false;
    prec_t p = infix_prec(t->kind, &right_assoc);
    if (p < min_prec || p == PREC_POSTFIX) break;
    vt_token op = *t;
    p_advance(P);
    prec_t next_min = right_assoc ? (prec_t)(p) : (prec_t)(p + 1);
    vt_ast_node* rhs = parse_postfix(P);
    rhs = parse_bin_rhs(P, next_min, rhs);
    vt_ast_node* node = N(&P->arena, AST_EXPR, &op);
    node->as.expr.op = op;
    vec_node ch = {0};
    vec_node_push(&P->arena, &ch, lhs);
    vec_node_push(&P->arena, &ch, rhs);
    node->as.expr.children = ch;
    lhs = node;
  }
  return lhs;
}

static vt_ast_node* parse_assign_chain(parser* P) {
  /* assignment is right-associative */
  vt_ast_node* lhs = parse_postfix(P);
  const vt_token* t = p_peek(P);
  bool ra = false;
  prec_t p = infix_prec(t->kind, &ra);
  if (p == PREC_ASSIGN) {
    vt_token op = *t;
    p_advance(P);
    vt_ast_node* rhs = parse_assign_chain(P);
    vt_ast_node* node = N(&P->arena, AST_EXPR, &op);
    node->as.expr.op = op;
    vec_node ch = {0};
    vec_node_push(&P->arena, &ch, lhs);
    vec_node_push(&P->arena, &ch, rhs);
    node->as.expr.children = ch;
    return node;
  }
  return lhs;
}

static vt_ast_node* parse_expr(parser* P) {
  vt_ast_node* lhs = parse_assign_chain(P);
  /* binaire classiques après postfix but before low? handle via parse_bin_rhs
   * from lowest */
  lhs = parse_bin_rhs(P, PREC_OR, lhs); /* OR and above; assign déjà traité */
  return lhs;
}

/* ---------------------------------------------------------------------------
   Blocks & Statements
--------------------------------------------------------------------------- */
static vt_ast_node* parse_block(parser* P) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_LBRACE, "'{'");
  vec_node ss = {0};
  while (!p_accept(P, TK_RBRACE)) {
    vt_ast_node* s = parse_stmt(P);
    if (s)
      vec_node_push(&P->arena, &ss, s);
    else {
      const vt_tok_kind follow[] = {TK_SEMI, TK_RBRACE, TK_EOF};
      p_sync_to(P, follow, 3);
      p_accept(P, TK_SEMI);
    }
  }
  vt_ast_node* b = N(&P->arena, AST_BLOCK, &at);
  b->as.block.stmts = ss;
  return b;
}

static vt_ast_node* parse_let(parser* P) {
  vt_token at = *p_peek(P);
  p_expect(P, TK_LET, "'let'");
  bool is_mut = p_accept(P, TK_MUT);
  p_expect(P, TK_IDENT, "nom");
  vt_token name = P->cur;
  vt_ast_node* ty = NULL;
  if (p_accept(P, TK_COLON)) ty = parse_type(P);
  p_expect(P, TK_ASSIGN, "'='");
  vt_ast_node* init = parse_expr(P);
  p_expect(P, TK_SEMI, "';'");
  vt_ast_node* n = N(&P->arena, AST_LET, &at);
  n->as.let_stmt.is_mut = is_mut;
  n->as.let_stmt.name = name;
  n->as.let_stmt.ty = ty;
  n->as.let_stmt.init = init;
  return n;
}

static vt_ast_node* parse_stmt(parser* P) {
  const vt_token* t = p_peek(P);
  switch (t->kind) {
    case TK_LET:
      return parse_let(P);
    case TK_RETURN: {
      vt_ast_node* r = parse_primary(P);
      p_expect(P, TK_SEMI, "';'");
      return r;
    }
    case TK_BREAK:
    case TK_CONTINUE: {
      vt_ast_node* s = parse_primary(P);
      p_expect(P, TK_SEMI, "';'");
      return s;
    }
    default: {
      vt_ast_node* e = parse_expr(P);
      p_expect(P, TK_SEMI, "';'");
      vt_ast_node* n = N(&P->arena, AST_STMT_EXPR, &e->at);
      n->as.expr_stmt.expr = e;
      return n;
    }
  }
}

/* ---------------------------------------------------------------------------
   Program / module
--------------------------------------------------------------------------- */
static vt_ast_node* parse_program(parser* P) {
  vt_token at = *p_peek(P);
  vt_ast_node* mod = N(&P->arena, AST_MODULE, &at);
  mod->as.module.items.len = mod->as.module.items.cap = 0;
  mod->as.module.items.data = NULL;
  /* optional module decl: module path ; | module path { items } */
  if (p_accept(P, TK_MODULE)) {
    /* path */
    mod->as.module.name_path = *p_peek(P);
    parse_path_as_token(P, "chemin de module");
    if (p_accept(P, TK_SEMI)) {
      /* then top-level items follow */
    } else {
      p_expect(P, TK_LBRACE, "'{'");
      while (!p_accept(P, TK_RBRACE)) {
        vt_ast_node* it = parse_item(P);
        if (it)
          vec_node_push(&P->arena, &mod->as.module.items, it);
        else {
          const vt_tok_kind follow[] = {TK_RBRACE, TK_EOF};
          p_sync_to(P, follow, 2);
          p_accept(P, TK_RBRACE);
          break;
        }
      }
      return mod;
    }
  }
  /* otherwise: { item }* EOF */
  while (p_peek(P)->kind != TK_EOF) {
    vt_ast_node* it = parse_item(P);
    if (it)
      vec_node_push(&P->arena, &mod->as.module.items, it);
    else {
      const vt_tok_kind follow[] = {TK_EOF};
      p_sync_to(P, follow, 1);
      break;
    }
  }
  return mod;
}

/* ---------------------------------------------------------------------------
   API externe
--------------------------------------------------------------------------- */
typedef struct vt_parse_result {
  vt_ast_node* module;
  const vt_diag* diags;
  size_t ndiags;
  parser_arena_t* arena; /* à libérer via vt_parse_free() si non connectée */
} vt_parse_result;

static vt_parse_result vt_parse_internal_from_lexer(vt_lexer* lx) {
  parser P = {0};
  PAR_ARENA_INIT(&P.arena);
  P.arena_inited = true;
  diags_init(&P.diags, &P.arena);
  P.lx = *lx;
  P.own_lexer = false;
  P.has_nxt = false;
  memset(&P.cur, 0, sizeof(P.cur));
  vt_ast_node* root = parse_program(&P);
  vt_parse_result R = {0};
  R.module = root;
  R.diags = P.diags.v;
  R.ndiags = P.diags.len;
  R.arena = &P.arena;
  return R;
}

vt_parse_result vt_parse_source(const char* src, const char* filename) {
  parser P = {0};
  PAR_ARENA_INIT(&P.arena);
  P.arena_inited = true;
  diags_init(&P.diags, &P.arena);
  p_init_lexer_from_source(&P, src, filename ? filename : "<memory>");
  vt_ast_node* root = parse_program(&P);
  vt_parse_result R = {0};
  R.module = root;
  R.diags = P.diags.v;
  R.ndiags = P.diags.len;
  R.arena = &P.arena;
  p_destroy_lexer(&P);
  return R;
}

vt_parse_result vt_parse_file(const char* path) {
  parser P = {0};
  PAR_ARENA_INIT(&P.arena);
  P.arena_inited = true;
  diags_init(&P.diags, &P.arena);
  p_init_lexer_from_file(&P, path);
  vt_ast_node* root = parse_program(&P);
  vt_parse_result R = {0};
  R.module = root;
  R.diags = P.diags.v;
  R.ndiags = P.diags.len;
  R.arena = &P.arena;
  p_destroy_lexer(&P);
  return R;
}

void vt_parse_free(vt_parse_result* R) {
  if (!R || !R->arena) return;
  PAR_ARENA_FREE(R->arena);
  memset(R, 0, sizeof(*R));
}

/* ---------------------------------------------------------------------------
   Dump AST (debug)
--------------------------------------------------------------------------- */
static void dump_indent(FILE* f, int n) {
  while (n-- > 0) fputc(' ', f);
}
static void dump_token(FILE* f, const vt_token* t) {
  fprintf(f, "%.*s", (int)t->len, t->lexeme ? t->lexeme : "");
}
static void dump_expr(FILE* f, vt_ast_node* n, int ind);

static void dump_list(FILE* f, const char* tag, vec_node* v, int ind) {
  dump_indent(f, ind);
  fprintf(f, "%s[%zu]\n", tag, v->len);
  for (size_t i = 0; i < v->len; i++) dump_expr(f, v->data[i], ind + 2);
}
static void dump_expr(FILE* f, vt_ast_node* n, int ind) {
  if (!n) {
    dump_indent(f, ind);
    fprintf(f, "(null)\n");
    return;
  }
  switch (n->kind) {
    case AST_MODULE:
      dump_indent(f, ind);
      fprintf(f, "MODULE\n");
      dump_list(f, "items", &n->as.module.items, ind + 2);
      break;
    case AST_USE:
      dump_indent(f, ind);
      fprintf(f, "USE ");
      dump_token(f, &n->as.use_decl.path);
      if (n->as.use_decl.alias.len) {
        fprintf(f, " as ");
        dump_token(f, &n->as.use_decl.alias);
      }
      fprintf(f, "\n");
      break;
    case AST_IMPORT:
      dump_indent(f, ind);
      fprintf(f, "IMPORT ");
      dump_token(f, &n->as.import_decl.path);
      fprintf(f, "\n");
      break;
    case AST_MOD:
      dump_indent(f, ind);
      fprintf(f, "MOD ");
      dump_token(f, &n->as.mod_decl.name);
      fprintf(f, "\n");
      if (n->as.mod_decl.inline_body)
        dump_list(f, "mod_items", &n->as.mod_decl.items, ind + 2);
      break;
    case AST_CONST:
      dump_indent(f, ind);
      fprintf(f, "CONST ");
      dump_token(f, &n->as.const_decl.name);
      fprintf(f, "\n");
      dump_expr(f, n->as.const_decl.type, ind + 2);
      dump_expr(f, n->as.const_decl.value, ind + 2);
      break;
    case AST_TYPEALIAS:
      dump_indent(f, ind);
      fprintf(f, "TYPE ");
      dump_token(f, &n->as.type_alias.name);
      fprintf(f, "\n");
      dump_expr(f, n->as.type_alias.aliased, ind + 2);
      break;
    case AST_STRUCT:
      dump_indent(f, ind);
      fprintf(f, "STRUCT ");
      dump_token(f, &n->as.struct_decl.name);
      fprintf(f, "\n");
      for (size_t i = 0; i < n->as.struct_decl.fields.len; i++)
        dump_expr(f, n->as.struct_decl.fields.data[i], ind + 2);
      break;
    case AST_FIELD:
      dump_indent(f, ind);
      fprintf(f, "FIELD ");
      dump_token(f, &n->as.field.name);
      fprintf(f, "\n");
      dump_expr(f, n->as.field.type, ind + 2);
      break;
    case AST_FN:
      dump_indent(f, ind);
      fprintf(f, "FN ");
      dump_token(f, &n->as.fn_decl.name);
      fprintf(f, "\n");
      dump_list(f, "params", &n->as.fn_decl.params, ind + 2);
      dump_expr(f, n->as.fn_decl.ret, ind + 2);
      dump_expr(f, n->as.fn_decl.body, ind + 2);
      break;
    case AST_PARAM:
      dump_indent(f, ind);
      fprintf(f, "PARAM ");
      dump_token(f, &n->as.param.name);
      fprintf(f, "\n");
      dump_expr(f, n->as.param.type, ind + 2);
      break;
    case AST_IMPL:
      dump_indent(f, ind);
      fprintf(f, "IMPL\n");
      dump_expr(f, n->as.impl_block.ty, ind + 2);
      dump_list(f, "impl_items", &n->as.impl_block.items, ind + 2);
      break;
    case AST_TEST:
      dump_indent(f, ind);
      fprintf(f, "TEST ");
      dump_token(f, &n->as.test_block.name);
      fprintf(f, "\n");
      dump_expr(f, n->as.test_block.body, ind + 2);
      break;
    case AST_BLOCK:
      dump_indent(f, ind);
      fprintf(f, "BLOCK\n");
      dump_list(f, "stmts", &n->as.block.stmts, ind + 2);
      break;
    case AST_LET:
      dump_indent(f, ind);
      fprintf(f, "LET ");
      dump_token(f, &n->as.let_stmt.name);
      fprintf(f, "\n");
      dump_expr(f, n->as.let_stmt.ty, ind + 2);
      dump_expr(f, n->as.let_stmt.init, ind + 2);
      break;
    case AST_STMT_EXPR:
      dump_indent(f, ind);
      fprintf(f, "EXPR_STMT\n");
      dump_expr(f, n->as.expr_stmt.expr, ind + 2);
      break;
    case AST_RETURN:
      dump_indent(f, ind);
      fprintf(f, "RETURN\n");
      dump_expr(f, n->as.ret_stmt.expr, ind + 2);
      break;
    case AST_BREAK:
      dump_indent(f, ind);
      fprintf(f, "BREAK\n");
      break;
    case AST_CONTINUE:
      dump_indent(f, ind);
      fprintf(f, "CONTINUE\n");
      break;
    case AST_IF:
      dump_indent(f, ind);
      fprintf(f, "IF\n");
      dump_expr(f, n->as.if_expr.cond, ind + 2);
      dump_expr(f, n->as.if_expr.thenb, ind + 2);
      dump_expr(f, n->as.if_expr.elseb, ind + 2);
      break;
    case AST_WHILE:
      dump_indent(f, ind);
      fprintf(f, "WHILE\n");
      dump_expr(f, n->as.while_expr.cond, ind + 2);
      dump_expr(f, n->as.while_expr.body, ind + 2);
      break;
    case AST_FOR:
      dump_indent(f, ind);
      fprintf(f, "FOR ");
      dump_token(f, &n->as.for_expr.iter);
      fprintf(f, "\n");
      dump_expr(f, n->as.for_expr.range, ind + 2);
      dump_expr(f, n->as.for_expr.body, ind + 2);
      break;
    case AST_MATCH:
      dump_indent(f, ind);
      fprintf(f, "MATCH\n");
      dump_expr(f, n->as.match_expr.scrut, ind + 2);
      for (size_t i = 0; i < n->as.match_expr.arms.len; i++)
        dump_expr(f, n->as.match_expr.arms.data[i], ind + 2);
      break;
    case AST_MATCH_ARM:
      dump_indent(f, ind);
      fprintf(f, "ARM\n");
      dump_expr(f, n->as.match_arm.pat, ind + 2);
      dump_expr(f, n->as.match_arm.expr, ind + 2);
      break;
    case AST_TYPE:
      dump_indent(f, ind);
      fprintf(f, "TYPE ");
      dump_token(f, &n->as.type.tag);
      fprintf(f, "\n");
      dump_list(f, "children", &n->as.type.children, ind + 2);
      break;
    case AST_EXPR:
      dump_indent(f, ind);
      fprintf(f, "EXPR ");
      dump_token(f, &n->as.expr.op);
      fprintf(f, "\n");
      dump_list(f, "children", &n->as.expr.children, ind + 2);
      break;
  }
}

/* convenience helper for external debug */
void vt_ast_dump(FILE* f, vt_parse_result* R) {
  if (!f) f = stderr;
  if (!R || !R->module) {
    fprintf(f, "(no AST)\n");
    return;
  }
  dump_expr(f, R->module, 0);
  if (R->ndiags) {
    fprintf(f, "\nDiagnostics (%zu):\n", R->ndiags);
    for (size_t i = 0; i < R->ndiags; i++) {
      const vt_diag* d = &R->diags[i];
      fprintf(f, "%s:%d:%d: %s\n", d->file ? d->file : "<src>", d->line, d->col,
              d->msg ? d->msg : "(null)");
    }
  }
}

/* ---------------------------------------------------------------------------
   Fin
--------------------------------------------------------------------------- */
