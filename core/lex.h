// vitte-light/core/lex.h
// En-tête du lexer VitteLight (version compacte alignée sur core/lex.c)
// Implémentation: core/lex.c
//
// Fournit un lexer générique pour l'ASM VitteLight et autres DSL simples.
// API stable: types VL_TokenKind, VL_Token et fonctions vl_lex_*.
//
// Build utilisateur typique:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/lex.c
//   cc ... vos_fichiers.c core/lex.o -o binaire

#ifndef VITTE_LIGHT_CORE_LEX_H
#define VITTE_LIGHT_CORE_LEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>  // size_t

// ───────────────────── Kinds de tokens ─────────────────────
// Doivent rester en phase avec core/lex.c
enum {
  VL_TK_EOF = 0,
  VL_TK_NL = 1,
  VL_TK_ID = 2,
  VL_TK_INT = 3,
  VL_TK_FLOAT = 4,
  VL_TK_STRING = 5,
  VL_TK_PUNCT = 6,
  VL_TK_ARROW = 7
};

typedef int VL_TokenKind;

// ───────────────────── Représentation d'un token ─────────────────────
// Les chaînes retournées par VL_TK_STRING peuvent être allouées. Libérez-les
// via vl_tok_free(). Les autres unions (i64, f64, ch) sont valides selon le
// kind.

typedef struct VL_Token {
  VL_TokenKind kind;  // catégorie
  const char *start;  // pointeur dans le buffer source
  size_t len;         // longueur brute dans la source
  int line;           // 1-based
  int col;            // 1-based
  union {
    long long i64;
    double f64;
    char *str;
    int ch;
  } v;
  unsigned owns_str : 1;  // string allouée par le lexer
} VL_Token;

// ───────────────────── État du lexer (opaque côté utilisateur)
// ───────────────────── Délibérément opaque: utilisez uniquement les fonctions
// vl_lex_*. La taille concrète et les champs sont définis dans core/lex.c.

typedef struct VL_Lexer VL_Lexer;

// ───────────────────── API ─────────────────────
// Initialise un lexer sur (src,n). Ne copie pas le buffer, garde des pointeurs.
void vl_lex_init(VL_Lexer *lx, const char *src, size_t n);
// Retour des NEWLINE sous forme de VL_TK_NL si return_newlines!=0
void vl_lex_cfg(VL_Lexer *lx, int return_newlines);
// Regarde sans consommer le prochain token. Retourne 1 si succès.
int vl_lex_peek(VL_Lexer *lx, VL_Token *out);
// Consomme et retourne le prochain token.
VL_Token vl_lex_next(VL_Lexer *lx);
// Remet un token en lookahead (une case). No-op si la case est déjà occupée.
void vl_lex_unget(VL_Lexer *lx, const VL_Token *tk);
// Attend un token précis: soit un kind VL_TK_*, soit un caractère ponctuel.
int vl_lex_expect(VL_Lexer *lx, int kind_or_char, VL_Token *out);
// Dernier message d'erreur humainement lisible, ou NULL si aucune.
const char *vl_lex_err(const VL_Lexer *lx);
// Libère les ressources détenues par un token (ex: chaîne).
void vl_tok_free(VL_Token *tk);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_LEX_H
