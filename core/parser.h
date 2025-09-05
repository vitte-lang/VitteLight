/* ============================================================================
   parser.h — Analyseur syntaxique Vitte/Vitl (EBNF canonique 2025)
   API publique: construction d’AST, diagnostics, parsing depuis
   mémoire/fichier. C17, UTF-8, licence MIT.
   ============================================================================
 */
#ifndef VT_PARSER_H
#define VT_PARSER_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdio.h>  /* FILE */

#ifdef __cplusplus
extern "C" {
#endif

/* Marqueur d’include */
#define VT_PARSER_H_SENTINEL 1

/* ---------------------------------------------------------------------------
   Export
--------------------------------------------------------------------------- */
#ifndef VT_PARSER_API
#define VT_PARSER_API extern
#endif

/* ---------------------------------------------------------------------------
   Dépendance lexer (types de base)
--------------------------------------------------------------------------- */
#include "lex.h" /* attend VT_LEX_H_SENTINEL, vt_lexer/vt_token/vt_tok_kind */

/* ---------------------------------------------------------------------------
   Arène mémoire (opaque ou mem.h)
--------------------------------------------------------------------------- */
#ifdef VT_MEM_H_SENTINEL
#include "mem.h"
typedef vt_arena parser_arena_t;
#else
/* Opaque si mem.h absent */
struct parser_arena; /* tag forward */
typedef struct parser_arena parser_arena_t;
#endif

/* ---------------------------------------------------------------------------
   Diagnostics
--------------------------------------------------------------------------- */
typedef struct vt_diag {
  int line;         /* 1-based */
  int col;          /* 1-based (UTF-8 byte index) */
  const char* file; /* peut être NULL */
  char* msg;        /* UTF-8, stockée dans l’arène du parse */
} vt_diag;

/* ---------------------------------------------------------------------------
   AST — Représentation minimale publique
   (Les nœuds sont alloués en arène; ne pas free individuellement)
--------------------------------------------------------------------------- */
typedef struct vt_ast_node vt_ast_node;

typedef enum vt_ast_kind {
  AST_MODULE = 0,
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
  AST_EXPR, /* opérateurs/valeurs: children+op */
  AST_TYPE  /* formes de types: path/generic/ref/slice/array/tuple */
} vt_ast_kind;

typedef struct vec_node {
  vt_ast_node** data;
  size_t len;
  size_t cap;
} vec_node;

/* Nœud AST (même mise en page que parser.c) */
struct vt_ast_node {
  vt_ast_kind kind;
  vt_token at; /* position de référence (token source) */
  union {
    struct {
      vt_token name_path;
      vec_node items;
    } module;

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
      int inline_body;
    } mod_decl;

    struct {
      int is_pub;
      vt_token name;
      vt_ast_node* type;
      vt_ast_node* value;
    } const_decl;

    struct {
      int is_pub;
      vt_token name;
      vt_ast_node* aliased;
    } type_alias;

    struct {
      int is_pub;
      vt_token name;
      vec_node fields;
    } struct_decl;
    struct {
      vt_token name;
      vt_ast_node* type;
    } field;

    struct {
      int is_pub;
      vt_token name;
      vec_node params;
      vt_ast_node* ret;
      vt_ast_node* where;
      vt_ast_node* body;
    } fn_decl;
    struct {
      int is_mut;
      vt_token name;
      vt_ast_node* type;
    } param;

    struct {
      vt_ast_node* ty;
      vec_node items;
    } impl_block;

    struct {
      vt_token name;
      vt_ast_node* body;
    } test_block;

    struct {
      vec_node stmts;
    } block;
    struct {
      int is_mut;
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

    struct {
      vec_node children;
      vt_token tag;
    } type; /* TYPE: forme et sous-types */
    struct {
      vec_node children;
      vt_token op;
    } expr; /* EXPR: opérateur/valeur et enfants */
  } as;
};

/* ---------------------------------------------------------------------------
   Résultat de parsing
--------------------------------------------------------------------------- */
typedef struct vt_parse_result {
  vt_ast_node* module;   /* racine AST (module) */
  const vt_diag* diags;  /* diagnostics (dans l’arène) */
  size_t ndiags;         /* #diags */
  parser_arena_t* arena; /* interne; libérer via vt_parse_free() */
} vt_parse_result;

/* ---------------------------------------------------------------------------
   API
--------------------------------------------------------------------------- */

/* Parse depuis une chaîne UTF-8 (filename facultatif pour les messages) */
VT_PARSER_API vt_parse_result vt_parse_source(const char* source_utf8,
                                              const char* filename_opt);

/* Parse depuis un fichier (chemin UTF-8/locale) */
VT_PARSER_API vt_parse_result vt_parse_file(const char* path);

/* Libère l’arène associée au résultat (invalide les pointeurs dans result) */
VT_PARSER_API void vt_parse_free(vt_parse_result* result);

/* Dump lisible de l’AST + diagnostics (pour debug) */
VT_PARSER_API void vt_ast_dump(FILE* out, vt_parse_result* result);

/* (Optionnel) — si vous avez déjà un lexer initialisé:
   VT_PARSER_API vt_parse_result vt_parse_from_lexer(vt_lexer* lx);
   (Non exporté par défaut; exposer si parser.c le fournit)
*/

/* Helpers simples (inline) pour tester le genre d’un nœud */
static inline int vt_is_expr(const vt_ast_node* n) {
  return n && n->kind == AST_EXPR;
}
static inline int vt_is_type(const vt_ast_node* n) {
  return n && n->kind == AST_TYPE;
}
static inline int vt_is_block(const vt_ast_node* n) {
  return n && n->kind == AST_BLOCK;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_PARSER_H */
